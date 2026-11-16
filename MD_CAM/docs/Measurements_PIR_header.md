# PIR board header – measurement plan

Goal: find where the camera's idle floor (~1.16 mA in PM2, see `TODO.md`) goes, and whether
the PIR board has a supply line the CC1110 could switch off.

The PIR board is soldered to the main board through a 10-pin header (the row of pins just below
J5, above the lens; see `images/crops/PXL_20261022_194026345_thumb.jpg`). It cannot be unplugged,
so its current cannot be measured directly. Above the header are four test pads: three unlabeled
and `ALARM` (CC1110 P1_7).

**Pin numbering here:** viewed from the component side as in the photo, pin 1 is the **leftmost**
header pin (battery-spring side) and pin 10 the rightmost.

## Measurement 1: voltages in idle (DMM to GND)

Board #1 powered from the Otii Arc at 3.0 V, our firmware, Blackfin off, alarm off, nobody in
front of the PIR. Black probe on GND (the `GND` pad next to `VBAT`).

| Pin | Idle voltage (V) | Beeps to VBAT pad? | Beeps to GND? | Note |
| :-- | :-- | :-- | :-- | :-- |
| 1 | 0 | | yes | GND |
| 2 | 0 | | | CC1110 pin 6 = P0_1 (34 Ω), driven low |
| 3 | 0.055 | | | CC1110 pin 12 = P0_6 (34 Ω), driven low but sinking current |
| 4 | 0.055 | | | CC1110 pin 13 = P0_7 (34 Ω), driven low but sinking current |
| 5 | 0 | | yes | GND |
| 6 | 0.04–2.78, changing | | | = ALARM (P1_7); the PIR was triggering (gateway: motion ON) |
| 7 | 0 | | yes | GND |
| 8 | 0 | | yes | GND |
| 9 | 0 | | yes | GND |
| 10 | 3.0 | | | VBAT: the PIR board's supply |
| Pad left | 0 | | | |
| Pad middle | 0.054 | | | probably the same net as pin 3 or 4 |
| Pad right | 0.054 | | | probably the same net as pin 3 or 4 |
| ALARM pad | = pin 6 | | | |

Continuity (beep) only with the power **off**.

What to look for:
- VBAT and GND pins: the PIR's supply. If the supply goes through a transistor on the main board
  first, that transistor may be switchable from the CC1110.
- A pin at a level between 0 and VBAT (e.g. 0.3–2 V): a likely leak path (current through a
  pull-up or into an unpowered part).
- Pins that beep to CC1110 pins P0_1 (pin 6), P0_3 (pin 9), P0_6 (pin 12), P2_0 (pin 14):
  the LED driver Q2 on the PIR board (memory bank `PIR_INDICATOR_LED`).

## Results (2026-11-16, Board #1)

- Header: pins 1, 5, 7, 8, 9 = GND; 2 = P0_1; 3 = P0_6; 4 = P0_7; 6 = ALARM (P1_7);
  10 = VBAT (the PIR board's supply, straight from the battery, no switch the CC1110 controls).
- Pins 3 and 4 have pull-ups to VBAT on the PIR board: 5.77 kΩ (P0_6) and 3.7 kΩ (P0_7),
  measured to pin 10 with the power off. Driven low they sat at 0.055 V and sank ~0.9 mA.
- Otii, P0_6 + P0_7 in PM2: driven low 1.164 mA floor, tri-state 0.299 mA, driven high
  0.299 mA. LD1 does not light in any state and the PIR keeps working. The firmware now drives
  both high while the Blackfin is off.
- While the PIR is triggered (ALARM low), the CC1110's internal pull-up on P1_7 adds ~0.15 mA.
