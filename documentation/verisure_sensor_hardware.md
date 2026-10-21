# Verisure Sensorkort: Hårdvarukartläggning & Kiselanalys (TI CC1110F32)

Detta dokument dokumenterar den fullständiga fysiska och logiska hårdvarukartläggningen av Verisure trådlösa dörr- och fönstersensor (`SD-MAG-VIB`), baserad på Texas Instruments **CC1110F32** (SoC med 8051-kärna och integrerad Sub-1GHz radio).

---

## 📌 GPIO- och Periferimappning

| Komponent / Sensor | CC1110 Pin | I/O Riktning | Registerkonfiguration | Beteende & Polaritet |
| :--- | :---: | :---: | :--- | :--- |
| **Reed-kontakt** (Magnet) | **`P1_7`** (Pin 32) | Ingång | `P1SEL &= ~0x80; P1DIR &= ~0x80; P1INP &= ~0x80;` | Intern pull-up.<br>• **`0` = Magnet nära** (Dörr stängd / Säkrad)<br>• **`1` = Magnet borta** (Dörr öppen / Larm) |
| **Skaksensor** (Piezo/Vib) | **`P1_2`** (Pin 1) | Ingång | `P1SEL &= ~0x04; P1DIR &= ~0x04; P1INP &= ~0x04;` | Digital pulsingång som växlar snabbt vid vibrationer eller stötar. |
| **Sabotagekontakt** (Tamper)| **`P0_1`** (Pin 15) | Ingång | `P0SEL &= ~0x02; P0DIR &= ~0x02; P0INP &= ~0x02;` | Intern pull-up.<br>• **`0` = Locket PÅ** (knapp nedtryckt mot GND via 100 $\Omega$ ➔ Säkrad)<br>• **`1` = Locket AV** (knapp släppt / öppen ➔ SABOTAGE) |
| **Röd LED** (`LED_RED`) | **`P0_2`** (Pin 16) | Utgång | `P0SEL &= ~0x04; P0DIR \|= 0x04;` | Aktiv hög (`1` = tänd, `0` = släckt). Lyser vid öppen dörr, stöt eller sabotage. |
| **Grön LED** (`LED_GREEN`) | **`P0_4`** (Pin 18) | Utgång | `P0SEL &= ~0x10; P0DIR \|= 0x10;` | Aktiv hög (`1` = tänd, `0` = släckt). Ger en skarp 40 ms puls vid aktiv radiosändning. |
| **Debug Data (`DD`)** | **`P2_1`** (Pin 21) | I/O / Utgång | `P2SEL &= ~0x02; P2DIR \|= 0x02;` | Växlar (togglar) för varje sänt paket som mätpunkt för oscilloskop/analysator. |
| **Debug Clock (`DC`)** | **`P2_2`** (Pin 22) | Ingång / Utgång | `P2SEL &= ~0x04; P2DIR \|= 0x04;` | Sätts till `1` när radiopaketet framgångsrikt överförts till luften. |

---

## 🔍 Sabotageknappen (`P0_1`): Upptäckt och Verifiering

Genom att överföra realtidsdiagnostik från sensorns alla portar (`P0`, `P1`, `P2`) till gatewayens serieport upptäcktes exakt hur sabotagebrytaren är kopplad:
1. **Viloläge (locket av, knappen fri):**
   ```text
   Pins : P0=0xEB P1=0x7F P2=0x1D
   ```
   `0xEB = 1110 1011`$_2$ ➔ Bit 1 (`P0_1`) är `1`.
2. **Knappen nedtryckt (motsvarar stängt lock):**
   ```text
   Pins : P0=0xE9 P1=0x7F P2=0x1D
   ```
   `0xE9 = 1110 1001`$_2$ ➔ Bit 1 (`P0_1`) dras till `0` (GND via 100 $\Omega$).
3. **Slutsats:** Sabotagelarmet ska därför triggas när `P0_1 == 1` (locket avlägsnat), och återställas när `P0_1 == 0` (locket stängt).

---

## 📡 RF-hårdvara, SAW-filter och Kristallberäkningar

### 1. 27.000 MHz Kristallen (`X1`)
Till skillnad från standardreferensdesigner som använder 26 MHz är kristallen `X1` på Verisure-kortet lasergraverad med **`27.000`**. Att använda 26 MHz-matematik förskjuter bärvågen med ~33 MHz och gör signalen omöjlig att ta emot.

Formel för syntesfrekvens vid $f_{\text{xosc}} = 27.000\text{ MHz}$ och bärvåg $f_{\text{carrier}} = 868.950\text{ MHz}$:
$$\text{FREQ} = \frac{f_{\text{carrier}} \times 2^{16}}{f_{\text{xosc}}} = \frac{868.950 \times 10^6 \times 65536}{27.000 \times 10^6} = 2\,109\,167 = \text{0x202EEF}$$

* **`FREQ2`** = `0x20`
* **`FREQ1`** = `0x2E`
* **`FREQ0`** = `0xEF` (Ger 868.950027 MHz, ett teoretiskt fel på endast +27 Hz / 0.03 ppm!)

Formel för symbolhastighet vid 38.383 kBaud standard:
$$R_{\text{data}} = \frac{(256 + 117) \times 2^{10}}{2^{28}} \times 27.000\text{ MHz} = 38\,386.6\text{ Baud}$$
* **`MDMCFG4`** = `0x8A`
* **`MDMCFG3`** = `0x75`

### 2. SAW-filter (`U2`, märkt `3716`)
Mellan CC1110:s balun och spiralantennen sitter ett ytakustiskt bandpassfilter: **EPCOS/TDK B3716** (B39871B3716U410) med passband **868.00–870.00 MHz**. Det dämpar effektivt övertoner och blockerar störningar utanför 868 MHz ISM-bandet.

---

## ⚠️ Kritiska Fällor & Lösningar

### Fälla 1: SDCC-headerfelet på `PA_TABLE0` vs `FSCAL0`
I standard SDCC `<cc1110.h>` är `PA_TABLE0` felaktigt definierad som `0xDF26`.
* Adress `0xDF26` är i själva verket **`FSCAL0`** (PLL-syntesens kalibreringsregister).
* Om kod skriver `PA_TABLE0 = 0xC2` med standard-headern skrivs `0xC2` rakt in i `FSCAL0`, vilket slår ut frekvenssyntesen helt och radion sänder bara tystnad eller brus!
* **Rätt definition:**
  ```c
  #define TRUE_FSCAL0      (*((__xdata volatile uint8_t *)0xDF26))
  #define TRUE_PA_TABLE0   (*((__xdata volatile uint8_t *)0xDF2E))

  TRUE_FSCAL0    = 0x1F; // MÅSTE vara 0x1F för att PLL ska låsa!
  TRUE_PA_TABLE0 = 0xC2; // Sätter +10 dBm uteffekt på rätt adress 0xDF2E
  ```

### Fälla 2: TX FIFO Underflow och DMA Trigger 19
CC1110 har till skillnad från CC1101 **ingen 64-byte hårdvaru-FIFO**. Radioregistret `RFD` (`0xDFD9`) är endast **1 byte djupt**. Om CPU:n inte hinner mata varje byte innan RF-modulatorn tömt bufferten kastar hårdvaran `MARCSTATE = 0x16` (`TXFIFO_UNDERFLOW`).
* **Lösning:** Sändningen styrs via **DMA Kanal 0** med **Trigger 19 (`DMA_TRIG_RADIO`)**:
  ```c
  dma_desc.cfg0 = 0x13; // Trigger 19: RADIO
  dma_desc.cfg1 = 0x4A; // Hög prioritet, SRCINC=1, DESTINC=0
  ```
  DMA-kontrollern matar automatiskt `RFD` i takt med radions interna klocka helt utan CPU-inblandning.
