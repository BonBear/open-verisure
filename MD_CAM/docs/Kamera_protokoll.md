# MD CAM: protokoll som det fungerar nu

Beskriver kedjan **Blackfin (snap_link) → CC1110 → radio → gateway → MQTT** som den är
implementerad (läget 2026-11-16). Källorna är `MD_CAM/blackfin/snap_link.c`,
`MD_CAM/firmware/main.c` och `868Receiver/src/` (main.cpp, crypto.cpp, downlink.*,
image_receiver.*, mqtt_manager.cpp). Äldre analysdokument i den här mappen beskriver
utredningen fram hit och kan innehålla antaganden som inte stämde.

## 1. Blackfin ↔ CC1110 (programvaru-SPI)

CC1110 är master (bitbangat läge 0, P1_2 väljer Blackfin), snap_link är slav på port H.
Varje byte som tas emot bestämmer svaret i *nästa* överföring.

| Kommando | Betydelse |
|---|---|
| `0xD0` | 320x240 gråskala, JPEG (eller läge enligt JTAG-ordet 0xFF800048) |
| `0xD1` | 320x240 färg, hel bild (ny delta-referens, COM-id i JPEG:en) |
| `0xD2` | 640x480 färg (ingen referens) |
| `0xD3` | 320x240 färg som delta mot referensen, eller hel bild om det behövs |
| `0xD4` | 320x240 gråskala med IR-blixt (PF10+PF12 i 5 ms på bildens egen ruta) |
| `0xD5` | larmbild: som 0xD3 i ljus, som 0xD4 om scenmedel < `DARK_Y` (40) |
| `0xD6` | larmbild: som 0xD2 i ljus, som 0xD4 i mörker |
| `0xC0`+q | rå kvart q (test) |
| `0xB0` | svar 0x5A när bilden är klar, annars 0x00 |
| `0xA0` | nollställ läspekaren |
| övrigt | nästa bildbyte (CC1110 skickar 0x00 när den läser) |

Efter 0x5A läser CC1110 längden (2 byte, little endian) och sedan datat.

**Bildkedja i snap_link:** hel bildruta med en DMA till SDRAM, 320-bilden är medel av 2x2
sensorpixlar, radiell linskorrigering, U/V-mättnad x7/4 efter vitbalansen, JPEG Q65.
Deltan använder den kvalitet basbilden hade.

**Delta-blob** (svar på 0xD3/0xD5 när en referens finns):
`'D' '1' id mcus_lo mcus_hi count_lo count_hi`, sedan per ändrat MCU
`idx_lo idx_hi len_lo len_hi` + `len` byte entropikodat data (ett restart-intervall).
Ett MCU räknas som ändrat om något 8x8-block (Y, U eller V) har medel |skillnad| > 12
(`DELTA_T`); grannar räknas från halva tröskeln. Fler än hälften ändrade ger hel bild.

**Trimord över JTAG** (`MD_CAM/blackfin/jtag/bf_set.sh`): 0xFF800034 sensorregister,
0xFF80003C linskorrigering/mättnad, 0xFF800040 JPEG-kvalitet, 0xFF800050 delta-tröskel,
0xFF800078 blixttest. Statusord och delta-histogram läses med `bf_read.sh`.

## 2. Radio upplänk (kamera → gateway), 868,950 MHz, 38,4 kBaud 2-FSK

Alla paket börjar med enhetens ID i klartext (8 ASCII-tecken) följt av AES-128 med
enhetens nyckel (härledd ur huvudnyckeln). CRC-16 läggs på av radion.

**Kort paket, 24 byte** (telemetri, poll, händelser): ID + ett AES-block (ECB).
Klartext: `seq(2) flags batt% mv(2) id[0..3] 'V' 'S' p0 p1 p2 p3`. Kameran använder p0 som typ:

| p0 | Betydelse |
|---|---|
| `0xBA` | poll: p1/p2 = utmaning, p3 = tillstånd (0x01 larmad, 0x02 Blackfin på, 0x04 kvittens) |
| `0xB9` | rörelse: p1 = 1 början, 0 slut |
| `0xB8` | bild misslyckades: p1 = försök, p2/p3 = status/längd |
| `0xB7` | test: tid per bildpaket (bara med `BF_IMG_TIMING 1`) |

**Bildpaket, 56 byte, typ 0x82:** ID + tre AES-block i CBC med noll-IV
(C0 = E(P0), C1 = E(P1 ⊕ C0), C2 = E(P2 ⊕ C1)). Klartext (48 byte):
`seq(2) 0x82 chunk(2) flags data[40] 'V' 'S'`, flags: 0x80 första, 0x40 sista, bit 0–5 längd.
Chunk n bär byte 40n.. av bilden. Ungefär 20 ms per paket: en 320-bild på ~7 KB tar
~4 s i luften, 5–8 s från kommando till färdig bild.
(Äldre bildpaket typ 0x80 i 24 byte med 8 byte data hanteras fortfarande av gatewayen.)

## 3. Gatewayens mottagning

CC1101 tar emot i fast längd **58 byte med hårdvaru-CRC av** (PKTCTRL0 0x00) plus två
statusbyte. Mjukvaran kontrollerar CRC (CC1101 CRC-16, polynom 0x8005, start 0xFFFF):

1. stämmer CRC efter 56 byte och paketet avkodas som typ 0x82 → bilddel;
2. annars stämmer CRC efter 24 byte → vanligt sensor- eller kamerapaket;
3. annars kastas ramen.

Efter ett 24-byte-paket lyssnar radion på brus ~7 ms till; ett paket som börjar då missas.
FIFO:n (64 byte) läses i bitar medan ramen kommer in, sista byten lämnas tills ramen är
klar. Radion stannar i RX efter ett paket (ingen tömning av FIFO:n). Nedlänken sänds med
PKTLEN 24 och CRC på, sedan tillbaka till 58.

Bilddelarna sätts ihop per paketnummer; saknade delar blir nollor. En bild avslutas av
sista delen, av första delen i nästa bild, eller efter 5 s tystnad ("end lost", EOI läggs
till). Hel 320-bild med COM-id blir bas för deltor; svartvita bilder utan id hålls separat
(webbsidan visar den nyaste); 640-bilder hålls separat.

## 4. Radio nedlänk (gateway → kamera)

Svar på varje poll inom ~0,1 s, 24 byte: ID + AES-block med klartext
`'D' 'L' seq(2) chal(2) armed cmd id[0..3] 0 0 'V' 'S'`. Kameran godtar bara ett svar vars
seq och utmaning matchar dess senaste poll. `cmd` 0 = inget, 0xD1–0xD6 = ta bild.

## 5. MQTT (basämne `verisure`)

| Ämne | Riktning | Innehåll |
|---|---|---|
| `verisure/<id>/picture/set` | till gateway | `D1`..`D6` (annars D2) |
| `verisure/alarm/set` | till gateway | `ON` / `OFF` (även alarm_control_panel-tillstånd) |
| `verisure/<id>/image` | från gateway | senaste bilden som JPEG |
| `verisure/<id>/state` | från gateway | telemetri (JSON) |

Home Assistant får knapparna "Take 640x480 picture" (D2) och "Take flash picture" (D4).

## 6. Kamerans tillstånd (cam_alarm_loop)

- Blackfin av: CC1110 sover i PM2 (0,5 s sömntimer eller PIR-flank), watchdog ~1 s.
- Poll var ~30 s (`BF_POLL_S`) och direkt efter varje bild.
- Larmad och rörelse: första bilden 0xD5, andra 0xD6, sedan 0xD5 var ~5 s så länge rörelsen
  håller i sig (`BF_PIR_HOLD_S` 30 s). Larmad utan rörelse: 0xD5 var 30:e minut.
- Blackfin hålls på 120 s efter en lyckad bild (`BF_KEEP_ON_S`), så en delta kan tas mot
  referensen i SDRAM.
