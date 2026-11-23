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
| IC2 | `L22901-707 OKI JAPAN 3285F01J` (SSOP-30) | **OKI ML22 family speech-synthesis LSI** (assumed); plays the stored siren tones / voice | Marking confirmed, function assumed |
| XT2 | `400W` | Clock for IC2 (~4.00 MHz ceramic resonator) | Marking confirmed |
| IC5 | `2115 E58 JRC` (MSOP-8) | NJM2115 dual op-amp (assumed) | Assumption |
| IC9 | Analog Devices MSOP-8 next to `100` (`10 µH`) inductor and 2x `4.7 µF / 50 V` | High-voltage boost converter for the piezo horn | Confirmed (ADI logo + 50 V caps) |
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

## Debug / programming pads (confirmed by DMM)

4-wire JTAG on `MSP430F149` (`IC1` pins 54–58): **`TP61`** (`TDO`), **`TP62`** (`TDI`), **`TP63`** (`TMS`), **`TP64`** (`TCK`), **`TP5`** (`RST/NMI`), **`TP68`** (`GND`), **`TP1A`/`TP1B`/`TP1C`** (`VCC`). BSL TXD (`P1.1`, pin 13) is on **`TP28`**. The MSP430F149 supports 4-wire JTAG only (no SBW; see `tools/esp32_msp430_reader/` or a TI MSP-FET). The ROM BSL is not a read path: a wrong password mass-erases the flash. See [re_worksheet.md](re_worksheet.md) and [documentation/bench_recipes.md](../documentation/bench_recipes.md).

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
