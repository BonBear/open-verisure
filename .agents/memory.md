# Projektminne & Kunskapsbank (Project Memory Bank)

Detta dokument fungerar som agenternas och utvecklarnas beständiga långtidsminne över sessioner.
Här sparas **verifierade tekniska insikter**, **hårdvarubegränsningar**, **kända fallgropar** och **arkitektoniska regler**.

---

## 📌 Minnesregler & Hygien (Memory Hygiene)
1. **Endast bekräftade fakta:** Spara aldrig hypoteser eller gissningar – endast lösningar och insikter som har testats och verifierats.
2. **Kompakt format:** Skriv korta, precisa bullet points:
   `- [ÅÅÅÅ-MM-DD] [OMRÅDE/KOMPONENT]: Konkret regel, begränsning eller lärdom.`
3. **Ingen loggdumpning:** Undvik hela källkodssnuttar eller råa stack traces. Beskriv *varför* och *hur*, inte dumpad rådata.

---

## ⚡ Hårdvara & RF (CC1110 / CC1101 / 868 MHz)
- [2026-10-22] [CC1110/RF]: RF-kärnan körs på 868.950 MHz med en 27 MHz-kristall (X1). Registerberäkningar för frekvens och baudrate måste baseras på 27 MHz, inte standard TI 24 MHz eller 26 MHz.
- [2026-10-22] [CC1110/DMA]: RFD FIFO måste hanteras via DMA-trigger 19 för att garantera att inga bytes tappas vid höga sändningshastigheter.
- [2026-10-22] [ESP32-S2/Coexistence]: Använd uteslutande ADC1-domänens pinnar för SPI-bussen till CC1101. Användning av ADC2-pinnar skapar högfrekvent switchbrus som slår ut ESP32-S2:s Wi-Fi-mottagare vid 2.4 GHz.
- [2026-10-22] [SENSOR/GPIO]: Reed-switchen sitter på P1_7, Shock/Vibration på P1_2, och Tamper på P0_1.
- [2026-10-24] [MD_CAM/FLASHING]: Programmeringsheadern på GW MD CAM2 (SECUR-PCAM-BLACKFIN) är J6: J6-2=DD (P2_1/Pin 15), J6-3=DC (P2_2/Pin 16), J6-4=RESET_N (Pin 31/SW1 via 2.7 kOhm), J6-5=GND. Anslut EJ 3V3 från ESP_CC_Flasher till VBAT (startströmmen till tantalkondensatorerna C94/C50/C3 sänker ESP32-LDO:n och tappar USB). Mata istället kortet med 1 batteri i hållaren (~3.1V) och koppla endast 4 trådar (GND, DD, DC, RST).
- [2026-10-24] [MD_CAM/RF]: GW MD CAM2 (Kort #1 = 2NCEKS3T upplåst, Kort #2 = 2NCEKT3X låst referens) använder 27.000 MHz kristall (Y1) och autentiserar direkt mot Gatewayen med FREQ=0x202EEF, MDMCFG4=0x8A, MDMCFG3=0x75 och FSCAL0=0x1F (RSSI -43 dBm, LQI 103). Sätt is_por_reset på både POR (rst_cause=0) och External Reset (rst_cause=1) så att Gatewayens replay-filter nollställs direkt efter omflashning/reset.
- [2026-10-24] [MD_CAM/GPIO]: PIR ALARM-signalen ligger på P1_7 (Pin 32, aktiv låg: 3.08V i vila, 0V vid rörelse). Endast SW3 är monterad på kortet och detekteras via dual Pull-Up/Pull-Down-skanning på P0 (mask 0xC1). När Blackfin-DSP:ns 3.3V-skena är avstängd (0V) MÅSTE P0INP=0x30 och P1INP=0x3F sättas (tristate på P0_4..P0_5 och P1_0..P1_5), annars läcker CC1110:s interna pull-up in i DSP:ns ESD-dioder (~0.6V) och skapar brus/falsklarm.
- [2026-10-23] [MD_CAM/BUS]: CC1110 pratar med Blackfin DSP (ADSP-BF512) och periferichip via SPI USART0 Alt. 2 (P1_3=SCK, P1_4=MISO, P1_5=MOSI, P1_2=SS1, P1_1=SS2) samt UART USART1 Alt. 1 (P0_4=TX, P0_5=RX). Spänningsskenorna 3.3V, 2.5V, 1.8V och 1.2V är helt avstängda (0V) i viloläge medan CC1110 och PIR matas direkt från VBAT (~3.1V).
- [2026-10-24] [CC1110/PM2]: Port 1-flank väljs med PICTL bit 1 (P1ICON = 0x02), INTE 0x04. Fel bit gjorde att PIR bara väckte på fel flank. PM2 kräver att CPU först byts till HS RCOSC (CLKCON.OSC=1) innan PCON.IDLE sätts; varje ISR ska nollställa SLEEP.MODE och PxIFG före PxIF.
- [2026-10-24] [MD_CAM/PM2]: Fungerande lågeffektdesign: PIR (P1_7) väcker direkt via Port 1-interrupt; Sleep Timer (WOR_RES=2, WOREVT0=16 = 0.5 s) väcker för att läsa SW3 via dual pull-scan mot boot-baslinjen (batteri isätts alltid med enheten utanför väggen = Tamper) och räkna ner heartbeat (7200 tick = 1 h). 27 MHz XOSC + radio slås bara på under sändning.
- [2026-10-29] [MD_CAM/TAMPER]: Sabotagebrytaren SW3 sitter på P1_6 (Pin 33), INTE P0_6! Den sluter mot VBAT+. Med Port 1 i pull-down läser P1_6=0 (0V) när kåpan är av (Tamper Alarm), och P1_6=1 (3.0V) när knappen trycks in mot väggen (Cover Secure). Tidigare troddes P1_6 vara en DSP-styrpinne vilket gjorde att intern pull-up lade 3V på pinnen.
- [2026-10-28] [MD_CAM/POWER]: Blackfin DSP (ADSP-BF512) spänningsmatning styrs av CC1110: P0_2=1 aktiverar 1.2V core via Q13, P1_0=1 aktiverar 3.3V switched rail via Q7, och P0_7=1 aktiverar auxiliary rail (J2-4). P0_0 är aktiv låg hårdvarureset (/RESET). Vid P0_0=0 drar kortet ~20 mA (eller 1 mA med alla rails av). Vid P0_0=1 och aktiva rails drar Blackfin stabilt 92-95 mA i aktiv körning.
- [2026-10-28] [MD_CAM/FLASH]: U3 SPI Flash (märkt "DSP V175") är ett EON cFeon EN25F40 4 Mbit (512 KB) flashminne med JEDEC ID 0x1C 0x31 0x13. Dess Chip Select (/CS, Pin 1) styrs direkt av Blackfins interna SPI0_SEL1 (inte CC1110:s SS-pinnar). När Blackfin hålls i reset (P0_0=0) dras U3 /CS låg och CC1110 kan läsa flashminnet via delade SPI-bussen (SCK=P1_3, MISO=P1_4, MOSI=P1_5). När Blackfin körs (P0_0=1) driver den SPI0_SEL1 hög så att U3 Flash tri-state:as i standby.
- [2026-10-28] [MD_CAM/SPI_HANDSHAKE]: Testpaddarna SS Övre (P1_2) och SS Undre (P1_1) är inter-processor-styrlinjer kopplade direkt till Blackfin DSP (/SPISS). Vid uppstart måste CC1110 tri-state:a P1_2..P1_5 så att Blackfin kan boota ostört från U3 Flash. Efter boot tar CC1110 över som SPI Master. Initieringskommandot 0x04 på P1_2 ger omedelbar respons på MISO (klampas från 0xFF till 0x00).
- [2026-10-28] [MD_CAM/BUS_SWITCH]: P1_1 fungerar som aktiv-hög Bus Switch Enable. När P1_1 = 1 är bussen helt öppen mot Blackfin och CC1110 kan klocka 4096 klockor kontinuerligt. När P1_1 = 0 klampas bussen låg.
- [2026-10-28] [MD_CAM/BLACKFIN_SPI]: Blackfin ADSP-BF512 kör stabilt vid 73–97 mA (P0_0 = 1). Dess SPI-slavhårdvara är aktiv och driver MISO till 0x00 (SENDZ = 1) under hela den 512 bytes långa utklockningen i väntan på att DSP-applikationen ska ladda sändbufferten. U3 Pin 2 piper direkt (0 ohm) mot CC1110 Pin 35 (MISO).
- [2026-10-28] [MD_CAM/FLASH_LED]: LED-blixtkortet (P/N3302042 C) har en röd indikator-LED som lyser när kortet strömsätts. Blixt-lysdioderna (4 st IR/högeffekts-LEDs) styrs via kombinerad logik (Blackfin/P0_7/ljussensor).

---

## 🔐 Krypto, Säkerhet & Protokoll
- [2026-10-22] [AES/KDF]: CC1110 använder sin inbyggda hårdvaru-coprocessor för AES-128. Enhetsnyckeln härleds via KDF baserat på enhetens serienummer och master-secret.
- [2026-10-22] [REPLAY]: Replay-skyddet bygger på en 16-bitars rullande sekvensräknare. Gatewayen måste förkasta alla paket vars räknare inte är strikt inkrementerande (med hänsyn till wrap-around vid 0xFFFF).
- [2026-10-22] [PAYLOAD]: Telemetripaketet är 24 bytes långt (inkluderar batterinivå VDD/3, kontakttillstånd och rullande sekvens).

---

## 🛠️ Byggsystem, Toolchains & Python
- [2026-10-22] [SDCC/8051]: CC1110 kompileras med SDCC. Undvik dynamisk allokering (`malloc`) helt på grund av 8051:ans begränsade XDATA/IDATA-ram.
- [2026-10-22] [PlatformIO]: ESP32-S2 Gateway kompileras med PlatformIO. Se till att inte störa captive portal- och MQTT-taskarna i FreeRTOS.
- [2026-10-22] [Python-verktyg]: Seriell portväljare är centraliserad i `port_selector.py` och ska alltid återanvändas av andra skript istället för att hårdkoda COM-portar.

---

## 📝 Nya Lärdomar (Fylls på löpande av agenterna)
- [2026-10-29] [MD_CAM/CAMERA_PIPELINE]: Komplett bildöverföring över 868.950 MHz Sub-1GHz RF till Home Assistant verifierad. CC1110 segmenterar JPEG-bilden i 8-bytes krypterade ramar (plain[2]=0x80), ESP32-S2 Gateway reassemblerar strömmen, validerar JPEG SOI (0xFF 0xD8) och EOI (0xFF 0xD9), och publicerar via MQTT till `camera.verisure_2nceks3t_camera`.
- [2026-10-29] [SDCC/MEMORY_MODELS]: I SDCC för 8051 (--model-large) får `__code`-pekare (ROM) inte passeras direkt till sändningsfunktioner som använder `MOVX` för XDATA utan explicit kopiering till en `__xdata`-buffert. Att läsa utanför XDATA in i SFR-adressrymden (0xF000+) hänger 8051-bussen.
- [2026-10-29] [CC1110/UART_ISR]: I CC1110 måste `U1DBUF` alltid tilldelas en `volatile uint8_t` i USART1 RX ISR (`urx1_isr`). Satser som `(void)U1DBUF;` optimeras bort av SDCC vilket gör att RX-avbrottsflaggan aldrig rensas av hårdvaran och orsakar en oändlig interrupt-storm.
- [2026-10-29] [MD_CAM/SHUTTER_BUTTON]: Sabotagebrytaren SW3 (P1_6) fungerar utmärkt som en fysisk kameraavtryckare. När knappen trycks in (P1_6=1, 3.0V) skickas ett triggningspaket (0xEE) och bildsekvensen avfyras med omedelbar RF-leverans till Home Assistant.
- [2026-10-29] [MD_CAM/PIR_INDICATOR_LED]: Den röda indikatorlysdioden LD1 sitter på PIR-dotterkortet (SECUR-PCAM BF-PIR P/N 3309012 A) och drivs av transistorn Q2. Om lediga pinnar (P0_1, P0_3, P0_6, P2_0) lämnas som standard inputs med pull-up hålls Q2 aktiverad och drar ~19 mA extra (totalt 22 mA). Genom att aktivt konfigurera och driva dessa pinnar låga (0.0 V) släcks LD1 och strömförbrukningen sjunker direkt till 3 mA i viloläge.
- [2026-10-29] [MD_CAM/P1_PULLUP]: Port 1 innehåller både PIR-sensorn (P1_7) och sabotagebrytaren SW3 (P1_6). Eftersom CC1110 har en gemensam pull-riktning för hela Port 1 (P2INP bit 6, PDUP1) får Port 1 ALDRIG sättas till pull-down under körning, då det drar P1_7 till 0V och orsakar kontinuerliga falska PIR-larm. Port 1 ska alltid hållas i Pull-UP läge (P2INP &= ~0x40).
- [2026-10-30] [MD_CAM/BOOT_CAPTURE_VERIFIED]: Mätningar på Referenskort #2 (låst) med Saleae Logic 4 fångade en fullständig 2.000 MHz SPI bootsekvens mot U3 Flash (EN25F40) vid t=42.286s (opcodes 0x05 RDSR, 0x9F RDID, 0x0B Fast Read på 0x000000, 0x020000, 0x040000, 0x07F000, 0x07E200, 0x07E000). Total bootduration är exakt 20.7 ms, varpå U3 /CS återgår till HIGH (Standby) och Blackfin kopplar om sin interna SPI0-kontroller från SPI Master till SPI Slave.
- [2026-10-30] [MD_CAM/BLACKFIN_BOOT_FIX]: För att Blackfin ADSP-BF512 ska kunna boota från U3 Flash på Kort #1 (där spänningsskenorna styrs via CC1110 Q13/Q7 istället för samtidig kallstart): P1_1 (Bus Switch Enable) MÅSTE drivas aktivt HÖG (1) och P1_2 (Blackfin /SPISS) MÅSTE hållas HÖG (1) under bootfasen för att förhindra SPI Mode Fault i DSP:n, medan P1_3..P1_5 (SCK, MISO, MOSI) hålls i äkta 3-state (P1INP |= 0x38).
- [2026-10-30] [MD_CAM/POWER_CURRENTS]: Verifierade strömnivåer på labbaggregat (COM5, 3.00 V): Viloläge med LD1 släckt drar exakt 3.0 mA. Under aktiv Blackfin-boot och exponering drar kortet 73.0–75.0 mA under ~1.5 sekunder och återgår därefter deterministiskt till 3.0 mA.
- [2026-10-30] [MD_CAM/UART_TELEMETRY]: Telemetri-opcode 0xDD implementerad i CC1110 för att rapportera antal mottagna bytes från Blackfin på UART1 (P0_5/RX) över Sub-1GHz RF till ESP32-S2 Gateway.
