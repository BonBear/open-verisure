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
<!-- Lägg till nya verifierade lärdomar här under rätt kategori -->
