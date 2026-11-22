# Siren: bench worksheet

Fill in on the bench. Leave a cell empty when it has not been measured; write `n/a` when it does
not apply. Background and part list: [README.md](README.md).

Unit / device ID: ________  Date: ________  Tools: ______________________

Safety: the horn is very loud. Disconnect it or cover it before triggering anything. Power the board
from the bench supply through CN2 (red/black) with a current limit.

## 1. Unit data (from the label, not photographed yet)

| Field | Value |
| :--- | :--- |
| Model | |
| Serial (device ID) | |
| Part No | |
| HW / SW | |
| Date | |
| Battery type and voltage | |

## 2. Power

| Point | Expected | Measured | Notes |
| :--- | :--- | :--- | :--- |
| CN2 battery voltage (red `アカ` / black `クロ`) | | | |
| MSP430F149 DVCC | 1.8–3.6 V | | |
| CC1101 VDD | 1.8–3.6 V | | |
| Boost output (IC9 + 100 µH `100` inductor, 2x 47 µF) | | | horn supply? |
| TP1 / TP6 | | | |
| TP4 / TP7 | | | |
| Idle current | | | Wake-on-radio? |
| Current while listening (RX) | ~15 mA continuous, or short WOR pulses | | |
| Current while sounding | | | |

Scope the supply current at idle: periodic RX pulses mean wake-on-radio. Write the period here:
______ ms.

## 3. Clocks

| Part | Marking | Expected | Measured |
| :--- | :--- | :--- | :--- |
| XT1 (CC1101) | | 27.000 MHz like the door sensor, or 26.000 MHz | |
| XT2 (next to the OKI chip) | | | |
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
| TP61 | | | |
| TP62 | | | |
| TP63 | | | |
| TP64 | | | |
| TP70 | | | |
| TP71 | | | |
| TP72 | | | |
| TP31 | | | |
| TP32 | | | |
| TP81 | | | |
| TP82 | | | |
| TP84 | | | |
| TP85 | | | |
| TP86 | | | |
| TP87 | | | |
| TP88 | | | |
| TP91 | | | |
| TP92 | | | |
| TP94 | | | |

| Step | Result |
| :--- | :--- |
| Tool (MSP-FET / mspdebug + FET / BSL script) | |
| Device ID read | |
| JTAG fuse blown? | |
| Flash dump via JTAG or BSL (file, size, SHA-256) | |
| BSL password (= vector table) available? | |

## 5. CC1101 (IC3) link to the MSP430

| CC1101 pin | MSP430 pin | Notes |
| :--- | :--- | :--- |
| SCLK | | |
| SI | | |
| SO / GDO1 | | |
| CSn | | |
| GDO0 | | |
| GDO2 | | |

Sniff the SPI at power-up with the logic analyser to capture the register setup (FREQ2..0, MDMCFG,
SYNC1/0, PKTCTRL, WORCTRL, MCSM). Register dump: ______________________

## 6. OKI L22901 (IC2) and the horn driver

| Item | Result |
| :--- | :--- |
| Full marking (`L22901-707 OKI JAPAN 3285F01J`) | OKI ML22 family speech-synthesis LSI (assumed); `-707` is probably the ROM/voice code |
| Interface to the MSP430 (serial command pins) | |
| Output to the horn (direct or through IC5 / the Q15–Q19 stage) | |
| IC5 `2115 E58 JRC` | NJM2115 dual op-amp (assumed): buffer/amplifier |
| Q15/Q16/Q17 ... `4F`, `4A`, `LG` marked transistors: H-bridge? | |
| Horn type and impedance | |

## 7. Connectors, switches and sensors

| Item | Pins | Measured / traced | Function |
| :--- | :--- | :--- | :--- |
| CN5 (3-pin, fuse F11) | | | |
| CN6 (3-pin, fuse F12) | | | |
| CN7 (3-pin, fuse F13) | | | |
| CN1 (back, castellated, TP84–TP87) | | | |
| CN2 (back, 2-pin red/black) | | | battery |
| CN4 U.FL + FIL1 (SAW) | | | RF test point? |
| SW1 (back) | | | tamper switch? |
| TH1 thermistor (back) | | | temperature / battery compensation |
| LED1 | | | |
| "BANE4-" contact (spring) | | | battery spring? |
| IC7, IC4 (unpopulated) | | | option footprints |

## 8. Radio

| Item | Result |
| :--- | :--- |
| Frequency, modulation, data rate | |
| Does the siren transmit a heartbeat? Interval | |
| Command from the hub to trigger it (capture with the gateway) | |
| ACK from the siren after a command? | |
| Device ID in plaintext in the header? | |

## 9. Open questions

- [ ] 26 or 27 MHz crystal for the CC1101?
- [ ] Wake-on-radio or continuous RX?
- [ ] Is the F149 JTAG fuse blown?
- [ ] Which pads are JTAG / BSL?
- [ ] What does the downlink command look like?
