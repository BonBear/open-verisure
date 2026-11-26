# GW-AUD-VB voicebox: bench worksheet

Fill in on the bench. Leave a cell empty when it has not been measured; write `n/a` when it does
not apply. Background and part list: [README.md](../README.md). Procedures, pinouts and the list of
actions that can erase the unit: [documentation/bench_recipes.md](../../documentation/bench_recipes.md).

Unit: 2AK2N9BR  Date: ________  Tools: ______________________

Power it from the bench supply (Otii Arc or PS3005D) at **3.0 V** through one of the battery JST
contacts (current limit ~200 mA to start; audio playback / DECT TX may need ~500 mA).

## 1. Power

| Point | Expected | Measured | Notes |
| :--- | :--- | :--- | :--- |
| Battery type | `3× CR-2/3AZ` (3.0 V Li-MnO2, `SH384552520`) | Confirmed (`3× CR-2/3AZ`) | 3 separate bays in back cover |
| Batteries: parallel OR-ed or split rails? | 3.0 V d.c. + 3× `SP1 539` + `F1`/`F2`/`F3` suggests parallel OR-ed | | Check continuity across `01`, `03`, `04` JSTs |
| MSP430 DVCC1/2/3/4 (pins 16, 51, 31, 67) | 1.8–3.6 V (likely ~3.0 V) | | All four should be equal |
| MSP430 AVCC (pin 11) | = DVCC (max 0.3 V difference) | | |
| MSP430 VCORE (pin 49, 470 nF cap) | ~1.4–1.9 V (PMM core level 0–3) | | Shows which core level the firmware runs; never load this pin |
| Si4460 VDD (pins 6, 8) | 1.8–3.6 V (likely ~3.0 V) | | |
| DCX81 core / IO rails | 1.2 V core; separate QSPI IO domain 1.8 V or 3.3 V (Synaptics brief) | | QSPI IO should read 1.8 V to match the W25Q16DW |
| W25Q16DW VCC (pin 8) | **1.65–1.95 V** (1.8 V ONLY!) | | Confirm before any SPI hookup |
| Output of `WLAG` (SOT-23-5) | LDO (~1.0–3.0 V) or RF switch? | | |
| Rail at the 330 µF cap (`p3 330 6V`, `+` pad `44`) | ~3.0 V or boosted? | | |
| Idle current (3.0 V) | µA range | | |
| Current with the PIR triggered | | | |
| Current while transmitting 868 MHz | ~15–30 mA | | |
| Current during a voice session / audio playback | | | |

## 2. Clocks

| Part | Marking | Expected | Measured |
| :--- | :--- | :--- | :--- |
| Crystal next to the Si4460 (pins 16–17) | faint | 26 MHz (AN785 for 868 MHz) or 30 MHz | |
| MSP430 XT1 (pins 13–14, `P7.0/XIN`, `P7.1/XOUT`) | `DA609C` | 32.768 kHz | |
| MSP430 XT2 (pins 69–70, `P5.2/XT2IN`, `P5.3/XT2OUT`) | none visible | n/a, or used as GPIO | Check whether anything is connected |
| DCX81 crystal (left of DCX81) | `D138` | 13.824 MHz (standard DECT = 12 × 1.152 MHz) | |

## 3. Pogo pad cluster (front, next to the FPC)

Number each pad as printed (`02, 41, 42, 51–61, 66, 67`). Idle level with the board powered;
"goes to" by continuity with the supply off.

Verified **MSP430F5435A (LQFP-80 PN)** debug pins (Pin 1 = bottom-left white dot in
`voicebox_closeup_msp430.jpg`; pins 61–80 run top-to-bottom along the left edge):
- Pin **71**: `TEST/SBWTCK`
- Pin **72**: `PJ.0/TDO`
- Pin **73**: `PJ.1/TDI/TCLK`
- Pin **74**: `PJ.2/TMS`
- Pin **75**: `PJ.3/TCK`
- Pin **76**: `RST/NMI/SBWTDIO`
- Pin **18** / **19**: `P1.1` / `P1.2` = BSL TXD / RXD (do **not** invoke BSL!)

Photo overlay (`docs/images/voicebox_overlay_msp430_on_front.jpg`, about ±1.5 mm): pins 71–76
fall on the pad row next to the labels `51`/`54`/`56`, so beep those pads (and one row either side)
against pins 71–76 first. SBW needs only pins 71 and 76 plus `DVCC`/`GND`, so the JTAG test below
can use thin wires on the IC pins before this table is filled in.

| Pad | Idle level (V) | Goes to (IC + pin) | Function |
| :--- | :--- | :--- | :--- |
| 02 | | | |
| 41 | | | |
| 42 | | | |
| 51 | | | |
| 52 | | | |
| 53 | | | |
| 54 | | | |
| 55 | | | |
| 56 | | | |
| 57 | | | |
| 58 | | | |
| 59 | | | |
| 60 | | | |
| 61 | | | |
| 66 | | | |
| 67 | | | |
| 49 (next to tact switch) | | | |

### MSP430 JTAG / SBW test

| Step | Result |
| :--- | :--- |
| Tool (MSP-FET / LaunchPad eZ-FET SBW / mspdebug) | |
| Device ID read | |
| JTAG locked? (F5xx electronic fuse: lock signature in flash) | |
| Flash dump (file, size, SHA-256) | |
| **Do not try the BSL**: a wrong password mass-erases the flash | ☐ read and understood |

## 4. Back test pads (`70–79`, `101–105`) & Si4460 SPI

Verified **Si4460 (QFN-20)** pins (Pin 1 dot = bottom-right in `voicebox_board_front.jpg`; left edge
top-to-bottom = pins 11–15):
- Pin **1**: `SDN` (shutdown, active high)
- Pin **11**: `nIRQ`
- Pin **12**: `SCLK`
- Pin **13**: `SDO` (MISO)
- Pin **14**: `SDI` (MOSI)
- Pin **15**: `nSEL` (CSn)
- Pins **9 / 10 / 19 / 20**: `GPIO0` / `GPIO1` / `GPIO2` / `GPIO3` (may carry RX data, TX/RX
  switch control or clock out)

MSP430F5435A serial-port candidates (USCI) for the Si4460 and DCX81 links:

| USCI | Pins (PN) | Signals |
| :--- | :--- | :--- |
| `UCB0` / `UCA0` | 35, 36, 37, 38 | `P3.0` STE/`UCA0CLK`, `P3.1` SIMO/SDA, `P3.2` SOMI/SCL, `P3.3` `UCB0CLK`/`UCA0STE` |
| `UCA0` UART/SPI | 39, 40 | `P3.4` TXD/SIMO, `P3.5` RXD/SOMI |
| `UCB1` / `UCA1` | 41, 42, 54, 55 | `P3.6` STE/`UCA1CLK`, `P3.7` SIMO/SDA, `P5.4` SOMI/SCL, `P5.5` `UCB1CLK`/`UCA1STE` |
| `UCA1` UART/SPI | 56, 57 | `P5.6` TXD/SIMO, `P5.7` RXD/SOMI |

Photo trace hint (`voicebox_closeup_msp430.jpg`): pads `70/71/72/76/77/79` sit on a bundle of 6–8
traces leaving the pin 21–40 edge (lower half, roughly `P1.4`–`P2.4`) towards the Si4460 corner, so
they are probably Si4460 control lines rather than the SPI bus. Pads `104/105` sit by pins 41–50.
The Si4460 overlay (`docs/images/voicebox_overlay_si4460_on_back.jpg`) puts pad `77` over the chip
and pads `70`/`72` next to it. Confirm each row by continuity or a Saleae capture, and decode the
capture with `python tools/si446x_spi_decode.py capture.csv`.

| Pad (back, around MSP430) | MSP430 pin | Si4460 pin (or other net) | Function |
| :--- | :--- | :--- | :--- |
| 70 | | | |
| 71 | | | |
| 72 | | | |
| 76 | | | |
| 77 | | | |
| 79 | | | |
| 101 | | | |
| 102 | | | |
| 104 | | | |
| 105 | | | |
| Si4460 `SDN` (pin 1) | | | |
| Si4460 `GPIO0`–`GPIO3` (pins 9, 10, 19, 20) | | | |

## 5. DCX81 and its 1.8 V flash (`W25Q16DW`)

| Item | Result |
| :--- | :--- |
| W25Q16DW VCC (pin 8) verified at ~1.8 V | |
| W25Q16DW JEDEC ID (expected `EF 60 15`) | |
| Flash dumped (**1.8 V level-shifted only!**) file, size, SHA-256 | |
| Content (DCX81 code, audio prompts, strings) | |
| DCX81 JTAG / UART test pads found (`03`/`100`/`108` or bottom pads `13`/`17`/`60`?) | |
| Link between the MSP430 and the DCX81 (UART / SPI, which pins) | |
| Link between the MSP430 and the Si4460 (SPI pins, `nIRQ`, `SDN`) | |

## 6. Unknown parts

| Ref / location | Marking | Package | Nets / connected parts | Identified as |
| :--- | :--- | :--- | :--- | :--- |
| `WLAG` (front, next to Si4460) | `WLAG` | SOT-23-5 | | Candidate `S-1315` LDO or RF switch |
| Mic preamp (back, top) | `2732 / 613 / JRC` | MSOP-8 (VSP8) | Below MEMS mic | **NJM2732** dual RRIO op-amp (confirm) |
| Back right of top (near U.FL `F1`) | `4G34 / 54663` (tentative) | MSOP-8 | | |
| Battery switches (back, ×3) | `SP1 539` | DFN-4/6 | Paired with `F1`/`F2`/`F3` (`N`) & `M` diodes | Candidate per-bay battery OR-ing / load switch |
| 6-pin near MSP430 (×2) | `21` | SOT-363 | | |
| SOT-23 parts (×5) | `64` | SOT-23 | | |
| Black foam squares on back (×2) | — | — | Mechanical spacers or parts underneath? | |

## 7. Connectors and peripherals

| Item | Pins | Measured / traced | Function |
| :--- | :--- | :--- | :--- |
| FPC connector (front) | 24-pin ZIF | | Front panel (buttons / LEDs)? |
| Top 2-pin JST (near `110`/`01`/`F3`) | 2 | | Battery bay 1 (`CR-2/3AZ`) |
| Middle 2-pin JST (near `03`/`108`/`F2`) | 2 | | Battery bay 2 (`CR-2/3AZ`) |
| Bottom 2-pin JST (near `04`/`F1`) | 2 | | Battery bay 3 (`CR-2/3AZ`) |
| Upper 2-pin `W B` JST (pad `45`) | 2 | Confirmed visually | Speaker 1 (`M36BN20-A 8Ω 0.5W`) |
| Lower 2-pin `W B` JST (pad `28`) | 2 | Confirmed visually | Speaker 2 (`M36BN20-A 8Ω 0.5W`) |
| 4-pin connector, back (R/B/W wires) | 4 (3 used) | Confirmed visually | PIR board `M1821` |
| PIR output: MSP430 pin | — | | |
| MEMS microphone: analog → `NJM2732` → ? | — | | |
| Tact switch (front, pad `49`) | 2 | | Tamper or pairing button? |
| U.FL `F1` (back top) | 1 | Visually traced to DCX81 | 1.9 GHz DECT RF test point |
| U.FL `2` (back bottom) | 1 | Visually traced to Si4460 | 868 MHz sub-GHz RF test point |
| Long wire antenna length (mm) | — | Visually traced to Si4460 | 868 MHz (expect ~80–90 mm) |
| Short wire antenna length (mm) | — | Visually traced to DCX81 | 1.9 GHz DECT (expect ~35–42 mm) |

## 8. Radio

| Item | Result |
| :--- | :--- |
| 868 MHz: frequency, modulation, data rate | |
| Seen by the gateway (`sniff`) with the door sensor settings? | |
| Packet length, first bytes, device ID (`2AK2N9BR`) in plaintext? | |
| Supervision/heartbeat interval | |
| DECT 1.88–1.90 GHz activity during a voice session? | |
| What starts a voice session (alarm, hub, button)? | |

## 9. Open questions

- [ ] Are the three `CR-2/3AZ` battery bays parallel OR-ed to a single 3.0 V rail or split across MCU vs DCX81?
- [ ] Which front pogo pads carry MSP430 JTAG/SBW (`pins 71–76`)?
- [ ] Do back pads `70–79` / `101–105` expose the Si4460 SPI bus (`SCLK`, `SDI`, `SDO`, `nSEL`, `nIRQ`, `SDN`) for easy Saleae sniffing?
- [ ] Is the MSP430 JTAG locked (electronic fuse)?
- [ ] What is stored in the 1.8 V `W25Q16DW` (DCX81 firmware + voice prompts)?
- [ ] Is the 868 MHz link the same PHY and packet format as the door sensor and siren?
- [ ] Is the DCX81 used as a DECT radio or only as the voice DSP?
