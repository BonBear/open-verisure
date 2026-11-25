# Verisure smoke detector GW-SD3 (research)

Photo-based hardware survey and reverse-engineering plan for the Verisure smoke alarm **GW-SD3**
(before DMM/bench measurements). Bench measurements go in [docs/re_worksheet.md](docs/re_worksheet.md);
the cross-device overview is in [documentation/device_survey.md](../documentation/device_survey.md).

Everything below comes from the photos and visual PCB inspection. "Confirmed" means the marking,
silkscreen, or physical part has been directly verified; everything else is marked as an assumption
to verify on the bench.

## Unit data (from the labels)

| Field | Unit 1 | Unit 2 |
| :--- | :--- | :--- |
| Model | GW-SD3 "Smoke alarm device" | GW-SD3 |
| Part No | 121467 | 121467 |
| HW / SW | 5.0 / 7.00 | 5.0 / 7.00 |
| Date (YYMMDD) | 240301 | 240219 |
| Serial (device ID) | 3EA8VKXK | 3EB7KFZQ |
| Origin | Verisure Sàrl, Made in Japan | same |
| Detection | Optical, EN 14604:2005, CE 2797 | same |
| Battery | 1× `CR-2/3AZ` (3.0 V Li-MnO2), built-in / "replace by 2034" | same |

Power-off procedure printed on the label: remove the detector from the bracket and hold the front
button until the LED flashes red fast.

## Main parts

| Ref | Marking | Identification | Status |
| :--- | :--- | :--- | :--- |
| MCU/radio | `EFR32 FG14P231GG 2313B0269N`, Silicon Labs logo | **Silicon Labs EFR32FG14P231** Flex Gecko SoC (Cortex-M4, Sub-GHz), QFN48 | Confirmed marking; flash size (`F128` vs `F256`) to read from the DI page over SWD |
| HFXO crystal | `3840T` (4-pad metal can) | 38.400 MHz HFXO routed directly to EFR32 pins 10–11 (`HFXTAL_N`/`_P`) | Visually traced; verify frequency on bench |
| LFXO crystal | `A320L` (2-pad brown) | 32.768 kHz LFXO routed directly to EFR32 pins 35–36 (`PB14`/`PB15`) | Visually traced |
| `IC6` | `winbond 25Q16JLNIG 2323` (SOIC-8) | Winbond **W25Q16JL**, 16 Mbit (2 MiB) SPI NOR flash; traces route into the 2×3 pad grid `48/49/56/57/58/59` + `50` | Confirmed marking; dump & analyze content (OTA image, audio, or log) |
| `IC81` | `C4660 224KU` (TSSOP-20) | Unknown. Surrounded by two inductors (`4R7`/round), `PC41`, and the 100 µF/6.3 V (`107 6.38`) cap; shares `8x` numbering with `LED81`/`PD81` and routes thick traces toward `PD81` | Verify role vs `IC301` by DMM continuity |
| `IC301` | `304A 029J 363` (QFN-32) | Unknown. Sits next to `IC6` flash, test button, and `CN301` (shares `30x` numbering with `CN301`) | Verify role vs `IC81` by DMM continuity |
| 6-pin near `IC81` | `PC41 351 APBP` | Likely boost converter or LDO feeding the 100 µF `107 6.38` rail | Assumption |
| 8-pin near EFR32 | `CP92` | Next to pad `23` and unpopulated `CN11` (`90/91/94`) | Unknown |
| 6-pad metal lid | `341 ZF` | **EPCOS/TDK 868 MHz SAW bandpass filter** between EFR32 `SUBGRF` (pins 13–16) and the U.FL / wire antenna path | Visually traced |
| U.FL connector | none | Inline RF test point next to a small 6-pin RF switch/balun | Confirmed part |
| Wire antenna | none | Bent wire soldered at the board edge, routed around the housing | Confirmed |
| Speaker | `M36BN3D-A 8Ω 0.5W` | 36 mm 8 Ω dynamic speaker (same family as the voicebox speakers) | Confirmed |
| Optical chamber | black disc on back (`LED81`, `PD81`) | Photoelectric smoke chamber; 2-pin `LED81` (IR LED) and 3-pin `PD81` (photodiode) solder through to the front | Confirmed |
| `CN301` | 3-pin JST (front) | Connected near `IC301` (speaker or sensor harness — confirm on bench) | Unknown |
| 2-pin connector | white 2-pin (back, top) | Battery connector (`1× CR-2/3AZ`, 3.0 V); through-hole pads next to front pad `95` | Confirmed |
| `LED41` / `LED42` / `LED43` | under curved reflector mask | `LED41` and `LED42` populated with SOT-23 drivers (`44`/`YK`); `LED43` unpopulated | Confirmed |
| Tact switch | gold top | Front test/silence button | Confirmed |
| `R1` + `R56` | `270` 2512 (27 Ω) ×2 | High-power resistors next to `LED81` and the debug header (IR LED pulse or speaker/load) | Assumption |
| PCB | `K2226`, UL `94V-0` | 4-layer board | Confirmed |

## Radio & custom firmware readiness

The **EFR32FG14P231** integrates a proprietary Sub-GHz transceiver (no external CC1101 needed).
Silicon Labs' public `SiliconLabs/gecko_sdk` (`v4.4.6` LTS) provides both the exact CMSIS device
headers (`efr32fg14p231f128gm48.h` / `efr32fg14p231f256gm48.h`) and the precompiled GCC RAIL
library (`librail_efr32xg14_gcc_release.a`), which supports 868.950 MHz 2-FSK at 38.4 kBaud with
sync word `0xD391` / `0xD391D391` — directly compatible with the OpenVerisure CC1101 gateway.

First step on the bench before writing any firmware: listen with the gateway (`sniff`) and an SDR
while pressing the test button to confirm the factory PHY.

## Debug / programming header & factory jig pads

A staggered 2×4 group of round pogo pads on the back matches 8 holes in the plastic base:

| Pad | Expected EFR32 pin | Assumed function |
| :--- | :--- | :--- |
| `CLK` | Pin 1 (`PF0`) | `SWCLK` |
| `DIO` | Pin 2 (`PF1`) | `SWDIO` |
| `SW` | Pin 3 (`PF2`) or button | `SWO` trace or front switch input (measure) |
| `RES` | Pin 12 (`RESETn`) | Active-LOW reset |
| `TXD` | USART TX (`ROUTELOC0`) | Factory/bootloader UART TX |
| `RXD` | USART RX (`ROUTELOC0`) | Factory/bootloader UART RX |
| `TPR` | GPIO | Tamper input |
| `FIRE` | GPIO / AFE | Fire/alarm test or interconnect signal |

There is no GND or VCC pad inside the 8-pad group. Two large round pads (`01` and `02`) sit next to
the battery connector directly under two matching holes in the base (almost certainly `VBAT+` and
`GND`), alongside `VCC` and `VCA` test pads. Confirm by continuity before powering.

EFR32 SWD works with the Raspberry Pi Debug Probe (CMSIS-DAP) and OpenOCD (`target/efm32.cfg`);
the strictly read-only procedure is in [documentation/bench_recipes.md](../documentation/bench_recipes.md).
**Never run `DEVICE_ERASE` on the only unit** until lock state has been checked and the external
`W25Q16JL` SPI flash has been dumped.

## Reverse-engineering plan

1. **Phase 0 (Passive DMM):** Confirm `01`/`02`/`VCC`/`VCA`/`GND`, map the 8 debug pads to EFR32
   pins, map `IC6` (`W25Q16JL`) pins to the `48/49/56/57/58/59` pad grid, and determine which of
   `IC81`/`IC301` drives the speaker vs the optical smoke chamber (`LED81`/`PD81`).
2. **Phase 1 (Active read-only):** Measure rail voltages (`VCC`, `VCA`, `IC6` VCC) and Otii Arc
   3.0 V idle/pulse current; listen on 868.950 MHz (`sniff`/SDR) and `TXD` UART; run a strictly
   read-only SWD probe (`IDCODE`, DI page, debug lock word).
3. **Phase 2 (External flash & peripheral bus):** Dump `IC6` (`W25Q16JL`) via FT232H/`flashrom`
   (after confirming VCC) and inspect for GBL OTA images, audio clips, or logs; capture EFR32 ↔
   `IC81`/`IC301` bus traffic on Saleae Logic 4.
4. **Phase 3 (OpenVerisure firmware & HA integration):** Build bare-metal + RAIL EFR32FG14 firmware
   (EM2 ~1.5 µA sleep, 868.950 MHz AES-128 KDF protocol, smoke + tamper + battery + speaker alarm)
   and add multi-device `smoke` support to `868Receiver` and Home Assistant MQTT discovery.

## Photo requests

Sharp close-ups still helpful (no full-board stitching needed — the PCB is 4-layer so internal nets
are mapped by DMM continuity):

- [x] **Battery** inside the base: confirmed `1× CR-2/3AZ` (3.0 V Li-MnO2).
- [ ] **`IC81`** (`C4660 224KU`) & **`IC301`** (`304A 029J 363`) with raking light to check for a
      manufacturer logo.
- [ ] **`CP92`** (8-pin near EFR32) and the tiny 6-pin black RF switch/balun above `341 ZF`.
- [ ] The small **SOT-23** parts marked `YK`, `KFD`, `A1`, `44`.
- [ ] Where **`CN301`** (3-pin) leads (cable/harness on the other end).

## Photos

Photos with GPS/EXIF metadata removed and downscaled to 3264 px live in [docs/images/](docs/images/):

* [docs/images/smoke_board_front.jpg](docs/images/smoke_board_front.jpg): component side, with the wire antenna.
* [docs/images/smoke_board_back.jpg](docs/images/smoke_board_back.jpg): back side with the optical chamber and the debug pads.
* [docs/images/smoke_board_back_in_housing.jpg](docs/images/smoke_board_back_in_housing.jpg): back side in the housing, with the 8 Ω speaker (`M36BN3D-A`), `LED81`, `PD81`, and `R56`.
* [docs/images/smoke_closeup_efr32.jpg](docs/images/smoke_closeup_efr32.jpg): the EFR32FG14P231GG and its crystals (`3840T`, `A320L`).
* [docs/images/smoke_closeup_ic81_ic301.jpg](docs/images/smoke_closeup_ic81_ic301.jpg): `IC81` (`C4660`), `IC301` (`304A`), and `PC41`.
* [docs/images/smoke_closeup_ic6_flash.jpg](docs/images/smoke_closeup_ic6_flash.jpg): `IC6` Winbond `W25Q16JL` SPI flash and pad grid `48/49/58/59`.
* [docs/images/smoke_closeup_debug_header.jpg](docs/images/smoke_closeup_debug_header.jpg): the 8 debug pads (`CLK`, `SW`, `RES`, `RXD`, `TXD`, `TPR`, `DIO`, `FIRE`).
* [docs/images/smoke_base_battery_bay.jpg](docs/images/smoke_base_battery_bay.jpg): the base with the `CR-2/3AZ` battery bay and the 2 + 8 factory jig holes.
