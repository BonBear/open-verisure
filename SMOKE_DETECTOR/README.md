# Verisure smoke detector GW-SD3 (research)

Hardware survey, bench measurements, and reverse-engineering plan for the Verisure smoke alarm
**GW-SD3**. Detailed bench logs live in [docs/re_worksheet.md](docs/re_worksheet.md); the
cross-device overview is in [documentation/device_survey.md](../documentation/device_survey.md).

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
| MCU/radio | `EFR32 FG14P231GG 2313B0269N` | **Silicon Labs EFR32FG14P231** Flex Gecko SoC (Cortex-M4, Sub-GHz), QFN48 | **SWD verified** (`DPIDR 0x2BA01477`, Series 1 AAP `0x26E60011` = debug-locked, unlockable) |
| HFXO crystal | `3840T` (4-pad metal can) | 38.400 MHz HFXO routed to EFR32 pins 10–11 (`HFXTAL_N`/`_P`) | Visually traced |
| LFXO crystal | `A320L` (2-pad brown) | 32.768 kHz LFXO routed to EFR32 pins 35–36 (`PB14`/`PB15`) | Visually traced |
| `IC6` | `winbond 25Q16JLNIG 2323` (SOIC-8) | Winbond **W25Q16JL**, 16 Mbit (2 MiB) SPI NOR flash (2.3–3.6 V, power-gated 0 V in idle) | **DMM mapped** to back pads `51` (`DO`), `52` (`CLK`), `53` (`DI`), `54` (`/CS`), `60` (`VCC`/`/WP`/`/HOLD`) |
| `IC81` | `C4660 224KU` (TSSOP-20) | **Optical smoke-chamber AFE** (connected to `PD81` photodiode; flanked by inductors, `PC41`, 100 µF cap) | **DMM confirmed** (`PD81` → `IC81`) |
| `IC301` | `304A 029J 363` (QFN-32) | **Audio/voice IC with BTL speaker driver** (pins 13–14 → pad `43`, pins 15–16 → pad `42` → `CN301`) | **DMM confirmed** |
| `CP92` | `CP92` (DFN-8 w/ window) | **Digital temperature & humidity sensor** (open rectangular sensing die window visible under microscope) | **Microscope confirmed** |
| 6-pin near `IC81` | `PC41 351 APBP` | Likely boost converter or LDO feeding the 100 µF `107 6.38` rail | Assumption |
| 6-pad metal lid | `341 ZF` | **EPCOS/TDK 868 MHz SAW bandpass filter** between EFR32 `SUBGRF` (pins 13–16) and U.FL / wire antenna | Visually traced |
| Speaker & `CN301` | `M36BN20-A 8Ω 0.5W` | 36 mm 8 Ω dynamic speaker plugged into 3-pin JST `CN301` (outer 2 pins used) | **Confirmed** (`CN301` → `IC301`) |
| Optical chamber | black disc (`LED81`, `PD81`) | Photoelectric smoke chamber; **`LED81` → EFR32 pin 5 (`PF4`)**, **`PD81` → `IC81`** | **DMM confirmed** |
| Power pads | `01`/`03`/`29`, `02`/`04`/`27`, `VCC`, `VCA` | `01`/`03`/`29` = `BAT+`, `02`/`04`/`27` = `GND`, `VCC` = 3.0 V always-on, `VCA` = 0 V in idle | **DMM & voltage confirmed** |
| `LED41` / `LED42` / `LED43` | under curved reflector mask | `LED41` and `LED42` populated with SOT-23 drivers (`44`/`YK`); `LED43` unpopulated | Confirmed |
| `R1` + `R56` | `270` 2512 (27 Ω) ×2 | 27 Ω ∥ 27 Ω = 13.5 Ω (measured 14 Ω) to `BAT+` | **DMM confirmed** |
| PCB | `K2226`, UL `94V-0` | 4-layer board | Confirmed |

## Radio & custom firmware readiness

**Bench-confirmed 868.950 MHz compatibility (2026-11-26):** powering the stock GW-SD3 at 3.0 V
produced **3 boot frames** received directly by the OpenVerisure gateway (`CC1101#1`,
**868.950 MHz, 2-FSK, 38.4 kBaud, sync `D3 91`**) at **-55 dBm** (LQI 51–57):

```
EC F1 {0D,19,05} 91 FF DD 84 DB 15 85 2D B0 B1 DB BF F4 4C 62 28 DF
```

Silicon Labs' public `SiliconLabs/gecko_sdk` (`v4.4.6` LTS) provides both the exact CMSIS device
headers (`efr32fg14p231f128gm48.h` / `efr32fg14p231f256gm48.h`) and the precompiled GCC RAIL
library (`librail_efr32xg14_gcc_release.a`), enabling clean bare-metal + RAIL builds with
`arm-none-eabi-gcc` both locally and in CI.

## Debug / programming header & SWD status

A staggered 2×4 group of round pogo pads on the back matches 8 holes in the plastic base (and pads
`01`/`02` match the 2 power holes next to the battery bay):

| Pad | Confirmed EFR32 pin | Function |
| :--- | :--- | :--- |
| `CLK` | **Pin 1 (`PF0`)** | **Confirmed `SWCLK`** |
| `DIO` | **Pin 2 (`PF1`)** | **Confirmed `SWDIO`** |
| `RES` | **Pin 12 (`RESETn`)** | **Confirmed `RESETn`** |
| `SW` | **Pin 25 (`PA0`)** | GPIO input (not `SWO`; likely test button) |
| `TXD` / `RXD` / `TPR` / `FIRE` | TBD | UART / tamper / fire interconnect |
| `01` / `03` / `29` | — | **`BAT+`** (3.0 V) |
| `02` / `04` / `27` | — | **`GND`** |

**SWD probe result (2026-11-26, read-only):** Raspberry Pi Debug Probe (CMSIS-DAP v2, `D` port:
Orange=`CLK`, Yellow=`DIO`, Black=`GND`) + `pyOCD 0.46.1` connected cleanly at 200 kHz with 3.0 V
supply:
- `DPIDR = 0x2BA01477` (ARM Cortex-M4 SW-DP)
- `AP 0 IDR = 0x26E60011` (Silicon Labs Series 1 **AAP** — **debug-locked**)
- Unlike the SIREN's blown MSP430 JTAG fuse, Series 1 EFR32 debug lock is **recoverable via
  `AAP DEVICE_ERASE`** using the same Raspberry Pi Debug Probe (erases stock flash and unlocks SWD
  for custom OpenVerisure firmware). **Do not unlock until `IC6` (`W25Q16JL`) has been dumped**, and
  keep Unit 2 (`3EB7KFZQ`) untouched as a stock reference.

## Reverse-engineering plan

1. **Phase 0 (Passive DMM & 3.0 V sniff) — DONE:** Confirmed power rails, SWD pins, `IC6` back-pad
   pinout (`51–54`/`60`), `IC301` speaker driver, `IC81`/`LED81`/`PD81` smoke chamber, `CP92`
   temp/humidity sensor, 868.950 MHz RF link, and SWD AAP status.
2. **Phase 1 (External flash & bus sniff) — NEXT:** Dump `IC6` (`W25Q16JL`, 2 MiB) via back pads
   `51–54`, `60`, `02` (FT232H + `flashrom`) and inspect for GBL OTA images or voice/audio clips;
   optionally sniff `IC81`/`IC301`/`CP92` buses with Saleae Logic 4.
3. **Phase 2 (OpenVerisure firmware & HA integration):** Unlock Unit 1 (`3EA8VKXK`) via AAP
   `DEVICE_ERASE`, build bare-metal + RAIL EFR32FG14 firmware (EM2 sleep, 868.950 MHz AES-128 KDF
   protocol, optical smoke + temp/humidity + tamper + battery + speaker alarm), and add
   multi-device `smoke` support to `868Receiver` and Home Assistant MQTT discovery.

## Photos

Photos (EXIF stripped) live in [docs/images/](docs/images/):

* [docs/images/smoke_board_front.jpg](docs/images/smoke_board_front.jpg): component side, with the wire antenna.
* [docs/images/smoke_board_back.jpg](docs/images/smoke_board_back.jpg): back side with the optical chamber and the debug/test pads.
* [docs/images/smoke_board_back_in_housing.jpg](docs/images/smoke_board_back_in_housing.jpg): back side in the housing, with the 8 Ω speaker, `LED81`, `PD81`, and `R56`.
* [docs/images/smoke_closeup_efr32.jpg](docs/images/smoke_closeup_efr32.jpg): the EFR32FG14P231GG and its crystals (`3840T`, `A320L`).
* [docs/images/smoke_closeup_ic81_ic301.jpg](docs/images/smoke_closeup_ic81_ic301.jpg): `IC81` (`C4660`), `IC301` (`304A`), and `PC41`.
* [docs/images/smoke_closeup_ic6_flash.jpg](docs/images/smoke_closeup_ic6_flash.jpg): `IC6` Winbond `W25Q16JL` SPI flash.
* [docs/images/smoke_closeup_debug_header.jpg](docs/images/smoke_closeup_debug_header.jpg): the 8 debug pads (`CLK`, `SW`, `RES`, `RXD`, `TXD`, `TPR`, `DIO`, `FIRE`).
* [docs/images/smoke_base_battery_bay.jpg](docs/images/smoke_base_battery_bay.jpg): the base with the `CR-2/3AZ` battery bay and the 2 + 8 factory jig holes.
* [docs/images/smoke_micro_cp92_sensor.jpg](docs/images/smoke_micro_cp92_sensor.jpg): microscope shot of `CP92` digital temperature/humidity sensor showing the open silicon sensing window.
* [docs/images/smoke_micro_ic81.jpg](docs/images/smoke_micro_ic81.jpg): microscope shot of `IC81` (`C4660 224KU`, optical smoke AFE).
* [docs/images/smoke_micro_ic301.jpg](docs/images/smoke_micro_ic301.jpg): microscope shot of `IC301` (`304A 029J 363`, audio/voice + BTL speaker driver).
* [docs/images/smoke_speaker_m36bn20a.jpg](docs/images/smoke_speaker_m36bn20a.jpg): 36 mm 8 Ω 0.5 W dynamic speaker `M36BN20-A` (plugs into `CN301`).
