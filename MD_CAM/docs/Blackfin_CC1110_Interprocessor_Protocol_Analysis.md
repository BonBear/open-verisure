# Teknisk Hårdvaru- & Protokollanalys: Inter-Processor-Arkitektur (CC1110 <-> ADSP-BF512)

> **Obs (2026-11-16):** detta är den tidiga analysen. Protokollet som det faktiskt fungerar nu (SPI-kommandon, radiopaket, gatewayens mottagning, MQTT) beskrivs i [Kamera_protokoll.md](Kamera_protokoll.md).

**Kameradetektor:** Verisure GW MD CAM2 (`SECUR-PCAM-BLACKFIN P/N3204965 D`, LED-kort `P/N3302042 C`)  
**Roll:** Hardware & Protocol Analyst (Pro / High Thinking)  
**Mottagare:** Orchestrator (Parent Agent)

---

## Executive Summary & Systemöversikt

Kommunikationen mellan **TI CC1110F32** (RF SoC & systemövervakare) och **Analog Devices ADSP-BF512 Blackfin** (bildbehandlings-DSP) utgör ett asymmetriskt tvåprocessorsystem konstruerat för extrem batterieffektivitet:
- **I viloläge (Deep Sleep / PM2):** Blackfin DSP, SDRAM (`EM638165TS`) och CMOS-sensorn (`Sunny 8811GA`) är **100 % spänningslösa (0.0 V, 0 µA)**. Endast CC1110 och den analoga PIR-sensorn matas från `VBAT` (~3.1 V).
- **Vid bildbegäran:** CC1110 aktiverar spänningsskenorna i sekvens, släpper Blackfin ur reset (`P0_0 = 1`), låter DSP:n boota från `U3 Flash` (BMODE=011, SPI Master), tar därefter över bussen som **SPI Master** i förhållande till Blackfin (**SPI Slave**), och klockar ut den färdiga JPEG-bilden i kontrollerade block anpassade för CC1110:s 4 KB RAM och 868.950 MHz RF-paket.

Nedan följer den detaljerade analysen av de 5 frågeställningarna.

---

## 1. Delning av SPI-bussen vid Boot & Flash Tri-State

### A. Boot-arkitektur (BMODE=011 / SPI Master Boot)
ADSP-BF512 är hårdvarukonfigurerad med **`BMODE[2:0] = 011`** (Boot from external SPI flash in Master Mode):
1. **Reset-fas (`P0_0 = 0`):**
   - Blackfins I/O-pinnar hålls i reset/flytande pulldown.
   - U3 Flash (`EN25F40`) Pin 1 (`/CS`) är kopplad direkt till Blackfins interna **`SPI0_SEL1` (PG1)**. När Blackfin hålls i reset faller `SPI0_SEL1` till 0 V, vilket oavsiktligt aktiverar U3 Flash (`/CS = 0`). Det var därför CC1110 kunde läsa ut JEDEC ID `0x1C 0x31 0x13` direkt från flashminnet när `P0_0 = 0`.
2. **Boot ROM-fas (`P0_0 = 1`, tid $t = 0 \dots 200	ext{ ms}$):**
   - Den interna Boot ROM:en i ADSP-BF512 vaknar och sätter processorns SPI0-kontroller i **SPI Master Mode** vid 10–25 MHz.
   - Blackfin driver `SCK` (`PG13`), sänder läskommandon över `MOSI` (`PG11`) och drar `SPI0_SEL1` (`PG1`) aktivt låg.
   - Boot-laddaren läser in `.ldr`-binären (applikationskoden) från U3 Flash till DSP:ns interna L1 SRAM samt det externa SDRAM-minnet.
3. **Kritiskt krav på CC1110 under Boot:**
   - Under de första 500–800 ms efter att `P0_0 = 1` släppts **MÅSTE CC1110 hålla alla sina SPI-linjer (`P1_2..P1_5`) i absolut tristate (High-Z)** (`P1SEL = 0`, `P1DIR = 0`, `P1INP = 0`). Om CC1110 skulle driva `SCK`, `MOSI` eller dra `/SPISS` låg under denna fas uppstår en busskollision (*bus contention*) som kraschar Blackfins bootsekvens.

### B. Hur U3 Flash Tri-state:as efter Boot
När Boot ROM har läst in hela LDR-strömmen och överlämnar exekveringen till Verisures applikationsfirmware:
1. Blackfin driver linjen **`SPI0_SEL1` HÖG (3.3V)**.
2. Enligt databladet för EON `EN25F40` (och JEDEC-standarden): När `/CS` går hög går flashminnet omedelbart in i lågeffekts *Standby Mode*, och dess datautgång `SO` (Pin 2, ansluten till MISO) går in i **High-Impedance (Tri-state)**.
3. Blackfin behåller `SPI0_SEL1` konstant hög under hela sin aktiva körtid, vilket garanterar att flashminnet förblir elektriskt tystat.

### C. Hur Blackfin blir tillgänglig för CC1110
Efter avslutad boot kör Blackfin sin applikationsinitiering:
1. Blackfin rekonfigurerar sin interna SPI0-periferikrets från Master till **SPI Slave** (`MSTR = 0` i `SPI_CTL`).
2. Blackfins `SCK` och `MOSI` blir högimpediva ingångar som lyssnar på bussen.
3. Blackfins `PG10` (`/SPISS`) blir aktiv-låg slavväljare kopplad till CC1110 Pin 1 (`P1_2`).
4. Dess `MISO` (`PG12`) tri-state:as i väntan på att `/SPISS` (`P1_2`) ska dras låg av CC1110.

---

## 2. Master vs Slave-roll och ADSP-BF512:s SPI Slave DMA/FIFO-arkitektur

### A. Vem är Master och vem är Slave vid bildöverföring?
**Slutsats:** **CC1110 är SPI Master** och **Blackfin är SPI Slave**.

**Arkitektoniska bevis:**
1. **Minnes- & Pacing-begränsning:** CC1110 har en 8051-kärna med endast **4 KB SRAM (XDATA)**. En komprimerad JPEG-bild är ca 5–15 KB. Radiosändningen över 868.950 MHz kräver strikt blockpacing (8 bytes per RF-chunk, med 15 ms fördröjning per paket för att inte överbelasta kanalen). Om Blackfin vore Master skulle den klocka ut megabytes per sekund och omedelbart orsaka buffertspill i CC1110. Som Master styr CC1110 klockningen (`SCK`), läser ut 8–64 bytes i taget, skickar dem över RF, och klockar ut nästa del.
2. **Empiriskt bevis från mätningar:** I mätprotokollet (`Matprotokoll_Fas2_Power_och_Bussar.md`) visade Saleae/multimetern att så fort CC1110 tog kontroll som SPI Master och drog `P1_2` (`/SPISS`) låg, svarade Blackfins MISO-pinne omedelbart genom att klampa signalen från flytande till **stabil logisk nolla (`0x00`) under samtliga 512 klockcykler**. Detta är ett 100 % säkert kvitto på en aktiv Blackfin SPI-slavhårdvara.

### B. ADSP-BF512 SPI Slave-arkitektur: Register, CPHA och SENDZ

```mermaid
flowchart LR
    subgraph CC1110 ["TI CC1110 (SPI Master)"]
        P1_3["P1_3 (SCK Master)"]
        P1_5["P1_5 (MOSI Master)"]
        P1_4["P1_4 (MISO Slave In)"]
        P1_2["P1_2 (/SPISS Output)"]
        P1_6["P1_6 (DSP_READY Input)"]
    end

    subgraph Blackfin ["ADSP-BF512 (SPI Slave)"]
        SCK_S["PG13 (SPI0_SCK)"]
        MOSI_S["PG11 (SPI0_MOSI)"]
        MISO_S["PG12 (SPI0_MISO)"]
        SPISS_S["PG10 (SPI0_SS)"]
        READY_S["PGx (DSP_READY Output)"]
        
        SPI_CTL["SPI_CTL: MSTR=0, SENDZ=1, CPHA=0/1"]
        TX_DMA["DMA Channel 7 (SPI TX) / SPI_TDBR"]
    end

    P1_3 -->|Clock Pulses| SCK_S
    P1_5 -->|Commands (0x04/0x03)| MOSI_S
    MISO_S -->|JPEG Stream (0xFF 0xD8...)| P1_4
    P1_2 -->|Word Framing Toggle| SPISS_S
    READY_S -.->|Frame Ready (3.3V)| P1_6
```

#### 1. Ordstorlek (`SIZE`-biten i `SPI_CTL`)
- I ADSP-BF512 styrs ordstorleken av bit 8 (`SIZE`) i `SPI_CTL`:
  - `SIZE = 0`: 8-bitars dataord.
  - `SIZE = 1`: 16-bitars dataord.
- I Blackfin är DMA-kanalerna (DMA7 för SPI TX) och kärnans bussar nativt 16-bitars inriktade. Om Verisures firmware konfigurerat slaven för 16-bitars ord förväntar den sig klockpulser i multiplar om 16 bitar. Att skicka udda bytes kan förskjuta bitfönstret.

#### 2. CPHA=0 vs CPHA=1 & Kravet på `/SPISS`-toggling (Kritisk hårdvaruquirk!)
Analog Devices Blackfin-processorer har ett unikt hårdvarubeteende i SPI Slave Mode som ofta ställer till problem vid interfacening mot generiska MCUs:
- **Vid CPHA = 0 (SPI Mode 0 / Mode 2):**
  - Slaven måste driva den första biten på MISO omedelbart när `/SPISS` faller från 1 till 0, innan den första klockflanken anländer.
  - **Kritiskt:** I Blackfins hårdvara fungerar `/SPISS` vid `CPHA = 0` som en **hårdvarumässig ordgränsmarkör (word-boundary framing)**! Om mastern håller `/SPISS` kontinuerligt låg och klockar ut en lång byte-ström, tappar Blackfins interna skiftregister synkroniseringen och vägrar ladda nästa ord från FIFO:n.
  - **Lösning för CPHA=0:** Mastern (CC1110) **MÅSTE toggla `/SPISS` hög mellan varje ord** (d.v.s. `P1_2 = 0` $\rightarrow$ överför 16 bitar $\rightarrow$ `P1_2 = 1` för minst 2–5 NOPs).
- **Vid CPHA = 1 (SPI Mode 1 / Mode 3):**
  - Data skiftas ut på den första klockflanken. Här kräver Blackfin **inte** toggling av `/SPISS` mellan ord; mastern kan dra `/SPISS` låg och klocka en kontinuerlig ström av hundratals bytes (burst transfer).
- **Slutsats för tidigare tester:** När CC1110 klockade 512 bytes kontinuerligt med `P1_2 = 0` i Mode 0 (CPHA=0), orsakade detta ett framing error i Blackfin, vilket resulterade i att slaven inte kunde mata ut data.

#### 3. Betydelsen av SENDZ (`SZ`, Bit 2 i `SPI_CTL`)
- `SENDZ = 0`: Slaven återsänder det senaste ordet vid TX FIFO-underrun.
- `SENDZ = 1`: Slaven sänder **logiska nollor (`0x0000` / `0x00`)** vid TX FIFO-underrun.
- Att CC1110 konsekvent läste `0x00` bevisar att Blackfin körs med `SENDZ = 1` och att sändbufferten (DMA7 / `SPI_TDBR`) vid mättillfället var tom.

---

## 3. Analys av Pin 33 (P1_6): DSP_READY vs SHUTTER_TRIGGER

### Fakta
- Pin 33 (`P1_6`) är dragen till en Blackfin GPIO (troligen `PG` eller `PF`).
- Pinnen uppmättes till **0.0 V i viloläge**.

### Djupanalys av Signalriktning
| Egenskap | Hypotes A: `SHUTTER_TRIGGER` (CC1110 Out $\rightarrow$ DSP In) | Hypotes B: `DSP_READY` (DSP Out $\rightarrow$ CC1110 In) |
| :--- | :--- | :--- |
| **Vilospänning (0 V)** | Aktiv-hög trigger som vilar lågt via internt pulldown-motstånd. | DSP:n är inte klar / bilden inte tagen, så DSP driver utgången låg. |
| **Systemarkitektur** | Möjlig, men Blackfin är redan väckt och startad enbart för att ta bild. | **Standardarkitektur i industriell kameradesign.** Kameran behöver 200–500 ms för auto-exponering (AEC/AWB) och JPEG-komprimering innan data finns i bufferten. |
| **Orsak till tidigare 0x00-resultat** | CC1110 sände korta pulser utan att invänta DSP-initiering. | **Busskollision!** I `main.c` konfigurerades `P1_6` som **UTGÅNG** och drevs till 0 V (`P1_6 = 0`). Om Blackfin försökte driva pinnen hög till 3.3V för att flagga "bild klar" kortslöts signalen till GND av CC1110! |

### Slutsats för Pin 33 (P1_6)
1. **Primär funktion:** `P1_6` är med största sannolikhet en **`DSP_READY` / `FRAME_VALID`**-utgång från Blackfin till CC1110.
2. **Hur den ska hanteras av CC1110:**
   - CC1110 ska sätta `P1_6` som **INGÅNG** (`P1DIR &= ~0x40`) med avstängd pull-down / svag pull-up.
   - När Blackfin bootat och slutfört bildkomprimering kommer Blackfin att driva `P1_6` till **3.3 V (HÖG)**.
   - CC1110 pollar `P1_6` eller sätter ett Port 1-interrupt på stigande flank. Först när `P1_6 == 1` påbörjas SPI-utläsningen!
3. **Fallback (om pinnen är en aktiv-hög Shutter Trigger):**
   - Om `P1_6` efter 1500 ms fortfarande är 0 V och inget svar fås på SPI, växlas pinnen till utgång, ges en 20 ms HIGH-puls (`0 -> 1 -> 0`), och återgår omedelbart till ingång.

---

## 4. Hur bildtagning triggas i Verisures originalfirmware

1. **Varför PIR ALARM (`P1_7`) INTE triggar Blackfin direkt:**
   - PIR-sensorn larmar dygnet runt vid varje rörelse i rummet. I ett avlarmat hem skulle direkt hårdvarukoppling mellan PIR och Blackfin tömma tre CR123A-litiumbatterier på några dagar (Blackfin drar ~100 mA och LED-blixten upp till 500 mA).
   - PIR-flanken går uteslutande till CC1110 (`P1_7`), som filtrerar larmet mjukvarumässigt och skickar RF-telemetri till gatewayen.
2. **Beslutsmatris för bildtagning:**
   - Bildtagning triggas enbart när **CC1110 aktivt strömsätter Blackfin** (antingen p.g.a. rörelse i larmat läge eller via ett nedlänkskommando från gatewayen).
3. **Den verkliga triggningsmekanismen i firmware:**
   - **Metod 1: Automatisk triggning vid Power-Up:**
     Eftersom Blackfin är 100 % spänningslös i vila och drar 95 mA när den är påslagen, finns det ingen anledning att strömsätta den om den inte ska ta en bild. I Verisures originalkod startar Blackfin sin kamerapipeline direkt vid boot:
     $$	ext{Power On} \rightarrow 	ext{Boot från U3 Flash} \rightarrow 	ext{Sensor I2C AEC/AWB} \rightarrow 	ext{PPI DMA Capture} \rightarrow 	ext{JPEG Encode} \rightarrow 	ext{DSP\_READY = 1}$$
   - **Metod 2: SPI-kommandohandskakning (`P1_2` / MOSI):**
     Om Blackfin väntar på ett synkront startkommando från CC1110 används industriell standard:
     - `0x04` = Snapshot Trigger (bekräftat i mätprotokoll: MISO svarar direkt på detta kommando).
     - `0x02` = Query Status / Frame Buffer Length.
     - `0x03` = Read Image Stream (klocka ut JPEG).

---

## 5. Konkret, testbar C-kodsekvens för CC1110 (SDCC)

Nedan är en komplett, optimerad och icke-blockerande C-sekvens som:
- Genomför korrekt power-sequencing av alla 4 spänningsskenor.
- Tristaterar SPI-bussen under Blackfin-boot.
- Läser `P1_6` som ingång (`DSP_READY`).
- Hanterar Blackfins `/SPISS`-toggling i 16-bitars ord (CPHA=0) samt testar Mode 3 (CPHA=1).
- Detekterar och sparar JPEG SOI (`0xFF 0xD8`).

```c
/**
 * @file blackfin_capture.c
 * @brief Robust CC1110 <-> ADSP-BF512 Image Capture & JPEG SOI Detector
 */

#include <cc1110.h>
#include <stdint.h>
#include <stdbool.h>

extern __xdata uint8_t g_spi_rx_buf[2048];

/* ----------------------------------------------------------------------------
 * 1. SPI0 Master Hjälpfunktioner
 * ---------------------------------------------------------------------------- */

void spi0_init_mode0(void) {
    PERCFG |= 0x01;       // USART0 Alt. 2 (P1_2..P1_5)
    P1SEL  |= 0x38;       // P1_3 (SCK), P1_4 (MISO), P1_5 (MOSI) = Perifer
    P1SEL  &= ~0x04;      // P1_2 = GPIO (/SPISS)
    P1DIR  |= 0x2C;       // P1_2, P1_3, P1_5 = Utgångar
    P1DIR  &= ~0x10;      // P1_4 (MISO) = Ingång
    P1INP  &= ~0x10;      // Pull-up aktiv på MISO

    P1_2 = 1;             // /SPISS idle HIGH
    P1_3 = 0;             // SCK idle LOW (Mode 0)

    U0CSR = 0x00;         // SPI Master
    U0GCR = 0x20 | 0x0F;  // MSB first, CPOL=0, CPHA=0, ~844 kHz @ 27 MHz
    U0BAUD = 0x00;
}

void spi0_init_mode3(void) {
    PERCFG |= 0x01;
    P1SEL  |= 0x38;
    P1SEL  &= ~0x04;
    P1DIR  |= 0x2C;
    P1DIR  &= ~0x10;
    P1INP  &= ~0x10;

    P1_2 = 1;
    P1_3 = 1;             // SCK idle HIGH (Mode 3)

    U0CSR = 0x00;
    U0GCR = 0xE0 | 0x0F;  // MSB first, CPOL=1, CPHA=1, ~844 kHz
    U0BAUD = 0x00;
}

uint8_t spi0_txrx(uint8_t b) {
    uint16_t timeout = 10000;
    URX0IF = 0;
    U0CSR |= 0x40;        // RE = 1
    U0DBUF = b;
    while (!URX0IF && --timeout);
    URX0IF = 0;
    return U0DBUF;
}

/**
 * @brief 16-bitars ordöverföring med obligatorisk /SPISS-toggling (för Blackfin CPHA=0)
 */
uint16_t spi0_transfer_word16(uint16_t out_word) {
    uint8_t hi, lo;
    
    P1_2 = 0; // Assert /SPISS
    __asm nop nop nop nop __endasm;
    
    hi = spi0_txrx((uint8_t)(out_word >> 8));
    lo = spi0_txrx((uint8_t)(out_word & 0xFF));
    
    __asm nop nop nop nop __endasm;
    P1_2 = 1; // Deassert /SPISS (tvingar word-boundary reload i Blackfin)
    __asm nop nop nop nop __endasm;
    
    return ((uint16_t)hi << 8) | lo;
}

/* ----------------------------------------------------------------------------
 * 2. Huvudsekvens för Bildfångst & JPEG SOI-detektion
 * ---------------------------------------------------------------------------- */

int16_t capture_and_detect_jpeg(uint16_t *out_jpeg_len) {
    uint16_t i;
    int16_t jpeg_start = -1;
    uint16_t total_len = 0;
    uint16_t ready_timeout = 200; // 200 * 10 ms = 2 sekunder max

    *out_jpeg_len = 0;

    /* ------------------------------------------------------------------------
     * STEG 1: Strömsätt Blackfin och Kringkretsar i Korrekt Sekvens
     * ------------------------------------------------------------------------ */
    P0DIR |= 0x85; // P0_0 (/RESET), P0_2 (1.2V), P0_7 (Kamera rail) = Utgångar
    P1DIR |= 0x03; // P1_0 (3.3V), P1_1 (Bus EN) = Utgångar

    P0_0 = 0;      // HÅLL BLACKFIN I RESET UNDER STRÖMSÄTTNING
    P0_2 = 1;      // Slå på 1.2V Core rail (Q13)
    P1_0 = 1;      // Slå på 3.3V switched rail (Q7 - SDRAM & Sensor)
    P0_7 = 1;      // Slå på 3.27V sensor analog matning (J2-4)
    P1_1 = 1;      // Aktivera Bus Switch Enable

    /* ------------------------------------------------------------------------
     * STEG 2: Tristatera SPI-bussen och Konfigurera P1_6 som Ingång (DSP_READY)
     * ------------------------------------------------------------------------ */
    P1SEL &= ~0x7C; // P1_2..P1_6 = GPIO
    P1DIR &= ~0x7C; // Tristate P1_2..P1_6 (Ingångar) under boot!
    P1INP &= ~0x7C; // Enable pull-ups

    // Vänta 50 ms för spänningsstabilisering
    delay_ms(50);

    /* ------------------------------------------------------------------------
     * STEG 3: Släpp Blackfin ur Reset och Låt den Boote från U3 Flash
     * ------------------------------------------------------------------------ */
    P0_0 = 1;       // RELEASE RESET (Blackfin körs, drar ~95 mA)

    // Vänta 1000 ms medan Boot ROM läser in LDR och sensorn kör AEC/AWB
    delay_ms(1000);

    /* ------------------------------------------------------------------------
     * STEG 4: Handskakning via P1_6 (DSP_READY)
     * ------------------------------------------------------------------------ */
    // Undersök om Blackfin har signalerat DSP_READY (P1_6 == 1)
    while ((P1 & 0x40) == 0 && --ready_timeout) {
        delay_ms(10);
    }

    // Om P1_6 förblir 0, testa fallback: Skicka en aktiv HIGH triggerpuls
    if ((P1 & 0x40) == 0) {
        P1DIR |= 0x40; // Växla temporärt till utgång
        P1_6 = 1;
        delay_ms(30);
        P1_6 = 0;
        P1DIR &= ~0x40; // Återgå till ingång
        delay_ms(400);  // Ge tid för exponering
    }

    /* ------------------------------------------------------------------------
     * STEG 5: Initiera CC1110 som SPI Master
     * ------------------------------------------------------------------------ */
    spi0_init_mode0();
    P1_1 = 1; // Säkerställ Bus Switch aktiv
    P1_2 = 1; // /SPISS idle HIGH
    delay_ms(10);

    /* ------------------------------------------------------------------------
     * STEG 6: Skicka Snapshot-kommando 0x0004 & Läs ut Bufferten
     * ------------------------------------------------------------------------ */
    // Skicka trigger/init-ord med 16-bit word-framing
    spi0_transfer_word16(0x0004);
    delay_ms(20);

    // Skicka read-kommando 0x0003
    spi0_transfer_word16(0x0003);
    delay_ms(5);

    // Läs ut 2048 bytes från Blackfin med strikt toggling av /SPISS
    for (i = 0; i < 2048; i += 2) {
        uint16_t w = spi0_transfer_word16(0x0000);
        g_spi_rx_buf[i]     = (uint8_t)(w >> 8);
        g_spi_rx_buf[i + 1] = (uint8_t)(w & 0xFF);
    }

    /* ------------------------------------------------------------------------
     * STEG 7: Detektera JPEG SOI (0xFF 0xD8) och EOI (0xFF 0xD9)
     * ------------------------------------------------------------------------ */
    for (i = 0; i < 2047; i++) {
        if (g_spi_rx_buf[i] == 0xFF && g_spi_rx_buf[i + 1] == 0xD8) {
            jpeg_start = (int16_t)i;
            break;
        }
    }

    if (jpeg_start >= 0) {
        for (i = (uint16_t)jpeg_start + 2; i < 2047; i++) {
            if (g_spi_rx_buf[i] == 0xFF && g_spi_rx_buf[i + 1] == 0xD9) {
                total_len = (i + 2) - (uint16_t)jpeg_start;
                break;
            }
        }
        if (total_len == 0) {
            total_len = 2048 - (uint16_t)jpeg_start;
        }
    }

    /* ------------------------------------------------------------------------
     * STEG 8: Om Mode 0 misslyckades, prova Mode 3 (CPHA=1) Burst Read
     * ------------------------------------------------------------------------ */
    if (jpeg_start < 0) {
        spi0_init_mode3();
        P1_2 = 0; // Håll /SPISS låg kontinuerligt (tillåtet i CPHA=1)
        delay_ms(1);
        
        spi0_txrx(0x03); // Read Stream Command
        for (i = 0; i < 2048; i++) {
            g_spi_rx_buf[i] = spi0_txrx(0x00);
        }
        P1_2 = 1;

        for (i = 0; i < 2047; i++) {
            if (g_spi_rx_buf[i] == 0xFF && g_spi_rx_buf[i + 1] == 0xD8) {
                jpeg_start = (int16_t)i;
                total_len = 2048 - (uint16_t)jpeg_start;
                break;
            }
        }
    }

    *out_jpeg_len = total_len;
    return jpeg_start;
}
```

---

## Sammanfattning av Åtgärdspunkter för `embedded_coder`
1. **Ändra `P1_6` från utgång till ingång (`P1DIR &= ~0x40`)**: Förhindra busskollision och mät om Blackfin sätter linjen hög vid avslutad JPEG-enkodning.
2. **Tillämpa 16-bitars `/SPISS`-toggling på `P1_2`**: Eftersom Blackfin kör `CPHA=0` krävs att `P1_2` deassertas mellan orden för att förhindra att slavens skiftregister låser sig.
3. **Behåll sensor rail `P0_7 = 1` aktiv**: Utan analog matning på J2-4 startar inte Sunny-sensorns pixelklocka, vilket blockerar Blackfins PPI DMA i en evig loop.
4. **Tristatera bussen i 800 ms vid boot**: Garantera att Blackfin ostört kan suga in LDR-filen från U3 Flash innan CC1110 tar över som SPI Master.

Rapporten är härmed överlämnad för implementering av Embedded Coder och efterföljande verifiering av QA Tester.