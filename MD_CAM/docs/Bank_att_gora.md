# MD CAM: att göra vid bänken

Saker som behöver någon på plats vid kameran. Bocka av och lägg till resultatet.

## 1. Viloström med Otii

Firmware från 2026-11-16 sover i PM2 när Blackfin är av (`BF_SLEEP_IDLE 1`, watchdog på).
Nätaggregatet visar bara under 1 mA, så exakt värde saknas.

- [ ] Mät med Otii i serie, 3,0 V, minst 2 minuter i vila (Blackfin av, larm av).
- [ ] Notera: snitt, golv mellan uppvakningarna, toppar och längd för pollen (var ~28 s, sändning plus ~110 ms lyssning) och för uppvakningen var 0,5 s.
- [ ] Mät samma sak med larmet på (bara PIR-väckningen och pollen ska synas).
- [ ] Mät en hel bild (D1 via MQTT `verisure/2NCEKS3T/picture/set`): Blackfin ~0,14 A plus radio, i ~5–8 s, och 120 s påslagen efteråt (`BF_KEEP_ON_S`).
- [ ] Räkna ut batteritid och avgör om `BF_POLL_S` (30 s) eller `BF_KEEP_ON_S` (120 s) bör ändras.

## 2. PIR väcker ur viloläget

Inte provat med den här byggen.

- [ ] Larmet av: rör dig framför kameran. Gatewayloggen ska visa ett B9-paket inom någon sekund.
- [ ] Larmet på (`verisure/alarm/set` ON): rörelse ska ge en larmbild (0xD5) inom ~10 s.

## 3. Larm i mörker med IR-blixt

- [ ] Mörklägg rummet och stäng av den andra kamerans IR-ljus.
- [ ] Larmet på och rörelse: bilderna ska bli svartvita blixtbilder (snap_link 0xD5/0xD6 med scenmedel under `DARK_Y` 40).
- [ ] Tänd: samma rörelse ska ge vanliga färgbilder och deltor.

## 4. Nattbilder utan blixt (idé)

Originalfirmwaren förlänger bildrutan (VTS, register 0x380E/0x380F) i mörker för längre exponering.

- [ ] Prova i mörkt rum via JTAG-skrivning av sensorregister (`0xFF800034 = 0x80000000 | reg << 8 | värde`) och jämför ljusnivå och brus.

## 5. Räckvidd i verklig placering

- [ ] Sätt kameran på sin tänkta plats och ta några D1 och D2. Gatewayloggen visar saknade paket per bild ("x/y chunks missing") och RSSI.
- [ ] Saknas paket ofta: omsändning av saknade paket är nästa steg (gatewayen vet vilka, Blackfin har bilden i 120 s).
