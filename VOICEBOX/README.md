# Verisure voicebox GW-AUD-VB (research)

Photo-based hardware survey and reverse-engineering plan for the Verisure voicebox **GW-AUD-VB**
(before DMM/bench measurements). Bench measurements go in
[docs/re_worksheet.md](docs/re_worksheet.md); the cross-device overview is in
[documentation/device_survey.md](../documentation/device_survey.md).

Everything below comes from the photos and visual PCB inspection. "Confirmed" means the marking,
silkscreen, or physical part can be read directly; everything else is marked as an assumption to
verify on the bench.

## Unit data (from the label)

| Field | Value |
| :--- | :--- |
| Model | GW-AUD-VB |
| Serial (device ID) | 2AK2N9BR |
| Part No | 121900 |
| HW / SW | 6.0 / 18.00 |
| Date (YYMMDD) | 160521 |
| Maker | Panasonic Corporation for Securitas Direct AB, Made in Japan, CE 0682 |
| Battery | 3× `CR-2/3AZ` (3.0 V Li-MnO2, Panasonic `SH384552520`), three separate bays in the back cover |

## Main parts

| Ref | Marking | Identification | Status |
| :--- | :--- | :--- | :--- |
| MCU | `TI 61A47XTG4 M430F5435A REV H` (LQFP-80 PN) | **TI MSP430F5435A** (MSP430F5xx, 192 KB flash, 16 KB RAM). Pin 1 at bottom-left (white PCB dot in `voicebox_closeup_msp430.jpg`); pins 13–14 (`XIN`/`XOUT`) route directly to `DA609C`; pins 71–76 (JTAG/SBW/RST) sit directly opposite the front pogo pad cluster. Full PN pinout: [bench_recipes.md](../documentation/bench_recipes.md) | Confirmed |
| MCU 32 kHz crystal | `DA609C` (2-pad metal can) | 32.768 kHz XT1 crystal routed to MSP430 pins 13–14 (`P7.0/XIN`, `P7.1/XOUT`; pins 11–12 are `AVCC`/`AVSS`) | Visually traced |
| Sub-GHz radio | `44601B BCL01Z 1534` (QFN-20, 4×4 mm) | **Silicon Labs Si4460** rev B1 sub-GHz transceiver. Pin 1 dot at bottom-right (`voicebox_board_front.jpg`); pins 11–15 (`nIRQ`, `SCLK`, `SDO`, `SDI`, `nSEL`) on left edge; pins 16–17 (`XOUT`/`XIN`) route directly down to the metal crystal | Very likely; confirm on the bench |
| Si4460 crystal | faint (`26...`?) 4-pad metal can | Silicon Labs AN785 recommends 26 MHz for 868 MHz designs (family default 30 MHz) | To measure |
| `WLAG` (SOT-23-5) | `WLAG` | Next to Si4460 RF network; `WLA` matches ABLIC/SII `S-1315` LDO, or could be an RF switch | Measure voltage & continuity |
| DSP / DECT SoC | `DSP Group DCX81ACPCFAE4FNC-PB ©2010 DSPG P3XT74.0G.A` (LQFP) | **DSP Group DCX81**, DECT/ULE voice SoC (audio codec, speakerphone DSP, DECT radio), surrounded by an unpopulated RF shield frame | Confirmed marking; confirm DECT RF role with SDR |
| DCX81 crystal | `D138` (4-pad metal can, left of DCX81) | **13.824 MHz** DECT reference crystal (12 × 1.152 MHz DECT bit clock) | Visually read; verify on bench |
| Flash next to DCX81 | `winbond 25Q16DWSIG 1602` (SOIC-8) | Winbond **W25Q16DW**, 16 Mbit (2 MiB) **1.8 V ONLY** SPI NOR flash (1.65–1.95 V, abs max 2.4 V): likely DCX81 firmware and voice prompts | Confirmed marking (**do not apply 3.3 V!**) |
| Mic preamp (back, top) | `2732 / 613 / JRC` (MSOP-8 / VSP8) | **NJM2732** (Nisshinbo/JRC) dual rail-to-rail low-noise 1.8–6.0 V op-amp directly below the MEMS mic | Visually read; confirm by continuity |
| MSOP-8 on back (right of top) | `4G34 / 54663` (tentative) | Next to DECT U.FL `F1` and `SP1 539` — possible audio amplifier or regulator | Unknown; trace on bench |
| Battery switches (back, ×3) | `SP1 539` (×3, 4/6-pin DFN along right edge) | Three identical parts paired with SMD fuses `F1`/`F2`/`F3` (marked `N`) and diodes (`M`) — almost certainly per-bay OR-ing / load-switch / reverse-polarity protection for the 3× `CR-2/3AZ` cells | Assumption; confirm by continuity |
| SOT-23 transistors (×5) | `64` (3 right of MSP430, 2 near top battery, 1 near Si4460) | Digital transistors or MOSFETs (Panasonic/Rohm `64` family) | Unknown |
| 6-pin parts near MSP430 (×2) | `21` (SOT-363 / SC-70-6) | Dual transistor / level shifter / logic gate near MSP430 & PIR connector | Unknown |
| Microphone | gold MEMS can with acoustic port, top of back side | Analog MEMS microphone (feeds `NJM2732`) | Confirmed part type |
| PIR | white Fresnel dome + daughterboard `M1821` (3 leads, R/W/B wires to back 4-pin header) | Passive infrared motion sensor | Confirmed assembly |
| Speakers | 2× `M36BN20-A 8Ω 0.5W` | 36 mm 8 Ω speakers in front cover, wired to the two 2-pin `W B` JST headers (pads `45` and `28`) | Confirmed |
| Output capacitor | `p3 330 6V` (330 µF / 6.3 V) | Reservoir capacitor next to `F3`/`F4` and `+` pad `44` | Confirmed |
| Battery connectors (×3) | three identical 2-pin white JST headers along front right edge (`01`, `03`, `04`) | Match the three `CR-2/3AZ` (`SH384552520`) bays in the back cover | Confirmed layout; confirm parallel vs split rails |
| U.FL connectors | two: `F1` (back top, DCX81/DECT) and `2` (back bottom, Si4460/868 MHz) | Inline RF test points for the two radios | Confirmed |
| Wire antennas | long wire (top-front / bottom-back, Si4460) + short L-wire (bottom-front / top-back, DCX81) | Long ≈ 868 MHz (λ/4 ≈ 86 mm); short ≈ 1.9 GHz DECT (λ/4 ≈ 39 mm) | Visually traced; measure exact lengths |
| FPC connector | 24-pin ZIF on front | Connects to front panel (buttons / LEDs) | Assumption |
| Tact switch | next to FPC connector and pads `49`/`02`/`60` | Tamper switch (back cover / wall bracket) or pairing button | Confirm on bench |
| PCB | `K1969-1` | Panasonic board number | Confirmed |

## Radio

Two radios are fitted:

- **Si4460** (sub-GHz, long wire antenna, U.FL `2`): almost certainly the Verisure 868.950 MHz alarm
  link (same PHY as the door sensor and siren — confirm with gateway `sniff` and Saleae SPI capture).
- **DCX81** (DECT 1.88–1.90 GHz in Europe, `13.824 MHz` crystal `D138`, short wire antenna, U.FL
  `F1`): full DECT RF front-end is populated (antenna + U.FL + shield footprint), strongly suggesting
  it handles the two-way DECT voice link to the hub in addition to speakerphone DSP. Confirm with an
  SDR at 1.88–1.90 GHz during a voice session.

## Debug / programming

No labelled header is printed. Candidates:

- **Pogo pad cluster** on the front (16 large round pads in a staggered 2-column grid next to the
  FPC connector: labelled `02, 41, 42, 51–61, 66, 67`, directly behind the MSP430F5435A). This is
  almost certainly the factory programming/test jig carrying MSP430 4-wire JTAG (`PJ.0–PJ.3`, pins
  72–75), `TEST/SBWTCK` (pin 71), `RST/NMI/SBWTDIO` (pin 76), UART BSL (`P1.1` TX pin 18 / `P1.2`
  RX pin 19), and power/GND. The photo overlay
  [voicebox_overlay_msp430_on_front.jpg](docs/images/voicebox_overlay_msp430_on_front.jpg)
  projects the MSP430 pins onto the front: the chip sits under the upper half of the cluster, and
  pins 71–76 fall on the pad row next to the labels `51`/`54`/`56` (accuracy about ±1.5 mm, about
  one pad row), so probe those pads first. The pads reach the pins through vias, so the exact
  pad-to-pin mapping still needs continuity.
- **Numbered test pads around the MSP430** on the back: `70, 71, 72, 76, 77, 79, 101, 102, 104, 105`.
  In `voicebox_closeup_msp430.jpg`, pads `70/71/72/76/77/79` sit on a bundle of 6–8 parallel traces
  that leave the pin 21–40 edge (lower half, roughly `P1.4`–`P2.4`) and run towards the Si4460
  corner, so they are most likely Si4460 control lines (`nIRQ`, `SDN`, `GPIO0–3`); the hardware SPI
  candidates `UCB0` are pins 35–38. Pads `104/105` sit by pins 41–50 (`P3.6`/`P3.7`/`P4.x`). The
  overlay [voicebox_overlay_si4460_on_back.jpg](docs/images/voicebox_overlay_si4460_on_back.jpg)
  projects the Si4460 onto the back: it lands on the 3×3 via field of its exposed pad, with pad `77`
  over the chip and pads `70`/`72` just above its crystal side, which supports the Si4460 reading.
  The board is multilayer with 0.5 mm pin pitch, so the exact pad-to-pin mapping needs continuity or
  a logic capture.
- **MSP430F5435A programming:** supports both 4-wire JTAG and 2-wire Spy-Bi-Wire (`SBWTCK` = pin 71,
  `SBWTDIO` = pin 76). Unlike the siren's MSP430F149, a TI LaunchPad eZ-FET (SBW) *does* support the
  F5435A. The F5435A JTAG lock is an electronic fuse (a lock signature in flash), not a physical
  fuse, and its UART BSL password is the 32-byte interrupt
  vector table — **never try the BSL** on this unit (wrong password mass-erases the flash). Details:
  [documentation/bench_recipes.md](../documentation/bench_recipes.md).
- **W25Q16DW external flash:** **1.8 V ONLY** — requires a 1.8 V level shifter (e.g. TXB0104 /
  TXS0108) or a 1.8 V-VCCIO FT232H, never a direct 3.3 V clip.

## Reverse-engineering plan

1. **Phase 0 (Passive DMM, batteries disconnected):** Confirm how the 3× `CR-2/3AZ` connectors
   (`01`/`03`/`04`) and `SP1 539` / `F1`–`F3` combine onto the supply rails; map the front pogo pad
   cluster to MSP430 pins 71–76 (JTAG/SBW/RST) and `P1.1`/`P1.2` (pins 18/19, BSL); map back test pads `70–79` /
   `101–105` to the Si4460 SPI pins (pins 1, 11–15); trace the speakers (`W B` at `45`/`28`) to the
   audio amplifier.
2. **Phase 1 (Active read-only at 3.0 V via Otii Arc / PS3005D):** Measure rail voltages (`DVCC`,
   `Si4460 VDD`, `W25Q16DW VCC` ≈ 1.8 V, `WLAG`, 330 µF cap) and idle/PIR/TX current; listen on
   868.950 MHz (`sniff`/SDR) and sniff MSP430 ↔ Si4460 SPI at boot with Saleae Logic 4; decode
   the export with `python tools/si446x_spi_decode.py capture.csv` (carrier, data rate, deviation,
   sync word, CRC and TX/RX payloads; see
   [bench_recipes.md](../documentation/bench_recipes.md)).
3. **Phase 2 (External flash & JTAG):** Dump `W25Q16DW` at **1.8 V** (level-shifted FT232H +
   `flashrom`) and inspect for DCX81 firmware / audio prompts; check MSP430 JTAG/SBW lock state once
   a TI FET / LaunchPad eZ-FET is available. SBW needs only `TEST` (pin 71), `RST` (pin 76), `DVCC`
   and `GND`, so thin wires on the IC pins work before the pogo cluster is mapped. If JTAG is open, a
   flash dump reveals every pin's function from the port setup code and replaces most of the
   continuity mapping.
4. **Phase 3 (OpenVerisure firmware):** Evaluate custom MSP430F5435A + Si4460 firmware (868.950 MHz
   AES-128 KDF protocol, PIR motion + tamper + battery + local speaker chimes/voice prompts).

## Photo requests

New photos that would identify the remaining unreadable parts. How to take them is in
[documentation/device_survey.md](../documentation/device_survey.md#photo-requests). Tick an item
when the photo is in `docs/images/`.

- [x] **Batteries:** confirmed `3× CR-2/3AZ` (3.0 V Li-MnO2, `SH384552520`).
- [x] **DCX81 crystal:** read as `D138` (13.824 MHz DECT).
- [x] **SOIC/MSOP-8 next to the microphone:** read as `2732 / 613 / JRC` (`NJM2732` dual op-amp).
- [ ] The **Si4460 crystal** (below `44601B` on the front): straight-on macro to read the exact frequency marking.
- [ ] The **MSOP-8 on the back, right** (`4G34 / 54663` tentative): raking light to confirm exact part number.
- [ ] Under the **two black foam squares** on the back, if they can be lifted without damage.
- [ ] The **other end of the FPC cable** (front panel board: buttons, LEDs) and its markings.
- [ ] Both **antenna wires** in full length, with a ruler (to confirm exact lengths).

## Photos

Photos with GPS/EXIF metadata removed and downscaled to 3264 px live in [docs/images/](docs/images/):

* [docs/images/voicebox_board_front.jpg](docs/images/voicebox_board_front.jpg): front (component) side — Si4460, pogo pads, FPC, 3 battery JSTs, PIR, W25Q16DW, DCX81.
* [docs/images/voicebox_board_front_in_housing.jpg](docs/images/voicebox_board_front_in_housing.jpg): front side in the housing over the speakers.
* [docs/images/voicebox_board_back.jpg](docs/images/voicebox_board_back.jpg): back side — MEMS mic, NJM2732, `SP1 539` (×3), MSP430F5435A, U.FL `F1` & `2`, antennas.
* [docs/images/voicebox_board_and_speakers.jpg](docs/images/voicebox_board_and_speakers.jpg): board lifted, speaker wiring to the two `W B` JST headers.
* [docs/images/voicebox_speakers_side.jpg](docs/images/voicebox_speakers_side.jpg): speakers and PIR board `M1821`, side view.
* [docs/images/voicebox_closeup_msp430.jpg](docs/images/voicebox_closeup_msp430.jpg), [docs/images/voicebox_closeup_msp430_bottom.jpg](docs/images/voicebox_closeup_msp430_bottom.jpg): the MSP430F5435A, `DA609C` 32.768 kHz crystal, and test pads `70–79`/`101–105`.
* [docs/images/voicebox_closeup_dcx81_flash.jpg](docs/images/voicebox_closeup_dcx81_flash.jpg): the DCX81, `D138` 13.824 MHz crystal, and W25Q16DW.
* [docs/images/voicebox_closeup_winbond.jpg](docs/images/voicebox_closeup_winbond.jpg): the W25Q16DW 1.8 V SPI NOR flash and speaker JST `28`.
* [docs/images/voicebox_closeup_rf_connector.jpg](docs/images/voicebox_closeup_rf_connector.jpg): U.FL `2` and 868 MHz antenna feed at the bottom of the back side.
* [docs/images/voicebox_overlay_msp430_on_front.jpg](docs/images/voicebox_overlay_msp430_on_front.jpg), [docs/images/voicebox_overlay_si4460_on_back.jpg](docs/images/voicebox_overlay_si4460_on_back.jpg): derived overlays. The board is flipped about its horizontal axis; the mapping `x_back = x_front − 10`, `y_back = 3361 − y_front` (full-resolution pixels) was fitted on the short-antenna slot, the right-edge notch and the PIR cut-out, and is accurate to about ±40 px (±1.5 mm). The MSP430 pins (back) are drawn on the front and the Si4460 (front) on the back.
* [docs/images/voicebox_housing_label.jpg](docs/images/voicebox_housing_label.jpg), [docs/images/voicebox_label.jpg](docs/images/voicebox_label.jpg), [docs/images/voicebox_label_closeup.jpg](docs/images/voicebox_label_closeup.jpg): back cover (3× `CR-2/3AZ` bays) and rating label.
