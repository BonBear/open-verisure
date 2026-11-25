# Verisure siren (research)

Photos of the Verisure siren's boards (CC1101 radio and others) and a general technical briefing
([Verisure_technical_briefing.md](Verisure_technical_briefing.md)). No firmware yet. Measurements
still to be taken go in [re_worksheet.md](re_worksheet.md); the cross-device overview is in
[documentation/device_survey.md](../documentation/device_survey.md).

## Main parts (from the photos)

| Ref | Marking | Identification | Status |
| :--- | :--- | :--- | :--- |
| IC1 | `TI 44C1PCWG4 M430F149 REV S` (LQFP-64) | **TI MSP430F149** (60 KB flash, 2 KB RAM) | Confirmed |
| IC3 | QFN-20 | **TI CC1101** sub-GHz transceiver | Confirmed (close-up) |
| XT1 | `26.000` | **26.000 MHz** CC1101 crystal (same as gateway; use `--xtal-mhz 26`) | Confirmed |
| IC2 | `L22901-707 OKI JAPAN 3285F01J` (SSOP-28) | **OKI L22901-707** parallel-controlled audio/tone synthesizer LSI (`-707` ROM/voice code) | Confirmed |
| XT2 | `400W` | Clock for IC2 (~4.00 MHz ceramic resonator) | Marking confirmed |
| IC5 | `2115 E58 JRC` (MSOP-8) | NJM2115 dual op-amp (`V+` pin 8 tied to `TP2` = `IC9` pins 3/5/6 & `Q3`) | Confirmed power link |
| IC9 | Analog Devices MSOP-8 next to `100` (`10 µH`) inductor and 2x `4.7 µF / 50 V` | High-voltage boost converter for the piezo horn; enable = **`IC1` pin 19 (`P1.7`)** | Confirmed (DMM) |
| Q15–Q19 | 2x `4F` + 2x `4A` SOT-89 + `LG` pre-drivers | Complementary H-bridge piezo horn driver -> `D1`/`D2` -> `CN2` | Confirmed |
| CN4 + FIL1 | U.FL + SAW filter | RF test point in the antenna path | Confirmed parts |
| Antenna | straight wire from the back, top left | 868 MHz whip | Confirmed |
| CN5 / CN6 / CN7 | 3x 3-pin JST, each with a fuse (`F11`–`F13`) -> `TP1A`/`TP1B`/`TP1C` | **3x parallel `CR-2/3AZ` (`SH384552520`) 3 V DC batteries** | Confirmed (DMM) |
| CN2 (back) | 2-pin, `アカ` (red -> `D2`) / `クロ` (black -> `D1`) | **Piezo horn** (`OL` resistance; H-bridge output, **NOT battery!**) | Confirmed (DMM) |
| CN1 (back) | castellated pads + TP84–TP87 | Unknown (factory connector?) | Unknown |
| SW1 (back) | push switch (`IC1` pin 18 `P1.6` to `GND`) | **Tamper switch** (active-low) | Confirmed (DMM) |
| TH1 (back) | leaded thermistor (`IC1` pin 6 `P6.7/A7`, `TP19`, to `BAT+`) | **Temperature sensing** (ADC A7) | Confirmed (DMM) |
| LED1 | SMD LED (`IC1` pin 43 `P4.7` -> `Q1` to `GND`) | **Status LED** (active-high) | Confirmed (DMM) |
| IC4, IC7, XT3 | not fitted | Option footprints | Confirmed empty |
| PCB | `M1765-1` | Board number; rating label confirms **Panasonic Corporation, Japan** (`GW-SI`, Part No `131155`, HW `8.0` / SW `4.00`, `2014-11-17`) | Confirmed |

## OKI L22901-707 (IC2) audio interface

Bench-verified parallel interface between IC1 (`MSP430F149`) and IC2 (`OKI L22901-707`):

- **Rails:** `P1.4` (IC1 pin 16, TP2) is the active-LOW audio/op-amp rail switch (`LOW` = ON, `HIGH` = OFF). `P1.7` (IC1 pin 19) is the active-HIGH IC9 HV horn boost enable (keep `LOW` on the bench when Saleae Logic 4 is attached).
- **Parallel port (`P6.0..P6.5` -> IC2 pins `13..18`):**
  - `P6.0` (pin 13): `STOP` / power-down (`val5 = 0x01` strobed stops playback, ~55 ms ramp-down to sleep).
  - `P6.1` (pin 14): Must be `0` for valid commands (`1` = no-op / empty bank).
  - `(P6.5, P6.3, P6.2)` (pins `18, 16, 15`): 3-bit sound index `1..7` (`P6OUT` mapping: `((val5 & 0x10U) << 1) | (val5 & 0x0FU)`).
  - `P6.4` (pin 17, TP38): Falling-edge strobe (`STARTB`/`STB`): drive target bits with `P6.4 = 1` for 10 ms setup, then drive `P6.4 = 0` to latch and trigger.

| `val5` | Index `(P6.5, P6.3, P6.2)` | Sound / behavior |
| :--- | :--- | :--- |
| `0x04` | `001` | 2-beep chime (~440 ms, auto-stops) |
| `0x08` | `010` | ~2112 Hz decaying tone (~560 ms, auto-stops) |
| `0x0C` | `011` | 2-tone chime 1122 Hz -> 1046 Hz (~650 ms, auto-stops) |
| `0x10` | `100` | Powered-up idle / silence (>2.5 s, +1.1 mA) |
| `0x14` | `101` | Full-amplitude continuous alarm siren burst (~2.15 s, auto-stops; re-trigger every 2 s) |
| `0x18` | `110` | Ding-dong bell 2007 Hz -> 1514 Hz (~1.8 s, auto-stops) |
| `0x1C` | `111` | 1-beep chime (~200 ms, auto-stops) |

## Debug / programming pads (confirmed by DMM)

4-wire JTAG on `MSP430F149` (`IC1` pins 54–58): **`TP61`** (`TDO`), **`TP62`** (`TDI`), **`TP63`** (`TMS`), **`TP64`** (`TCK`), **`TP5`** (`RST/NMI`), **`TP68`** (`GND`), **`TP1A`/`TP1B`/`TP1C`** (`VCC`). **Factory JTAG security fuse is blown** (`ERR FUSE_BLOWN` / `0x5555`). ROM BSL uses **`TP28`** (`P1.1` TXD), **`IC1` pin 22** (`P2.2` RXD — no TP pad, direct wire required), **`TP5`** (`RST/NMI`), **`TP64`** (`TCK`), and **`TP68`** (`GND`). The ROM BSL is not a read path (a wrong password mass-erases the flash), so it is used to flash custom OpenVerisure firmware. CC1101 boot SPI was captured on 2026-11-24 (`26 MHz` crystal, GFSK `38.38 kbps`, sync `0xD391`, PATABLE `20 03 0D 34 57 8E 85 C4`). See [re_worksheet.md](re_worksheet.md) and [documentation/bench_recipes.md](../documentation/bench_recipes.md).

## Photo requests

New photos that would identify the parts the current ones cannot. How to take them is in
[documentation/device_survey.md](../documentation/device_survey.md#photo-requests). Tick an item when
the photo is in the folder.

- [x] The **rating label** (`GW-SI`, `2S5F 554X`, Part No `131155`, HW `8.0` / SW `4.00`, `2014-11-17`, 3x `CR-2/3AZ` 3 V DC, Panasonic).
- [x] **XT1** next to the CC1101 (`26.000` MHz) and **XT2** next to the OKI chip (`400W`).
- [ ] **IC9** (next to the 100 µH inductor) and the **`EADH`** part next to the MCU, readable.
- [ ] The transistors **Q15–Q19** and **Q3, Q6–Q9**: markings readable.
- [ ] **CN1** on the back (castellated pads, TP84–TP87) and **SW1** close up.
- [ ] The **TP61–TP64** group next to IC1 together with the MCU pins, so the traces can be followed.
- [ ] The **horn** and the **battery** (type, voltage) and where CN5/CN6/CN7 lead.

## Photos

* [siren_board_overview.jpg](siren_board_overview.jpg): the whole board.
* [siren_board_front_left.jpg](siren_board_front_left.jpg): front side, left half.
* [siren_board_front_right.jpg](siren_board_front_right.jpg): front side, right half.
* [siren_board_front_connectors.jpg](siren_board_front_connectors.jpg): front side, the connectors.
* [siren_board_back_1.jpg](siren_board_back_1.jpg): back side, first view.
* [siren_board_back_2.jpg](siren_board_back_2.jpg): back side, second view.
* [siren_msp430f149.jpeg](siren_msp430f149.jpeg): close-up of the MCU, marked MSP430F149.
* [siren_oki_l22901.jpeg](siren_oki_l22901.jpeg): close-up of the chip marked OKI L22901.
* [siren_cc1101.jpeg](siren_cc1101.jpeg): close-up of the CC1101 radio.

Missing: a photo of the siren's rating label (model, device ID, HW/SW).
