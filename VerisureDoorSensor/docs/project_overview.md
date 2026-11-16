# Projektöversikt: Verisure CC1110 till ESP32-C3 (CC1101) RF-länk

Detta dokument är huvudindex för projektet. För att spara kontextfönster är den tekniska dokumentationen i `docs/` uppdelad i modulära filer. Läs endast in de underfiler eller bilder som behövs för den aktuella uppgiften.

---

## 1. Projektmål

1. **Sändare (Verisure-kort, TI CC1110F32):** Läsa av kortets sensorer (Reed-kontakt på `P1_7`, skaksensor på `P1_2`, knapp på `P2_0`) och sända ett 8-bytes paket (`"HB:ALIVE"`) på **868.950 MHz** (2-FSK, 38.4 kBaud, synkord `0xD3 0x91`, fast längd med hårdvaru-CRC).
2. **Mottagare (ESP32-C3 Super Mini #1 + CC1101):** Ta emot paketet via SPI, verifiera hårdvaru-CRC och logga payload, RSSI och frekvensoffset via USB Serial (`COM11`).
3. **Referensnod (ESP32-C3 Super Mini #2 + GERUI CC1101):** Identiskt kopplad ESP32-C3 + CC1101 som fungerar som referenssändare/mottagare.
4. **Автоmatiserad HIL-loop (Hardware-in-the-Loop):** Bygga med SDCC, flasha CC1110 direkt från terminalen via ESP32 CC-Flasher (`http://cc.local`) och verifiera resultat i realtid via ESP32-mottagarens serieport.

---

## 2. Nuläge & Verifierad Baslinje

* **ESP32 #2 $\rightarrow$ ESP32 #1 (100 % Verifierad):** Referenslänken mellan de två ESP32-C3-enheterna fungerar felfritt på 868.950 MHz och ger `MOTTAGET: "HB:ALIVE" | RSSI: -42 dBm | Offset: -4.8 kHz | CRC: OK` varannan sekund.
* **Verisure CC1110 $\rightarrow$ ESP32 #1 (100 % Verifierad med Telemetri):** 
  - **Kristall:** Verifierad till **27.000 MHz** (märkt "27.000" på X1). Radioregistren `FREQ2..0 = 0x202EEF` och `MDMCFG3 = 0x75` ger exakt 868.950 MHz och 38.38 kBaud.
  - **Sändning:** DMA Kanal 0 med Trigger 19 (`DMA_TRIG_RADIO`) mot `0xDFD9` (`X_RFD`) levererar kontinuerliga sändningar utan underflow.
  - **Länkprestanda:** Mottagaren loggar paketen med **`RSSI: -36 dBm`**, **`LQI: 0xFF` (255/255)** och **`CRC: OK`**!
  - **Händelsestyrd sändning & Telemetri:** Paketformat `VS:<Reed><Shock><Button><Seq>`:
    - **Omedelbar sändning vid trigger:** Så fort magnetkontakten (`P1_7`) ändrar läge eller skaksensorn (`P1_2`) registrerar en stöt avbryts vänteläget direkt och ett paket sänds omedelbart (< 2 ms latens).
    - **Röd LED (`P0_2`):** Lyser röd så länge magneten är öppen (`P1_7 == 1`, larmtillstånd) eller under stöt/vibrationsförlopp (hålls tänd 1000 ms). Släckt vid stängd dörr och lugnt läge.
    - **Grön LED (`P0_4`):** Ger en kort 40 ms puls vid varje aktiv radiosändning.
    - **Knapp (`SW1` på Pin 31 / `RESET_N`):** Hårdvarureset-knapp; rapporterar `'B'` under de första 5 paketen efter knapptryck.
  - **HIL CLI-loop:** Automatiserad med `python build_and_flash.py` (kompilerar med SDCC, laddar upp och flashar via ESP_CC_Flasher på `http://192.168.0.80`, och övervakar `COM11`).

---

## 3. Modulärt Filindex i `docs/` (Lazy-Loading Guide)

* **`verisure_door_sensor_guide.md`** $\rightarrow$ **Komplett huvuddokumentation:** Detaljerad genomgång av hårdvaran, pinouts, knapp (`SW1`), magnetkontakt, skaksensor, J2-kontakten för flashning, 27 MHz-kristallberäkningar, `FSCAL0` vs `PA_TABLE0`-fällan, DMA Trigger 19 och telemetriprotokollet (med inbäddade bilder).
* **`hardware_mapping.md`** $\rightarrow$ *Öppna när du arbetar med GPIO, sensorer (`P1_7` reed, `P1_2` shock, knapp via `RESET_N`), lysdioder (`P0_2`/`P0_4`), SPI-koppling på ESP32-C3 eller RF-hårdvaran (`U2` SAW-filter).*
* **`registers.md`** $\rightarrow$ *Öppna när du konfigurerar eller verifierar radioregistren (`FREQ`, `MDMCFG`, `DEVIATN`, `PKTCTRL`, `PA_TABLE0`).*
* **`cc1110_quirks_and_dma.md`** $\rightarrow$ *Öppna när du skriver sändningslogik i C för CC1110, sätter upp DMA Kanal 0 (Trigger 19), hanterar SDCC-kompilatorflaggor eller automatiserar flashning mot `http://cc.local`.*

### Bildfiler i `docs/` (Öppna vid behov av visuell hårdvaruverifiering)
* `pcb_bottom_mapped.png`: Översikt över baksidan med mappning av J2-kontakten, U1, batterianslutningar och sensorer.
* `fig_j2_connector_detail.jpg`: Detaljerad närbild på J2-flashningskontakten (Pin 1 kvadratisk padd).
* `fig_top_sensors_sw1.jpg`: Detaljerad närbild på ovansidan: batteriklämma, knapp `SW1` och reed-kontakt.
* `fig_cc1110_rf_crystal.jpg`: Närbild på CC1110, 27.000 MHz-kristallen `X1`, balun och RF-väg.
* `U2-1.jpeg` & `U2-2.jpeg`: Mikroskopbilder på RF-balun och passivt SAW-filter `U2` (EPCOS B3716, 868–870 MHz).
* `CC1110.jpeg`: Mikroskopbild på CC1110F32-chippet.
* `PXL_20261015_200824175.jpg` & `PXL_20261015_200955005~2.jpg`: Närbilder på 27.000 MHz-kristallen `X1`.
* `board-top.jpg`, `board-bottom-1.jpg`, `board-bottom-2.jpg`: Högupplösta originalfoton av kretskortet.