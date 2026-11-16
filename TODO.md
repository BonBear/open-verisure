# TODO

Två listor: det Claude kan göra helt själv, och det som behöver dig vid kameran eller ett
beslut. Bocka av med `[x]` och skriv resultatet kort efter punkten.

## A. Claude kan göra autonomt

- [x] **Visa blixtbilder på gatewayens webbsida.** Klart 2026-11-16: `/api/camera.jpg` visar den nyaste av färgbilden och blixtbilden (verifierat D1 -> färg, D4 -> svartvit, D1 -> färg).
- [x] **Utöka e2e-testet** (`MD_CAM/test_camera_e2e.py`). Klart 2026-11-16: kollar nu 56-byte-paket (antal paket = byte/40), D4 svartvit på MQTT, D3 efter D4 som delta, D5/D6 (färg i ljus, blixtbild i mörker). 11/11 godkända.
- [x] **Uppdatera protokolldokumentationen.** Klart 2026-11-16: nytt dokument `MD_CAM/docs/Kamera_protokoll.md` (SPI-kommandon 0xD0–0xD6, delta-format, radiopaket 24/56 byte, gatewayens 58-byte-mottagning, nedlänk, MQTT, kamerans tillstånd); hänvisning från den gamla analysen.
- [x] **Mjukvaruskärpning.** Klart 2026-11-16: skärpning q4 (y + q/32·(4y − grannar)) bara när ett MCU kodas, referensen hålls oskärpt så deltorna påverkas inte. Detalj (Laplace-sd) 16,9 -> 22,6, fil +7 %, brus på jämn yta oförändrat; q8 gav 27,6 men mer brus. Gäller 320-färgbilder (hel och delta). Trimbart via JTAG 0xFF800030. L1-kod utökad till 32 KB (provat via JTAG).
- [x] **Delta-brus från färgen.** Utrett 2026-11-16: med U/V-vikt 4/2/0 (bara Y) låg brusgolvet ändå på 4–6; det är ljusheten (troligen lampflimmer mellan bildrutor), inte färgen. Ingen ändring, `DELTA_T` 12 behålls.
- [x] **Städning i CC1110-firmware.** 2026-11-16: inaktuella kommentarer rättade (paketformat 0x82, 640-bild ~12 s). Testverktygen bakom avstängda brytare (pinnvandring, SPI-test, prober, U3-dump, rå-tiles) är kvar; om de ska bort är ett beslut för dig, se B7.
- [ ] **Omsändning av saknade bildpaket** (gatewayen begär saknade paketnummer vid nästa poll; Blackfin har bilden i 120 s). Görs om räckviddstestet (B5) visar förluster.

## B. Behöver dig

- [ ] **1. Viloström med Otii.** Firmware sover nu i PM2 när Blackfin är av; nätaggregatet visar bara "under 1 mA".
  - Mät i serie vid 3,0 V minst 2 min i vila (larm av): snitt, golv, toppar för pollen (var ~28 s, sändning + ~110 ms lyssning) och uppvakningen var 0,5 s.
  - Samma med larmet på, och en hel bild (D1 via MQTT `verisure/2NCEKS3T/picture/set`): Blackfin ~0,14 A i ~5–8 s plus 120 s påslagen efteråt (`BF_KEEP_ON_S`).
  - Utifrån resultatet: beslut om `BF_POLL_S` (30 s) och `BF_KEEP_ON_S` (120 s) ska ändras.
- [ ] **2. PIR väcker ur viloläget** (ej provat med den här byggen).
  - Larmet av: rör dig framför kameran, gatewayloggen ska visa B9 inom någon sekund.
  - Larmet på (`verisure/alarm/set` ON): rörelse ska ge en larmbild (0xD5) inom ~10 s.
- [ ] **3. Larm i mörker med IR-blixt.** Mörklägg rummet och stäng av den andra kamerans IR-ljus; larmet på och rörelse ska ge svartvita blixtbilder (scenmedel under `DARK_Y` 40). Tänd: samma rörelse ska ge färgbilder och deltor.
- [ ] **4. Nattbilder utan blixt (idé).** Originalet förlänger bildrutan (VTS, 0x380E/0x380F) i mörker. Kräver mörkt rum; Claude kan köra testet via JTAG (`MD_CAM/blackfin/jtag/bf_set.sh 0xFF800034 ...`) när rummet är mörkt.
- [ ] **5. Räckvidd i verklig placering.** Sätt kameran på sin tänkta plats; några D1/D2 visar saknade paket per bild ("x/y chunks missing") och RSSI i gatewayloggen.
- [ ] **6. Granska och slå ihop grenen.** `md-cam/u3-flash-dump` har mycket nytt (blixt, hastighet, bildkvalitet, viloläge). Säg till om Claude ska öppna en PR mot `main`.
- [ ] **7. Testverktyg i CC1110-firmware:** ska undersökningsverktygen bakom avstängda brytare (`BF_PIN_WALK`, `BF_SPI_WALK`, `BF_SPI1_TEST`, `BF_PROBE_AT_BOOT`, `BF_P0_0_TOGGLE`, U3-dumpen, rå-tiles 0x81) tas bort eller sparas? De kostar inget när de är avstängda men gör `main.c` (~2900 rader) svårare att läsa.
