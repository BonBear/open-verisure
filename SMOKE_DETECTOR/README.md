# Verisure smoke detector GW-SD3 (research)

Photo-based hardware survey of the Verisure smoke alarm **GW-SD3**, made before any bench work.
No firmware or protocol work yet. Measurements still to be taken go in
[re_worksheet.md](re_worksheet.md); the cross-device overview is in
[documentation/device_survey.md](../documentation/device_survey.md).

Everything below comes from the photos. "Confirmed" means the marking or the silkscreen text can be
read in a photo; everything else is marked as an assumption to verify.

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
| Battery | Not replaceable, "replace by 2034" | same |

Power-off procedure printed on the label: remove the detector from the bracket and hold the front
button until the LED flashes red fast.

## Main parts

| Ref | Marking | Identification | Status |
| :--- | :--- | :--- | :--- |
| (MCU/radio, no ref visible) | `EFR32 FG14P231GG 2313B0269N`, Silicon Labs logo | **Silicon Labs EFR32FG14P231** Flex Gecko SoC (Cortex-M4, sub-GHz + 2.4 GHz radio), QFN48 | Confirmed marking; exact flash size from the full OPN still to read |
| Crystal next to the EFR32 | `3840T` (metal can, 4-pad) | 38.400 MHz HFXO (the EFR32 reference value) | Assumption: verify on the bench |
| Crystal (brown, 2-pad) | `A320L` | Probably the 32.768 kHz LFXO | Assumption |
| IC6 | `winbond 25Q16JLNIG 2323` (SOIC-8) | Winbond **W25Q16JL**, 16 Mbit SPI NOR flash | Confirmed marking; use to be found (OTA image, event log or voice samples) |
| IC81 | `C4660 224KU` (TSSOP-20) | Unknown. Sits next to the inductors and the 100 µF/6.3 V polymer capacitor: probably the sounder/speaker driver or a boost converter | Unknown |
| IC301 | `304A 029J 363` (QFN-32) | Unknown; IC3xx numbering and CN301 suggest the smoke-sensing (photo AFE) section | Unknown |
| Small 6-pad metal-lid part | `341 ZF` | Near the U.FL connector: SAW filter, balun or RF switch | Unknown |
| U.FL connector (top left) | none | RF test point or antenna feed | Confirmed part, use unknown |
| Wire antenna | none | Bent wire soldered at the board edge, around the housing | Confirmed |
| Speaker | `M36BN3D-A 8Ω 0.5W` | 36 mm 8 Ω speaker (the same family as the voicebox speakers) | Confirmed; a speaker, not a piezo horn, so the unit may play tones or voice |
| Optical chamber | black disc on the back | Smoke chamber; IR LED/photodiode underneath | Confirmed |
| CN301 | 3-pin JST | Unknown connection | Unknown |
| 2-pin connector (back, top) | white 2-pin | Battery connector (the battery sits in the white bay in the base) | Assumption |
| LED41/42/43 | under a light-pipe window | Status LEDs (red/green?) | Confirmed refs |
| Tact switch | gold top | Test/front button | Confirmed |
| R1 + neighbour | `270` 2512 (27 Ω) x2 | High-power resistors: speaker series or chamber LED | Assumption |
| PCB | `K2226`, UL `94V-0` | Board number | Confirmed |

## Radio

The EFR32FG14 supports proprietary sub-GHz PHYs, so it can probably speak the same 868.950 MHz
2-FSK, 38.4 kBaud link as the CC1101/CC1110 devices. It may also use a newer PHY. Nothing is known
about the protocol yet. First step on the bench: listen with the gateway's CC1101 (the door sensor
settings) and with an SDR while the test button is pressed. See the worksheet.

## Debug / programming header

A row of 8 round pogo pads on the back (silkscreen read from the photo), arranged in two columns:

| Pad | Assumed meaning |
| :--- | :--- |
| CLK | SWCLK (EFR32 SWD clock, PF0) |
| DIO | SWDIO (EFR32 SWD data, PF1) |
| SW | SWO trace (PF2) or a switch input: to measure |
| RES | RESETn |
| TXD | UART TX (factory/bootloader serial) |
| RXD | UART RX |
| TPR | Tamper input? |
| FIRE | Fire/alarm signal (test or interconnect output?) |

There is no GND or VCC pad in this group. `VCC` and `VCA` test pads sit next to the battery
connector; GND has to be found by continuity. The base has a matching 8-hole pattern, so the
factory jig reaches the pads through the base.

EFR32 SWD works with a J-Link, a CMSIS-DAP probe (for example a Raspberry Pi Pico running
debugprobe) or OpenOCD (`target/efm32.cfg`). Debug access may be locked. On Series 1 parts a
device erase (`DEVICE_ERASE` through the AAP) unlocks it, but it wipes the flash, so **read
the lock state first and never erase the only unit** until the firmware has been dumped another way
(for example from the external W25Q16 flash).

## Numbered test pads

The back has many numbered round pads (5, 9, 10, 21-26, 30, 34-37, 41-45, 48, 49, 51-54, 56-61, 63-65,
76, 88, 89, ...). The front has 15, 23, 28, 32, 39, 40, 50, 56-59 and 48-49 (a 2x4 grid next to IC6),
62, 90, 91, 94, 95. Their nets are unknown; the worksheet has a table for them.

## Photo requests

New photos that would identify the parts the current ones cannot. How to take them is in
[documentation/device_survey.md](../documentation/device_survey.md#photo-requests). Tick an item when
the photo is in the folder.

- [ ] **IC81** (`C4660 224KU`, TSSOP-20): straight on, sharp, plus the parts around it, to identify
      the chip and see whether it drives the speaker.
- [ ] **IC301** (`304A 029J 363`, QFN-32): straight on with raking light; the third line may be a date
      code, so the whole top surface is needed.
- [ ] The 6-pad metal-lid part **`341 ZF`** and the **U.FL** connector next to it, with the traces
      between them and the EFR32.
- [ ] The small **SOT-23 / SOT-323** parts marked `YK`, `KFD`, `A1`, `4A` (one photo each, readable).
- [ ] The **battery** inside the base: type, voltage and maker marking, and how it connects to the
      2-pin connector.
- [ ] Where **CN301** (3-pin) leads: the cable or part on the other end.
- [ ] The **8 debug pads** straight on with a ruler or calliper in the picture (pad pitch and diameter
      for a pogo jig).
- [ ] The **EFR32** marking once more in sharp focus, to read the full part number (flash size).

## Photos

Photos with GPS/EXIF metadata removed and downscaled to 3264 px.

* [smoke_board_front.jpg](smoke_board_front.jpg): component side, with the wire antenna.
* [smoke_board_back.jpg](smoke_board_back.jpg): back side with the optical chamber and the debug pads.
* [smoke_board_back_in_housing.jpg](smoke_board_back_in_housing.jpg): back side in the housing, with the speaker.
* [smoke_closeup_efr32.jpg](smoke_closeup_efr32.jpg): the EFR32FG14 and its crystals.
* [smoke_closeup_ic81_ic301.jpg](smoke_closeup_ic81_ic301.jpg): IC81 (C4660) and IC301 (304A).
* [smoke_closeup_ic6_flash.jpg](smoke_closeup_ic6_flash.jpg): IC6 Winbond SPI flash.
* [smoke_closeup_debug_header.jpg](smoke_closeup_debug_header.jpg): the 8 debug pads (CLK, SW, RES, RXD, TXD, TPR, DIO, FIRE).
* [smoke_base_battery_bay.jpg](smoke_base_battery_bay.jpg): the base with the battery bay and the jig holes.
* [smoke_housing_front.jpg](smoke_housing_front.jpg): the assembled detector.
* [smoke_label_unit1.jpg](smoke_label_unit1.jpg), [smoke_label_unit2.jpg](smoke_label_unit2.jpg): rating labels.
