# Verisure voicebox GW-AUD-VB (research)

Photo-based hardware survey of the Verisure voicebox **GW-AUD-VB**, made before any bench work.
No firmware or protocol work yet. Measurements still to be taken go in
[re_worksheet.md](re_worksheet.md); the cross-device overview is in
[documentation/device_survey.md](../documentation/device_survey.md).

Everything below comes from the photos. "Confirmed" means the marking or the silkscreen text can be
read in a photo; everything else is marked as an assumption to verify.

## Unit data (from the label)

| Field | Value |
| :--- | :--- |
| Model | GW-AUD-VB |
| Serial (device ID) | 2AK2N9BR |
| Part No | 121900 |
| HW / SW | 6.0 / 18.00 |
| Date (YYMMDD) | 160521 |
| Maker | Panasonic Corporation for Securitas Direct AB, Made in Japan, CE 0682 |
| Battery | 3 lithium batteries, 3 V d.c. (`SH384552520`), three separate bays in the back cover |

## Main parts

| Ref | Marking | Identification | Status |
| :--- | :--- | :--- | :--- |
| MCU | `TI 61A47XTG4 M430F5435A REV H` (LQFP-80) | **TI MSP430F5435A** (MSP430F5xx, 192 KB flash, 16 KB RAM) | Confirmed |
| Sub-GHz radio | `44601B BCL01Z 1534` (QFN-20, 4x4) | **Silicon Labs Si4460** rev B1 sub-GHz transceiver (assumed from the `4460 1B` marking) | Very likely; confirm on the bench |
| Crystal next to the Si4460 | not readable | Silicon Labs AN785 recommends 26 MHz for 868 MHz designs (family default 30 MHz) | To measure |
| `WLAG` (SOT-23-5) | `WLAG` | LDO or RF switch next to the radio | Unknown |
| DSP | `DSP Group DCX81ACPCFAE4FNC-PB ©2010 DSPG P3XT74.0G.A` (LQFP ~100) | **DSP Group DCX81**, DECT/ULE voice SoC (audio codec, speakerphone DSP, DECT radio) | Confirmed marking; role here (voice codec only, or DECT link to the hub) to find out |
| Flash next to the DCX81 | `winbond 25Q16DWSIG 1602` (SOIC-8) | Winbond **W25Q16DW**, 16 Mbit **1.8 V** SPI NOR flash: probably the DCX81 firmware and voice prompts | Confirmed marking |
| SOIC/MSOP-8 on the back, top | not readable | Near the microphone: microphone preamp or EEPROM | Unknown |
| Large MSOP-8 on the back (right of top) | not readable | Unknown | Unknown |
| `SP1` 4/6-pin parts on the back | `SP1 6E9`, `SP1 51C` | Unknown (load switches?) | Unknown |
| Microphone | gold MEMS can with port hole, top of the back side | MEMS microphone | Confirmed part type |
| PIR | white dome lens + small board `M1821` with 3 leads and R/W/B wires | Passive infrared motion sensor | Assumption from the shape |
| Speakers | 2x `M36BN20-A 8Ω 0.5W` | 36 mm 8 Ω speakers in the front cover | Confirmed |
| Output capacitor | `p3 330 6V` (330 µF/6.3 V) | Audio amplifier / boost reservoir | Confirmed |
| U.FL connectors | two (top of the back, bottom right of the back) | RF test points for the two radios | Confirmed parts |
| Wire antennas | two bent wires along the long edges | Probably one for 868 MHz and one for 1.9 GHz DECT | Assumption |
| FPC connector | ~24-pin ZIF on the front | To the front panel (buttons/LEDs) | Assumption |
| Tact switch | next to the FPC connector | Pairing / reset button? | Unknown |
| PCB | `K1969-1` | Board number | Confirmed |

## Radio

Two radios are fitted:

- **Si4460** (sub-GHz): most likely the Verisure 868 MHz alarm link (the same link as the door sensor
  and the siren).
- **DCX81** (DECT 1.88–1.90 GHz in Europe): either the two-way voice link to the hub or only used as
  the voice DSP. To find out with an SDR at 1.88–1.90 GHz during a voice session.

## Debug / programming

No labelled header was found. Candidates:

- **Pogo pad cluster** on the front (pads 41, 42, 51–59, 61, 66, 67 and others; about 20 large pads in
  a hexagonal grid next to the FPC connector). This is most likely the factory test/programming
  jig. The MSP430F5435A can be programmed through 4-wire JTAG (TDO/TDI/TMS/TCK + TEST/RST) or
  Spy-Bi-Wire (SBWTCK = TEST, SBWTDIO = RST); both need a TI FET (MSP-FET or a LaunchPad's eZ-FET),
  which the bench does not have yet. The UART BSL (P1.6/P1.5) is not a read path: a wrong password
  mass-erases the flash. The DCX81 has its own JTAG/UART.
- Numbered test pads around the MCU: 70, 71, 72, 76, 77, 79, 101, 102, 104, 105.
- Others: 1, 2, 28, 45, 49, 62, 63, 65, 73–75, 78, 82, 88, 96, 97, 100, 103, 106–108, 110, 121.

The MSP430F5xx has a JTAG security fuse, and its BSL password is the interrupt vector table, which
is unknown without the firmware. Check the JTAG state with a FET before anything else, and never try
the BSL on this unit. Details: [documentation/bench_recipes.md](../documentation/bench_recipes.md).

## Photo requests

New photos that would identify the parts the current ones cannot. How to take them is in
[documentation/device_survey.md](../documentation/device_survey.md#photo-requests). Tick an item when
the photo is in the folder.

- [ ] The **Si4460** (`44601B`) and its **crystal** next to it (top of the front side): straight on, to
      confirm the radio and read the crystal frequency.
- [ ] **`WLAG`** (SOT-23-5) next to the radio, readable.
- [ ] The **SOIC/MSOP-8 next to the microphone** (back, top) and the **MSOP-8 on the back, right**: the
      markings are not readable in the current photos.
- [ ] The **`SP1 6E9`**, **`SP1 51C`** and the 6-pin **`21`** parts on the back, plus the SOT parts
      marked `B4`, `64`, `6P`.
- [ ] Under the **two black foam squares** on the back, if they can be lifted without damage.
- [ ] The **DCX81 crystal** and the area around the DCX81 for test pads (UART/JTAG).
- [ ] The **pogo pad cluster** straight on with all pad numbers readable and a ruler in the picture.
- [ ] The **other end of the FPC cable** (front panel board: buttons, LEDs) and its markings.
- [ ] The **batteries**: type and size.
- [ ] Both **antenna wires** in full length, with a ruler (the length tells which band each serves).

## Photos

Photos with GPS/EXIF metadata removed and downscaled to 3264 px.

* [voicebox_board_front.jpg](voicebox_board_front.jpg): front (component) side, pogo pads, FPC, PIR, DCX81.
* [voicebox_board_front_in_housing.jpg](voicebox_board_front_in_housing.jpg): front side in the housing.
* [voicebox_board_back.jpg](voicebox_board_back.jpg): back side: MSP430, microphone, U.FL, antennas.
* [voicebox_board_and_speakers.jpg](voicebox_board_and_speakers.jpg): board lifted, speaker wiring.
* [voicebox_speakers_side.jpg](voicebox_speakers_side.jpg): speakers and the PIR board, side view.
* [voicebox_closeup_msp430.jpg](voicebox_closeup_msp430.jpg), [voicebox_closeup_msp430_bottom.jpg](voicebox_closeup_msp430_bottom.jpg): the MSP430F5435A.
* [voicebox_closeup_dcx81_flash.jpg](voicebox_closeup_dcx81_flash.jpg): the DCX81 and the W25Q16DW.
* [voicebox_closeup_winbond.jpg](voicebox_closeup_winbond.jpg): the W25Q16DW flash.
* [voicebox_closeup_rf_connector.jpg](voicebox_closeup_rf_connector.jpg): U.FL and antenna feed at the bottom.
* [voicebox_housing_label.jpg](voicebox_housing_label.jpg), [voicebox_label.jpg](voicebox_label.jpg), [voicebox_label_closeup.jpg](voicebox_label_closeup.jpg): back cover and rating label.
