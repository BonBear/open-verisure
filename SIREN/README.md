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
| XT1 | not readable | CC1101 crystal: 27.000 MHz like the door sensor, or 26 MHz | To measure |
| IC2 | `L22901-707 OKI JAPAN 3285F01J` (SSOP-30) | **OKI ML22 family speech-synthesis LSI** (assumed); plays the stored siren tones / voice | Marking confirmed, function assumed |
| XT2 | not readable | Clock for IC2 | To measure |
| IC5 | `2115 E58 JRC` (MSOP-8) | NJM2115 dual op-amp (assumed) | Assumption |
| IC9 | MSOP/TSSOP-8 next to a 100 µH inductor and 2x 47 µF | Boost converter for the horn supply (assumed) | Assumption |
| Q15–Q19 | `4F`, `4A` SOT-89 | Horn driver transistors (H-bridge?) | Assumption |
| CN4 + FIL1 | U.FL + SAW filter | RF test point in the antenna path | Confirmed parts |
| Antenna | straight wire from the back, top left | 868 MHz whip | Confirmed |
| CN5 / CN6 / CN7 | 3x 3-pin JST, each with a fuse (F11–F13) | Horn / external connections | Unknown |
| CN2 (back) | 2-pin, `アカ` (red) / `クロ` (black) | Battery | Confirmed silkscreen |
| CN1 (back) | castellated pads + TP84–TP87 | Unknown (factory connector?) | Unknown |
| SW1 (back) | push switch | Tamper switch (assumed) | Assumption |
| TH1 (back) | leaded thermistor | Temperature sensing | Confirmed part |
| IC4, IC7, XT3 | not fitted | Option footprints | Confirmed empty |
| PCB | `M1765-1` | Board number; the katakana silkscreen suggests a Japanese maker (Panasonic, like the voicebox) | Confirmed |

## Debug / programming candidates

TP61–TP64 (a 4-pad group next to IC1), TP70–TP72, TP31/TP32, TP81/TP82, TP84–TP88 (back, next to
CN1), TP91/TP92 (back). The MSP430F149 has 4-wire JTAG (needs a TI FET) and a ROM BSL (P1.1 TX /
P2.2 RX). The BSL is not a read path: a wrong password mass-erases the flash. See the worksheet and
[documentation/bench_recipes.md](../documentation/bench_recipes.md).

## Photo requests

New photos that would identify the parts the current ones cannot. How to take them is in
[documentation/device_survey.md](../documentation/device_survey.md#photo-requests). Tick an item when
the photo is in the folder.

- [ ] The **rating label** (model, device ID, part number, HW/SW, battery).
- [ ] **XT1** next to the CC1101 and **XT2** next to the OKI chip: straight on, the frequency marking
      must be readable (26 or 27 MHz matters for the radio settings).
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
