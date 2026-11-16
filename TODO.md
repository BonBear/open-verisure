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
- [x] **Optimeringsflaggor för snap_link.** Klart 2026-11-16: -O2, fångst+kodning 3–4x snabbare (320: 0,86 -> 0,29 s, 640: 2,61 -> 0,60 s).
- [x] **Korta Blackfins uppstartsväntan.** Klart 2026-11-16: fast väntan (~3 s) ersatt med ping (0xE5 -> 0xA5) efter ~140 ms; snap_link svarar efter ~1,2 s (37 pingar). Kall bild 8 s -> 6–7 s från kommando, ~2 s mindre Blackfin-tid per larmhändelse. e2e 11/11.
- [ ] **Dörrsensorn sover inte.** `VerisureDoorSensor/main.c` läser reed/sabotage/skak varje ms i en aktiv loop (ingen PM2, inga avbrott), vilket begränsar batteritiden. Inför PM2 med port-avbrott (P1_7, P1_2, P0_1) och sömntimer för hjärtslaget, med watchdog som i kameran. Kräver en dörrsensor på flashern för test.
- [x] **Exponeringen efter kallstart.** Klart 2026-11-16: det var färgen, inte exponeringen (Cb +4..5 de första sekunderna, Y oförändrat; sensorns vitbalans, 0x3406 hjälpte inte). snap_link mäter nu färgförskjutningen mot referensen i block där ljusheten inte ändrats och kompenserar före delta-jämförelsen: första deltan efter kallstart 118–134 block -> 0. e2e 12/12.
- [x] **Radera originalfirmwaren i Blackfins boot-flash.** Klart 2026-11-16: alla dumpar kopierade till `C:epoerisure_firmware_dumps\` (med SHA-256-lista och README), kopian på 0x10000 läst ur chippet och identisk med dumpen, sedan raderad (0x10000–0x17FFF, kontrollerad tom). Kameran startar som förut, e2e 12/12. Dörrsensorns original finns inte dumpat.
- [x] **Omsändning av saknade bildpaket.** Klart 2026-11-16: gatewayen begär saknade delar (nedlänk 0xF0–0xFF, upp till 16 delar) med svaret på pollen efter bilden, kameran skickar dem som 0x83 ur Blackfins buffert (snap_link 0xE1). Testat med gatewayens `drop <n>`: var 7:e tappad -> komplett efter 10 omgångar/3,2 s (640: 23 omgångar/7,6 s, även med förlorat JPEG-huvud), var 3:e -> 13 omgångar/4,1 s. Nyttan i verklig placering visar räckviddstestet (B5).

## B. Behöver dig

- [ ] **0. Byt huvudnyckel före driftsättning.** Den nuvarande nyckeln har legat i två dokument på publika `origin/main` sedan 2026-10-22 (ersatt med platshållare i commit 75062aa, finns kvar i git-historiken). Inte allvarligt så länge systemet inte används på riktigt, men innan det tas i drift: ny nyckel (`python derive_key.py --generate-master`) i gatewayens `secrets.h`, nya `config.h` och omflashning av gatewayen, alla sensorer och kameran.

- [ ] **1. Viloström med Otii.** Firmware sover nu i PM2 när Blackfin är av; nätaggregatet visar bara "under 1 mA".
  - Mät i serie vid 3,0 V minst 2 min i vila (larm av): snitt, golv, toppar för pollen (var ~28 s, sändning + ~110 ms lyssning) och uppvakningen var 0,5 s.
  - Samma med larmet på, och en hel bild (D1 via MQTT `verisure/2NCEKS3T/picture/set`): Blackfin ~0,14 A i ~5–8 s plus 15 s påslagen efteråt (`BF_KEEP_ON_S`).
  - Utifrån resultatet: beslut om `BF_POLL_S` (30 s) och `BF_KEEP_ON_S` (sänkt från 120 till 15 s 2026-11-16 i väntan på mätningen) ska ändras. Obs: 120 s påslagen efter varje bild kostar ~17 As (0,14 A), mer än själva bilden; nu när en hel bild bara tar ~5 s kan en kortare tid vara värd att deltor ibland blir hela bilder.
- [ ] **2. PIR väcker ur viloläget** (ej provat med den här byggen).
  - Larmet av: rör dig framför kameran, gatewayloggen ska visa B9 inom någon sekund.
  - Larmet på (`verisure/alarm/set` ON): rörelse ska ge en larmbild (0xD5) inom ~10 s.
- [ ] **3. Larm i mörker med IR-blixt.** Mörklägg rummet och stäng av den andra kamerans IR-ljus; larmet på och rörelse ska ge svartvita blixtbilder (scenmedel under `DARK_Y` 40). Tänd: samma rörelse ska ge färgbilder och deltor.
- [ ] **4. Nattbilder utan blixt (idé).** Originalet förlänger bildrutan (VTS, 0x380E/0x380F) i mörker. Kräver mörkt rum; Claude kan köra testet via JTAG (`MD_CAM/blackfin/jtag/bf_set.sh 0xFF800034 ...`) när rummet är mörkt.
- [ ] **5. Räckvidd i verklig placering.** Sätt kameran på sin tänkta plats; några D1/D2 visar saknade paket per bild ("x/y chunks missing") och RSSI i gatewayloggen.
- [ ] **6. Granska och slå ihop grenen.** `md-cam/u3-flash-dump` har mycket nytt (blixt, hastighet, bildkvalitet, viloläge). Säg till om Claude ska öppna en PR mot `main`.
- [x] **7. Testverktyg i CC1110-firmware.** Borttagna 2026-11-16 efter ditt OK: main.c 2924 -> ~1000 rader, firmware 22490 -> 5989 byte, e2e 11/11. Finns kvar i git-historiken.
