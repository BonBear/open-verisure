# Kameramodul (J1) – läge och mätplan

Kort #1 (2NCEKS3T). Sammanställt 2026-11-11, mätningarna ska göras härnäst.

## Vad vi vet

- Kameran är en Sunny-modul märkt **"8811GA SUNNY"** på en 24-polig FPC-kontakt, **J1**. Stift 1 sitter överst vid SW1 och stift 24 nederst vid C23/C21.
- Kortet är märkt "SECUR.PCAM.BLACKFIN".
- **U4 = ISSI IS42S16800F** (SDRAM, 16 MB). Den kan användas som bildbuffert via Blackfins EBIU.
- Sensortypen är inte bekräftad. En liknande 24-polig Sunny-modul (8806Z) säljs med OmniVision OV7725, så det är troligen OmniVision.
- TWI-skanning från Blackfin (`blackfin/twi_scan.S`) hittar bara **0x40** (okänd) och **0x5C** (troligen en ljussensor av BH1750-typ). Ingen OmniVision-adress (0x21 eller 0x30) svarar. Sensorn saknar alltså troligen matning, XCLK eller hålls i PWDN.
- PF0..PF9 (troligen PPI-data från kameran) flyter.
- Skriv aldrig "registerpekare" till okända TWI-enheter. Kommandostyrda kretsar tolkar dem som kommandon.

## Förberedelse

1. Strömförsörj kortet med 3,0 V (PSU). CC1110 ska köra ett BF_JTAG_HOLD-bygge med P1_0 = 0 (Blackfin ur reset).
2. Kontrollera strömmen: cirka 95–100 mA betyder att Blackfin går.

## Mätning 1: spänningar på J1 (multimeter mot GND)

Kolumnen "Vanlig funktion" gäller om modulen följer den vanliga 24-poliga OmniVision-layouten. Det är inte bekräftat.

| J1 | Vanlig funktion | Förväntat om sensorn är på | Uppmätt |
|---|---|---|---|
| 1 | NC | – | |
| 2 | AGND | 0 V | |
| 3 | SIO_D (I²C-data) | ~2,8–3,3 V | |
| 4 | AVDD | ~2,8 V | |
| 5 | SIO_C (I²C-klocka) | ~2,8–3,3 V | |
| 6 | RESET | hög | |
| 7 | VSYNC | – | |
| 8 | PWDN | **låg** (hög = viloläge) | |
| 9 | HREF | – | |
| 10 | DVDD | ~1,5–1,8 V | |
| 11 | DOVDD | ~1,8–3,3 V | |
| 12 | D9 | – | |
| 13 | **XCLK** | klocka (se mätning 2) | |
| 14 | D8 | – | |
| 15 | DGND | 0 V | |
| 16 | D7 | – | |
| 17 | PCLK | klockar bara när sensorn är igång | |
| 18 | D6 | – | |
| 19 | D2 | – | |
| 20 | D5 | – | |
| 21 | D3 | – | |
| 22 | D4 | – | |
| 23 | D1 | – | |
| 24 | D0 | – | |

## Mätning 2: klocka på J1-13 (Saleae)

- Sätt en Saleae-kanal på J1-13 och kortets GND.
- Notera om det finns en klocka och ungefär vilken frekvens (vanligt är 6–27 MHz).

## Tolkning

- **Matning och XCLK finns, PWDN låg:** sensorn borde svara på TWI. Kör `twi_scan.S` igen och leta efter 0x21 eller 0x30.
- **PWDN hög:** hitta vilket CC1110- eller Blackfin-stift som styr PWDN.
- **Matning saknas:** någon av transistorerna Q8, Q9, Q10 eller Q11 styr den troligen. Hitta vilket stift som slår på den. P0_1 (+7 mA) och P0_7 (aux) är kandidater.
- **XCLK saknas:** klockan måste komma från Blackfin (en timer, TMRx) eller från en oscillator på kortet. Den behöver startas.
- **Stiften följer inte tabellen** (till exempel matning på stift 2 och 15): modulen har en annan layout. Kartlägg den med multimeter (genomgång mot GND och matningsskenor) innan något drivs.
