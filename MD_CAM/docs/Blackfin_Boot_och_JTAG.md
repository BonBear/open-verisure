# Blackfin (U5, ADSP-BF512) – start, JTAG och plan för eget program

Status 2026-11-01, Kort #1 (2NCEKS3T, upplåst). Mätningar är gjorda med genomringning på U5:s viafält på undersidan efter att SW3 (sabotagebrytaren) lödats av.

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
- Adapter: **CJMCU FT232H** (FT232HQ, MPSSE). urJTAG-kabeln är `cable FT2232 vid=0403 pid=6014 driver=ftd2xx-mpsse`. FTDI:s D2XX-drivrutin (`ftd2xx.dll`, 32-bit) finns redan i SysWOW64.
- FT232R-kort och ST-Link V2 fungerar **inte**: FT232R saknar MPSSE och ST-Link stöds inte av urJTAG.

## Program och skript (`MD_CAM/blackfin/`)

| Fil | Syfte |
|---|---|
| `bf512_l1.ld` | Länkskript: allt i L1-instruktionsminnet (0xFFA00000). |
| `build_ldr.py` | Bygger ELF, och utan `--elf-only` även LDR och `firmware/bf_ldr.h`. |
| `jtag_test.S` | Första JTAG-testet: skriver 0xB1A5C0DE till 0xFF800000 och räknar i 0xFF800004. Driver inga stift. |
| `jtag_run.py` | `detect` läser JTAG-kedjan; `load` bygger, laddar via gdbproxy + gdb, kör till `_done` och läser tillbaka kontrollordet. |
| `spi_probe.S` | Gammalt SPI0-slavboottest (fungerar inte med BMODE 011). |

## CC1110-läget BF_JTAG_HOLD (`firmware/main.c`)

Slår på 3,3 V (P1_0), 1,2 V (P0_2) och aux (P0_7) i den ordningen, släpper Blackfin /RESET (P0_0) och låter SPI-linjerna vara tri-state med U3 bortkopplad. Skickar RF-livstecken `B0, räknare` var 10:e sekund med seq från 44000. Bekräftat på Kort #1: 94 mA vid 3,0 V.

## Fallgropar

- En **strömlös** ESP32-flasher på J6 håller CC1110 i reset via GPIO33 (RESET_N). Ge den USB-ström innan den kopplas in.
- Flasherns verifiering kan svara "Verify failed" fast programmet körs. Kontrollera med gatewayens `/api/status` (seq och p0).
- Gatewayens replay-filter kräver högre seq än tidigare byggen.

## Nästa steg

1. Löd 1 kΩ mellan J5-1 och J5-2 och kontrollera att J5-2 visar cirka 3,2 V med Blackfin påslagen.
2. Koppla FT232H enligt tabellen och kör `python MD_CAM/blackfin/jtag_run.py detect` (förväntat: BF518-familjen i kedjan).
3. Kör `python MD_CAM/blackfin/jtag_run.py load` och bekräfta 0xB1A5C0DE.
4. Därefter: kartlägg CC1110 ↔ Blackfin-förbindelserna (SPI1, UART1, PG15), sedan kamerasensor (PPI) och protokoll mot CC1110.
