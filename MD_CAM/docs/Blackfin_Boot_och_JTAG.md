# Blackfin (U5, ADSP-BF512) – start, JTAG och plan för eget program

Status 2026-11-01, Kort #1 (2NCEKS3T, upplåst, SW3 kvar). Genomringningen av J5 och bollarna under SW3 (BMODE, HWAIT, JTAG) gjordes på Kort #2 (låst referens) efter att SW3 lödats av där; korten är byggda lika. JTAG och matningsmätningarna är gjorda på Kort #1.

## Bakgrund

- U5 är **ADSP-BF512 KBCZ-4**, varianten utan internt flashminne. Programmet måste komma utifrån.
- U3 (EN25F40, 512 KB) innehåller **ingen** Blackfin-image på något av korten, bara tre konfigurationssidor (0x07E000, 0x07E200, 0x07F000).
- Referenskort #2 startar aldrig Blackfin utan en Verisure-central som begär bild, så originalprogrammet går inte att fånga därifrån.
- Beslut: vi skriver **ett eget Blackfin-program** och laddar det själva.

## Bollkarta och orientering

- 168-bolls CSP_BGA, 14×14, 0,8 mm pitch. Viafältet på undersidan är en dog-bone-fanout med en tom mittlinje, så varje via motsvarar en boll.
- I `images/blackfin_bottom.jpeg` sitter **A1 uppe till höger**, raderna A..P uppifrån och ned och kolumnerna 14..1 från vänster till höger. Bekräftat på tre sätt: G14 = GND, märkningen av pricken och siffrorna 1/3/5/7/8/12 i kretskortstrycket längs rad A.
- Kartor: `images/blackfin_bottom_annotated.png` (hela fältet) och `images/blackfin_bottom_under_sw3_annotated.png` (området under SW3).

## Startläge (BMODE)

| Boll | Signal | Uppmätt | Värde |
|---|---|---|---|
| M3 | BMODE0 | 10 kΩ till 3,3 V | 1 |
| N1 | BMODE1 | 10 kΩ till 3,3 V | 1 |
| M2 | BMODE2 | GND | 0 |
| M4 | HWAIT | ej ansluten | – |

**BMODE = 011: SPI0-master-boot** från ett SPI-minne med /CS på PG15 (G1). Blackfin kan därför inte ta emot sitt program från CC1110 via SPI0-slav (100) eller UART0 (111) utan omlödning. Det förklarar varför våra försök med SPI-slavboot bara gav 0x00 tillbaka.

## J5 = Blackfin JTAG

8-polig kontakt på undersidan, stift 1 = den fyrkantiga padden.

| J5 | Boll | Signal | Koppling till FT232H (CJMCU) |
|---|---|---|---|
| 1 | – | 3,3 V via 10 kΩ (referens, 3,26 V uppmätt) | – |
| 2 | N4 | TRST (ingen neddragning på kortet, 0 V när Blackfin är på) | 1 kΩ till J5-1 |
| 3 | P2 | TDI | AD1 |
| 4 | N5 | TMS | AD3 |
| 5 | P3 | TCK | AD0 |
| 6 | – | GND | GND |
| 7 | N3 | TDO | AD2 |
| 8 | M5 | EMU | – |

TRST måste ligga hög för att JTAG ska fungera, därav 1 kΩ mellan J5-1 och J5-2.

## Verktyg

- ADI GNU-toolchain 2014R1 i `C:\Tools\adi-blackfin\elf\bin`: `bfin-elf-gcc`, `bfin-elf-ldr`, `bfin-elf-gdb`, `bfin-jtag` (urJTAG med BF51x-stöd, part `bf518`) och `bfin-gdbproxy`.
- Adapter: **CJMCU FT232H** (FT232HQ, MPSSE) med **WinUSB**-drivrutin installerad via Zadig (enheten heter "Single RS232-HS", 0403:6014). Den ger då ingen COM-port.
- **ADI:s `bfin-jtag`/`bfin-gdbproxy` når inte FT232H på Windows 11.** De har ett gammalt statiskt libusb (1.0.6) och inget fungerande ftd2xx-stöd. Vi pratar därför JTAG direkt med `blackfin/ft232h_jtag.py` (pyusb + libusb-package).
- Blackfins JTAG-register (IR 5 bitar): IDCODE 00010, DBGCTL 00100, DBGSTAT 01100, EMUIR 01000, EMUDAT 10100, EMUPC 11110, BYPASS 11111. Källa: `C:\Tools\adi-blackfin\elf\share\urjtag\analog\bfin\bfin`.
- **Verifierat 2026-11-08:** IDCODE **0x227E80CB** (version 2, part 0x27E8 = BF51x, ADI).
- FT232R-kort och ST-Link V2 fungerar **inte**: FT232R saknar MPSSE och ST-Link stöds inte av urJTAG.

## Program och skript (`MD_CAM/blackfin/`)

| Fil | Syfte |
|---|---|
| `bf512_l1.ld` | Länkskript: allt i L1-instruktionsminnet (0xFFA00000). |
| `build_ldr.py` | Bygger ELF, och utan `--elf-only` även LDR och `firmware/bf_ldr.h`. |
| `jtag_test.S` | Första JTAG-testet: skriver 0xB1A5C0DE till 0xFF800000 och räknar i 0xFF800004. Driver inga stift. |
| `ft232h_jtag.py` | Egen JTAG via FT232H (MPSSE). `idcode` läser kedjan. |
| `jtag_run.py` | Avsett för ADI:s gdbproxy, som inte når FT232H på Windows 11 (se Verktyg). |
| `spi_probe.S` | Gammalt SPI0-slavboottest (fungerar inte med BMODE 011). |
| `ph_sniff.S` | Samplar PH0..PH3 och loggar varje ändring i L1 (enkel logikanalysator för CC1110-länken). |
| `sw_spi_slave.S` | Mjukvaru-SPI-slav på PH0..PH3. Svarar med komplementet av förra byten. |

## CC1110-läget BF_JTAG_HOLD (`firmware/main.c`)

Slår på matningarna (P0_2, P0_7), sätter P0_0 och skickar RF-livstecken `B0, räknare` var 10:e sekund (seq från 48000). Med `BF_P1_0_RELEASE` släpps P1_0 efter 10 s, och då kommer Blackfin ur reset.

**/RESET styrs av Q7 (verifierat 2026-11-11):** Q7 (SOT-89, märkt "N06") är en N-MOSFET. Gate = P1_0 (CC1110 pin 4), drain = B10 /RESET, source = GND. B10 har 10 kΩ pull-up. P1_0 = 1 håller Blackfin i reset och P1_0 = 0 släpper den. När CC1110 står i debug halt hamnar P1_0 hög via pull-up, så då är Blackfin också i reset. Bilder: `images/q7_reset_area.jpg` och `images/q7_reset_area_annotated.png`.

## Fallgropar

- En **strömlös** ESP32-flasher på J6 håller CC1110 i reset via GPIO33 (RESET_N). Ge den USB-ström innan den kopplas in.
- Flasherns verifiering kan svara "Verify failed" fast programmet körs. Kontrollera med gatewayens `/api/status` (seq och p0).
- Gatewayens replay-filter kräver högre seq än tidigare byggen.

## Nästa steg

1. Löd 1 kΩ mellan J5-1 och J5-2 och kontrollera att J5-2 visar cirka 3,2 V med Blackfin påslagen.
2. ~~Koppla FT232H och läs IDCODE~~: klart 2026-11-08, `python MD_CAM/blackfin/ft232h_jtag.py idcode` ger 0x227E80CB.
3. ~~Ladda och kör `jtag_test.S` via JTAG~~: klart 2026-11-10 med ADI:s `bfin-gdbproxy` i WSL och Windows `bfin-elf-gdb` (`target remote localhost:2000`, `load build/jtag_test.elf`, break på `_done`). Resultat: 0xB1A5C0DE och räknaren 0x100000 i 0xFF800000. Förutsättning: B10 (/RESET) måste vara hög. Efter flashning står CC1110 i debug halt och då ligger B10 låg. Skicka `/cc_exit_dbg` och vänta tills P1_0 = 0 (Q7).
4. ~~Kartlägg CC1110 ↔ Blackfin~~: klart 2026-11-11 med `BF_PIN_WALK`/`BF_SPI_WALK` i `firmware/main.c`. CC1110 når Blackfin **SPI1** på port H via P1_2-enable: P1_4 -> PH1 (SCK), P1_3 -> PH2 (MISO), P1_5 -> PH3 (MOSI), PH0 (SSEL) = 0. UART-stiften P0_4 och P0_5 syns inte på PF, PG eller PH. P0_3 måste vara hög, och P0_0 verkar slå på Blackfins matning. Detaljer finns i `Hardware_Pinout_och_Matprotokoll.md`.
5. ~~SPI mellan CC1110 och Blackfin~~: tvåvägs-SPI fungerar 2026-11-11 med mjukvaru-SPI på båda sidor. Blackfin kör `sw_spi_slave.S` (PH0 SS, PH1 SCK, PH2 MISO, PH3 MOSI som GPIO) och CC1110 kör `BF_SPI1_TEST` (bit-bangad master i läge 0). Blackfins SPI1-kontroller tar inte emot något som slav (provat med PORTH_MUX 0–3, CPHA 0/1 och TIMOD 00/01). Orsaken är inte utredd, se `.agents/memory.md` (BF_SPI1_SLAVE_FAIL).
6. Nästa: ett riktigt protokoll över länken (kommandon, längd, kontrollsumma), sedan kamerasensorn (PPI på PF).

## Kartläggningsmetod

Blackfin körs ur reset med `jtag_test` laddat (driver inga stift). Via gdbproxy sätts PORTF/G/H_FER = 0, DIR = 0 och INEN = 0xFFFF. CC1110 lägger ett stift i taget högt, och portarna läses via JTAG (scratchpad-skriptet `pinwalk3.py`). Om IDCODE blir 0xFFFFFFFF eller skräp sitter en JTAG-sladd löst, oftast TDO (AD2).
