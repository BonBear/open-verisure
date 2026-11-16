# Measurement log: the flash board (LED BOARD P/N3302042 C)

Goal: find out what the 8 pins do before anything is driven. All measurements here are passive (ohms and volts), so nothing is driven.

## What we already know

- The board sits in the main board's **J2** and **J3** (4 pins each). On the flash board itself the connectors are called **J1** and **J2**.
- CC1110 **P0_7** (pin 13) goes to the main board's **J2-4** via ~6 kΩ. P0_7 = 1 is currently called "aux rail" in the firmware and is switched on together with the Blackfin.
- On the back there are **U2 MCP6042** (two op-amps), **Q3/Q6** and two **1R0** resistors. These are two constant-current channels with 2 LEDs each. The current per channel is roughly *control voltage / 1 Ω*: 0.5 V on the control input gives ~0.5 A.
  **Therefore:** a control pin must never be driven directly to 3.3 V before we know what sits in front of the op-amp's input (it could be several amperes).
- On the tab above the lens sits the light sensor (6 legs). The Blackfin's TWI bus sees a device at **0x5C**, which fits a BH1750 with ADDR high. Pin order for the BH1750 (WSOF6): 1 VCC, 2 ADDR, 3 GND, 4 SDA, 5 DVI, 6 SCL. Check against what is printed on the IC.
- **C3** (47 µF tantalum) is next to connector J1 on the flash board, probably on the LED supply.

## Part A: flash board removed, unpowered

### A1. The main board's J2/J3 against known nets (ohms, bench power supply OFF)

Pin 1 = square pad / marked 1.

| Pin | to GND | to VBAT pad | to 3.3 V (J5-1) | to CC1110 pin (P0_7 pin 13, P0_3 pin 9, P0_1 pin 6, P0_6 pin 12, P2_0 pin 14) | Note |
| :--- | :--- | :--- | :--- | :--- | :--- |
| J2-1 | | | | | |
| J2-2 | | | | | |
| J2-3 | | | | | |
| J2-4 | | | | P0_7 ~6 kΩ (known) | |
| J3-1 | | | | | |
| J3-2 | | | | | |
| J3-3 | | | | | |
| J3-4 | | | | | |

P0_3 is interesting: it is connected somewhere (the current rose 74→92 mA when it was driven high) but we do not know where.

### A2. The flash board's J1/J2 against the components (ohms)

| Pin on the flash board | Light sensor legs (1–6) | U2 legs (3 and 5 = control inputs, 4 = GND, 8 = VCC) | LED anodes / C3 | Note |
| :--- | :--- | :--- | :--- | :--- |
| J1-1 | | | | |
| J1-2 | | | | |
| J1-3 | | | | |
| J1-4 | | | | |
| J2-1 | | | | |
| J2-2 | | | | |
| J2-3 | | | | |
| J2-4 | | | | |

Also note which of the flash board's connectors sits in the main board's J2 and which in J3.

## Part B: flash board fitted, voltage (volts to GND)

Measure on the main board's side of the connectors (or on the flash board's pins from above).

| Pin | Idle (rails off, the board draws ~3 mA) | Camera running (after motion in front of the PIR, ~0.17 A) | Note |
| :--- | :--- | :--- | :--- |
| J2-1 | | | |
| J2-2 | | | |
| J2-3 | | | |
| J2-4 | | | |
| J3-1 | | | |
| J3-2 | | | |
| J3-3 | | | |
| J3-4 | | | |

## After the measurement

When we know which pin controls the current (and what sits in front of the op-amp), we choose the control:
- from the CC1110 (if the control pin goes there), or
- from the Blackfin (if it goes via TWI/GPIO, in which case the flash is controlled exactly at the exposure).
The first test is done with the bench power supply's current limit low and a short pulse.
