# Siren: bench worksheet

Fill in on the bench. Leave a cell empty when it has not been measured; write `n/a` when it does
not apply. Background and part list: [README.md](README.md). Procedures, pinouts (MSP430F149 JTAG
pins 54–58, CC1101) and the list of actions that can erase the unit:
[documentation/bench_recipes.md](../documentation/bench_recipes.md). Decode an SPI capture with
`python tools/cc1101_spi_decode.py capture.csv --xtal-mhz 27`.

Unit / device ID: `2S5F 554X`  Date: `2026-11-22`  Tools: DMM, USB microscope

Safety: the horn is very loud. Disconnect CN2 (piezo horn) or cover it before triggering anything.
Power the board from the bench supply through one of the battery inputs (CN5/CN6/CN7 or TP1A `+` /
TP68 `GND`) at 3.0 V with a current limit. **Do NOT apply 3.0 V to CN2** (CN2 is the piezo horn
output from the H-bridge D1/D2).

## 1. Unit data (from the rating label)

| Field | Value |
| :--- | :--- |
| Model | `GW-SI` |
| Serial (device ID) | `2S5F 554X` (`2S5F554X`) |
| Part No | `131155` |
| HW / SW | `HW 8.0` / `SW 4.00` |
| Date | `2014-11-17` (`141117`) |
| Maker | Panasonic Corporation, Japan (for Securitas Direct AB) |
| Battery type and voltage | 3x `CR-2/3AZ` (`SH384552520`) in parallel, `3 V DC` (0 Ω across all `+` and all `-` contacts) |

## 2. Power

| Point | Expected | Measured | Notes |
| :--- | :--- | :--- | :--- |
| Battery input (`CN5`/`CN6`/`CN7` -> `TP1A`/`TP1B`/`TP1C` `+`, `TP68` `GND`) | `3.0 V` (3x parallel) | `3.0 V` (Otii Arc) | Confirmed: `DVCC`/`AVCC` (IC1 pins 1/64) connect to `TP1A`/`TP1B`/`TP1C`; `CN2` is the piezo horn, not battery |
| MSP430F149 DVCC | 1.8–3.6 V | `3.0 V` | Measured at `IC1` pin 1 |
| CC1101 VDD | 1.8–3.6 V | `3.0 V` | Measured at `IC3` pin 4 |
| Boost output (IC9 + 10 µH `100` inductor, 2x 4.7 µF / 50 V) | | | High-voltage piezo horn supply |
| TP1 / TP6 | | | |
| TP4 / TP7 | | | |
| Idle current | | `6.66 µA` floor (`p50`); `~16 µA` steady-state mean after boot | Otii 91 s @ 4 kHz: 5 boot pulses (`320 ms`, `27.2–27.9 mA` peak), then `20.5 ms` @ `1.93 mA` wake every `4000.0 ms` |
| Current while listening (RX) | ~15 mA continuous, or short WOR pulses | `27.2–27.9 mA` peak during 5x `320 ms` boot pulses | |
| Current while sounding | | | |

Scope the supply current at idle: periodic RX pulses mean wake-on-radio. Write the period here:
**`4000.0` ms** (locked to 32.768 kHz `EADH` crystal; min `3999.0` ms, max `4001.0` ms).

## 3. Clocks

| Part | Marking | Expected | Measured |
| :--- | :--- | :--- | :--- |
| XT1 (CC1101) | `26.000` | 26.000 MHz (use `--xtal-mhz 26` for SPI decode) | Confirmed marking |
| XT2 (next to the OKI chip) | `400W` | ~4.00 MHz ceramic resonator (OKI ML22 clock) | Confirmed marking |
| MSP430 LFXT1 (`EADH` can?) | `EADH` | 32.768 kHz | |
| XT3 (IC4 area, unpopulated) | not fitted | | |

## 4. MSP430F149 debug access

The F149 (64-pin) has 4-wire JTAG on dedicated pins (TDO/TDI, TDI/TCLK, TMS, TCK, plus RST/NMI).
It has no TEST pin, so JTAG stays available until the fuse is blown. It also has a ROM BSL on P1.1
(TX) / P2.2 (RX), entered with a RST/TCK sequence.

Candidate pads: TP61, TP62, TP63, TP64 (4-pad group next to IC1), TP70, TP71, TP72, TP31, TP32,
TP81/TP82 and TP84–TP88 on the back (next to CN1), TP91/TP92 on the back.

| Pad | Idle level (V) | Goes to (IC1 pin) | Function |
| :--- | :--- | :--- | :--- |
| TP61 | | **54** | **JTAG `TDO/TDI`** |
| TP62 | | **55** | **JTAG `TDI/TCLK`** |
| TP63 | | **56** | **JTAG `TMS`** |
| TP64 | | **57** | **JTAG `TCK`** |
| TP5 | `3.0 V` | **58** | **JTAG `RST/NMI`** (pull-up confirmed HIGH) |
| TP68 | | **62, 63** (and **53** `XT2IN`) | **`GND` (`AVSS`/`DVSS`)** |
| TP1A / TP1B / TP1C | | **1, 64** | **`VCC` (`DVCC`/`AVCC`, via F11–F13)** |
| TP28 | | **13** (`P1.1`) | **BSL TXD** (`P1.1`) |
| *(no TP pad)* | | **22** (`P2.2`) | **BSL RXD** (`P2.2`) — direct wire to IC1 pin 22 required |
| TP27 | | **12** (`P1.0`) | `P1.0` |
| TP17 | | **20** (`P2.0`) | `P2.0` |
| *(trace)* | | **19** (`P1.7`) | **`IC9` boost enable** |
| TP37 | | **2** (`P6.3`) | `P6.3` / A3 |
| TP38 | | **3** (`P6.4`) | `P6.4` / A4 |
| TP39 | | **4** (`P6.5`) | `P6.5` / A5 |
| TP19 | | **6** (`P6.7`) | `TH1` thermistor (`P6.7` / A7) |
| TP1 | | **10** (`VeREF+`) | `VeREF+` |
| TP70–TP72, TP31–TP32 | | | |
| TP81–TP82, TP84–TP88, TP91–TP92, TP94 | | | |

| Step | Result |
| :--- | :--- |
| Tool (4-wire JTAG only — TI MSP-FET, Olimex MSP430-JTAG-TINY-V2, or ESP32 SLAU320; LaunchPad eZ-FET is SBW-only and cannot talk to F149) | **ESP32-C3 Super Mini** (`tools/esp32_msp430_reader`, TI SLAU320, 330 Ω series resistors) |
| Device ID read | Blocked by blown JTAG fuse |
| JTAG fuse blown? | **Yes** (`ERR FUSE_BLOWN` — `DR_Shift16(0xAAAA)` returned `0x5555`, 2026-11-24) |
| Flash dump via JTAG (file, size, SHA-256) | `n/a` (JTAG security fuse blown at factory) |
| **Do not try the BSL**: a wrong password mass-erases the flash | ☑ read and understood (BSL can be used later to mass-erase and flash custom firmware) |

## 5. CC1101 (IC3) link to the MSP430

| CC1101 pin | MSP430 pin | Notes |
| :--- | :--- | :--- |
| SCLK (pin 1) | **47** (`P5.3 / UCLK1`) | Hardware SPI on USART1 |
| SI (pin 20) | **45** (`P5.1 / SIMO1`) | Hardware SPI on USART1 |
| SO / GDO1 (pin 2) | **46** (`P5.2 / SOMI1`) | Hardware SPI on USART1 |
| CSn (pin 7) | **44** (`P5.0 / STE1`) | Hardware SPI CS on Port 5 |
| GDO0 (pin 6) | **26** (`P2.6`) | Port 2 interrupt-capable |
| GDO2 (pin 3) | **27** (`P2.7`) | Port 2 interrupt-capable |

Sniffed on 2026-11-24 (Saleae Logic 4 @ 12 MS/s, Ch0=`SCLK`, Ch1=`MOSI`, Ch3=`CSn`, SPI Mode 3 `CPOL=1/CPHA=0`, 465 transactions / 1045 MOSI bytes, decoded with `--xtal-mhz 26`):
- **Boot sequence:** `SRES` -> 47-byte burst init (`0x00..0x2E`) -> `SPWD`, then 5x CCA (`SRX` + `RSSI` poll) + 20-byte TX bursts (`STX`) + RX window + `SPWD`.
- **Frequencies (26 MHz XTAL):** Init `868.059 MHz` (`FREQ=0x2162F4`), boot bursts alternate `868.157 MHz` (`0x2163EC`) and `868.448 MHz` (`0x2166E0`).
- **RF profile:** GFSK, `38.38 kbps` (`MDMCFG4=0xCA`, `MDMCFG3=0x83`), `19.04 kHz` deviation (`DEVIATN=0x34`), `101.6 kHz` RX BW, sync `0xD391` (`MDMCFG2=0x17` TX/RX), 12-byte preamble (`MDMCFG1=0x52`), whitening + CRC on (`PKTCTRL0=0x44` TX / `0x45` RX).
- **PATABLE (8-step ramp, `FREND0=0x17`):** `20 03 0D 34 57 8E 85 C4` (~+10 dBm max, matched to PCB balun/antenna).
- **GDO pins:** `GDO2` (`P2.7`) = `0x06` / `0x46` (sync/packet IRQ), `GDO0` (`P2.6`) = `0x00` / `0x0F` / `0x42`.
- **Boot TX packet (20 B):** `13 10 00 06 06 D1 8C FF FF FF FF 89 04 0E 76 84 08 0B 08 04` (incrementing seq byte at index 2).

## 6. OKI L22901 (IC2) and the horn driver

| Item | Result |
| :--- | :--- |
| Full marking (`L22901-707 OKI JAPAN 3285F01J`) | OKI L22901-707 parallel audio/tone synthesizer LSI (`-707` ROM code) — **Confirmed**; `IC2` pin 12 -> `TP13`, pin 20 -> `TP12`, pin 23 -> `TP18`, pin 26 -> `R90` -> `Q3` -> `TP2` |
| Boost regulator (`IC9`) | Enable = **`IC1` pin 19 (`P1.7`)**; `IC9` pins 3, 5, 6 = `TP2` = `IC5` pin 8 (`V+`) |
| Interface to the MSP430 (parallel command pins) | `P6.0..P6.5` -> IC2 pins `13..18` (falling-edge strobe on `P6.4` / `TP38`); `P1.4` (`TP2`) audio rail (active-LOW); `P1.7` `IC9` HV boost (active-HIGH) |
| Output to the horn (direct or through IC5 / the Q15–Q19 stage) | H-bridge (`Q15–Q19`, 2x `4F` + 2x `4A` SOT-89) -> `D1`/`D2` -> `CN2` (`アカ`/`クロ`) |
| IC5 `2115 E58 JRC` | NJM2115 dual op-amp: `V+` (pin 8) tied to `TP2` (switched/fed with `IC9` pins 3,5,6 & `Q3`) |
| Q15/Q16/Q17 ... `4F`, `4A`, `LG` marked transistors: H-bridge? | Confirmed complementary H-bridge (2x `4F` + 2x `4A` + `LG`/`L6` level-shifters + 4x `5.1 Ω`) |
| Horn type and impedance | **Piezo horn** (`OL` open-circuit resistance across disconnected `CN2` leads) |

**Confirmed IC2 pinout & strobe protocol:**
- **Power rails:** `P1.4` (IC1 pin 16, TP2) enables the audio/op-amp rail (`LOW` = ON, `HIGH` = OFF). `P1.7` (IC1 pin 19) enables IC9 high-voltage horn boost (`HIGH` = ON; keep `LOW` on bench when Saleae Logic 4 is attached).
- **Parallel bus (`P6.0..P6.5` -> IC2 pins `13..18`):**
  - `P6.0` (IC2 pin 13): `STOP` / power-down (`val5 = 0x01` strobed stops playback, ~55 ms ramp-down to sleep).
  - `P6.1` (IC2 pin 14): Must be `0` for valid sound commands (`1` = no-op / empty bank).
  - `(P6.5, P6.3, P6.2)` (IC2 pins `18, 16, 15`): 3-bit sound index `1..7` (`P6OUT` bit mapping for 5-bit `val5`: `((val5 & 0x10U) << 1) | (val5 & 0x0FU)`).
  - `P6.4` (IC2 pin 17, TP38): Falling-edge strobe (`STARTB`/`STB`): drive target bits with `P6.4 = 1` for 10 ms setup, then drive `P6.4 = 0` to latch and trigger.

**Confirmed 7-sound table (`val5`):**

| `val5` | Index `(P6.5, P6.3, P6.2)` | Sound / behavior |
| :--- | :--- | :--- |
| `0x04` | `001` | 2-beep chime (~440 ms, auto-stops) |
| `0x08` | `010` | ~2112 Hz decaying tone (~560 ms, auto-stops) |
| `0x0C` | `011` | 2-tone chime 1122 Hz -> 1046 Hz (~650 ms, auto-stops) |
| `0x10` | `100` | Powered-up idle / silence (>2.5 s, +1.1 mA) |
| `0x14` | `101` | Full-amplitude continuous alarm siren burst (~2.15 s, auto-stops; re-trigger every 2 s for continuous alarm) |
| `0x18` | `110` | Ding-dong bell 2007 Hz -> 1514 Hz (~1.8 s, auto-stops) |
| `0x1C` | `111` | 1-beep chime (~200 ms, auto-stops) |

## 7. Connectors, switches and sensors

| Item | Pins | Measured / traced | Function |
| :--- | :--- | :--- | :--- |
| CN5 (3-pin, fuse F11) | 3 | `TP1A` -> IC1 pin 1/64 (`DVCC`/`AVCC`) | **Battery cell 1** (3.0 V parallel) |
| CN6 (3-pin, fuse F12) | 3 | `TP1B` -> IC1 pin 1/64 (`DVCC`/`AVCC`) | **Battery cell 2** (3.0 V parallel) |
| CN7 (3-pin, fuse F13) | 3 | `TP1C` -> IC1 pin 1/64 (`DVCC`/`AVCC`) | **Battery cell 3** (3.0 V parallel) |
| CN1 (back, castellated, TP84–TP87) | | | |
| CN2 (back, 2-pin red/black) | 2 | `アカ` (red) -> `D2` out; `クロ` (black) -> `D1` out | **Piezo horn** (H-bridge output, NOT battery!) |
| CN4 U.FL + FIL1 (SAW) | | | RF test point |
| SW1 (back) | 2 | **IC1 pin 18 (`P1.6`)** + `GND` | **Tamper switch** (active-low to GND) |
| TH1 thermistor (back) | 2 | **IC1 pin 6 (`P6.7 / A7`, `TP19`)** + `BAT+` | **Temperature sensing** (ADC channel A7) |
| LED1 | 2 | **IC1 pin 43 (`P4.7`)** -> `Q1` (pin 2 `GND`) | **Status LED** (active-high via `Q1`) |
| "BANE4-" contact (spring) | | | battery / chassis spring? |
| IC7, IC4 (unpopulated) | | | option footprints |

## 8. Radio

| Item | Result |
| :--- | :--- |
| Frequency, modulation, data rate | `868.059 / 868.157 / 868.448 MHz`, GFSK, `38.38 kbps`, `19.04 kHz` dev, `101.6 kHz` BW, sync `0xD391`, PATABLE `20 03 0D 34 57 8E 85 C4` |
| Does the siren transmit a heartbeat? Interval | 5x boot TX bursts (`20 B`), then `4000.0 ms` wake ticks (`20.5 ms` @ `1.93 mA`) |
| Command from the hub to trigger it (capture with the gateway) | `n/a` (will run custom OpenVerisure firmware) |
| ACK from the siren after a command? | `n/a` (will run custom OpenVerisure firmware) |
| Device ID in plaintext in the header? | Factory boot packet: `13 10 00 06 06 D1 8C FF FF FF FF ...` |

## 9. Open questions

- [x] 26 or 27 MHz crystal for the CC1101? → **`26.000` MHz** (confirmed both on marking and SPI decode `--xtal-mhz 26`).
- [x] Wake-on-radio or continuous RX? → Timed `4000.0 ms` wake ticks (`6.66 µA` floor, `20.5 ms` @ `1.93 mA` idle ticks; 5x `320 ms` @ `27.5 mA` CCA+TX boot bursts).
- [x] Is the F149 JTAG fuse blown? → **Yes** (confirmed 2026-11-24 via ESP32-C3 SLAU320 reader: `ERR FUSE_BLOWN` / `0x5555`).
- [x] Which pads are JTAG / BSL? → **JTAG:** `TP61` (`TDO`), `TP62` (`TDI`), `TP63` (`TMS`), `TP64` (`TCK`), `TP5` (`RST`), `TP68` (`GND`). **BSL:** `TP28` (`P1.1` TXD), `IC1` pin 22 (`P2.2` RXD — no TP pad, direct wire), `TP5` (`RST`), `TP64` (`TCK`), `TP68` (`GND`).
- [ ] Which `IC1` pins drive `IC2` / H-bridge audio? (To be probed once custom ROM BSL test firmware runs.)
