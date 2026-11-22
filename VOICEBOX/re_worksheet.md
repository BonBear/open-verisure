# GW-AUD-VB voicebox: bench worksheet

Fill in on the bench. Leave a cell empty when it has not been measured; write `n/a` when it does
not apply. Background and part list: [README.md](README.md). Procedures, pinouts and the list of
actions that can erase the unit: [documentation/bench_recipes.md](../documentation/bench_recipes.md).

Unit: 2AK2N9BR  Date: ________  Tools: ______________________

Power it from the bench supply at 3.0 V through the battery contacts (current limit ~200 mA to start;
the audio amplifier may need more when it plays).

## 1. Power

| Point | Expected | Measured | Notes |
| :--- | :--- | :--- | :--- |
| Batteries: in series or in parallel? (3 V d.c. on the label suggests parallel) | | | |
| Battery type (CR123A? size) | | | |
| MSP430 DVCC | 1.8–3.6 V | | |
| Si4460 VDD | 1.8–3.6 V | | |
| DCX81 core / IO rails | | | |
| W25Q16DW VCC (must be ~1.8 V) | 1.65–1.95 V | | |
| Output of `WLAG` (SOT-23-5) | | | |
| Rail at the 330 µF cap | | | |
| Idle current | µA range | | |
| Current with the PIR triggered | | | |
| Current while transmitting 868 MHz | | | |
| Current during a voice session | | | |

## 2. Clocks

| Part | Marking | Expected | Measured |
| :--- | :--- | :--- | :--- |
| Crystal next to the Si4460 | | 26 MHz (AN785 for 868 MHz) or 30 MHz | |
| MSP430 XT1 (`DA60C`-type can near the MCU) | | 32.768 kHz | |
| MSP430 XT2 (if fitted) | | | |
| DCX81 crystal | | 10.368 MHz or 13.824 MHz (DECT) | |

## 3. Pogo pad cluster (front, next to the FPC)

Number each pad as printed. Idle level with the board powered; "goes to" by continuity with the
supply off.

| Pad | Idle level (V) | Goes to (IC + pin) | Function |
| :--- | :--- | :--- | :--- |
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
| 61 | | | |
| 66 | | | |
| 67 | | | |
| | | | |
| | | | |

MSP430F5435A pins to look for (LQFP-80): TEST/SBWTCK, RST/NMI/SBWTDIO, TDO, TDI, TMS, TCK
(PJ.0–PJ.3), BSL TXD/RXD (P1.6/P1.5), DVCC, DVSS. Exact pin numbers are unverified; see
[documentation/bench_recipes.md](../documentation/bench_recipes.md).

### MSP430 JTAG / SBW test

| Step | Result |
| :--- | :--- |
| Tool (MSP-FET / eZ-FET / LaunchPad SBW / mspdebug) | |
| Device ID read | |
| JTAG fuse blown? | |
| Flash dump (file, size, SHA-256) | |
| **Do not try the BSL**: a wrong password mass-erases the flash | ☐ read and understood |

## 4. DCX81 and its flash

| Item | Result |
| :--- | :--- |
| W25Q16DW JEDEC ID (should be EF 60 15) | |
| Flash dumped (clip at 1.8 V!) file, size, SHA-256 | |
| Content (code, audio prompts, strings) | |
| DCX81 JTAG / UART test pads found | |
| Link between the MSP430 and the DCX81 (UART / SPI, which pins) | |
| Link between the MSP430 and the Si4460 (SPI pins, nIRQ, SDN) | |

## 5. Unknown parts

| Ref / location | Marking | Package | Nets / connected parts | Identified as |
| :--- | :--- | :--- | :--- | :--- |
| `WLAG` | WLAG | SOT-23-5 | | |
| SOIC-8, back top (near the microphone) | | | | |
| MSOP-8, back right | | | | |
| `SP1 6E9` | | | | |
| `SP1 51C` | | | | |
| 6-pin `21` near the MSP430 | | | | |
| `B4`, `64`, `6P` SOT parts | | | | |
| Black foam squares on the back (2x): parts underneath? | | | | |

## 6. Connectors and peripherals

| Item | Pins | Measured / traced | Function |
| :--- | :--- | :--- | :--- |
| FPC connector (front) | | | front panel? |
| 2-pin JST next to pad 110 | | | |
| 2-pin JST (W/B) at pad 45 | | | speaker 1? |
| 2-pin JST (W/B) at pad 28 | | | speaker 2? |
| 2-pin JST at the bottom | | | |
| 4-pin connector, back (R/B/W wires) | | | PIR board M1821 |
| PIR output: MSP430 pin | | | |
| MEMS microphone: analog or PDM, goes to | | | |
| Tact switch: function | | | |
| U.FL top: which radio | | | |
| U.FL bottom: which radio | | | |
| Antenna wire 1 length / radio | | | |
| Antenna wire 2 length / radio | | | |

## 7. Radio

| Item | Result |
| :--- | :--- |
| 868 MHz: frequency, modulation, data rate | |
| Seen by the gateway with the door sensor settings? | |
| Packet length, first bytes, device ID in plaintext? | |
| Supervision/heartbeat interval | |
| DECT 1.88–1.90 GHz activity during a voice session? | |
| What starts a voice session (alarm, hub, button)? | |

## 8. Open questions

- [ ] Is the DCX81 used as a DECT radio or only as the voice DSP?
- [ ] Which pogo pads are the MSP430 JTAG/SBW?
- [ ] Is the MSP430 JTAG fuse blown?
- [ ] What is stored in the W25Q16DW (voice prompts)?
- [ ] Is the 868 MHz link the same PHY and packet format as the door sensor?
