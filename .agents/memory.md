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
