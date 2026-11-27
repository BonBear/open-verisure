# GW-SD3 smoke detector: bench worksheet

Bench measurements for the Verisure **GW-SD3** smoke detector. Background and part list:
[README.md](../README.md). Procedures, pinouts and erase warnings:
[documentation/bench_recipes.md](../../documentation/bench_recipes.md).

Unit used: ☑ `3EB7KFZQ` (reference units `3EA8VKXK` and `3EA8W6D6` kept untouched)  
Date: `2026-11-25/26`  
Tools: DMM, Otii Arc (3.0 V / 0.2 A), USB microscope, Raspberry Pi Debug Probe (CMSIS-DAP v2 + `pyOCD 0.46.1`), OpenVerisure Gateway (`lolin_s2_mini_eval` `sniff all`).

## 1. Power

| Point | Expected | Measured | Notes |
| :--- | :--- | :--- | :--- |
| Battery type / marking | Lithium | `1× CR-2/3AZ` (3.0 V Li-MnO2) | Same cell type as SIREN `GW-SI` |
| Battery connector (`BAT+` / `BAT-`) | 2-pin JST (back) | **`01`, `03`, `29` = `BAT+`**; **`02`, `04`, `27` = `BAT-` (`GND`)** | `01` & `02` are the two large pads under the base jig holes |
| `VCC` pad to GND | ~3.0 V | **3.0 V** | Always-on rail to EFR32 |
| `VCA` pad to GND | switched rail | **0 V** (in idle) | Power-gated analog/audio rail |
| `IC6` (`W25Q16JL`) `VCC` (pad `60`) | 2.3–3.6 V | **0 V** (in idle) | Power-gated; with the EFR32 held in reset it stays at 0 V, so feed pad `60` (or `IC6` pin 8) directly for a dump |
| `R1` + `R56` (2× `270` = 27 Ω) | series/inrush | **14 Ω** to `BAT+` (27 Ω ∥ 27 Ω = 13.5 Ω) | |
| Best accessible GND pad | — | **`02`** (large round pad), also `04`, `27` | |

## 2. Clocks

| Part | Marking | Expected | Status |
| :--- | :--- | :--- | :--- |
| HFXO next to EFR32 | `3840T` | 38.400 MHz | Visually traced to EFR32 pins 10–11 (`HFXTAL_N`/`_P`) |
| LFXO (brown 2-pad) | `A320L` | 32.768 kHz | Visually traced to EFR32 pins 35–36 (`PB14`/`PB15`) |

## 3. Debug header (8 pogo pads on the back)

| Pad | Assumed | Goes to (EFR32 pin) | Confirmed function |
| :--- | :--- | :--- | :--- |
| `CLK` | `SWCLK` | **EFR32 pin 1 (`PF0`)** | **Confirmed `SWCLK`** |
| `DIO` | `SWDIO` | **EFR32 pin 2 (`PF1`)** | **Confirmed `SWDIO`** |
| `RES` | `RESETn` | **EFR32 pin 12 (`RESETn`)** | **Confirmed `RESETn`** |
| `SW` | `SWO` or switch | **EFR32 pin 25 (`PA0`)** | **Not `SWO`** (`PF2`); GPIO input (likely test switch) |
| `TXD` | UART TX | TBD | |
| `RXD` | UART RX | TBD | |
| `TPR` | tamper | TBD | |
| `FIRE` | alarm signal | TBD | |

### SWD connection test (strictly read-only, 2026-11-26)

| Step | Result |
| :--- | :--- |
| Probe used | Raspberry Pi Debug Probe (CMSIS-DAP v2, `D` port: Orange=`CLK`, Yellow=`DIO`, Black=`GND`) + `pyOCD 0.46.1` (200 kHz, 3.0 V from Otii Arc) |
| `DPIDR` | **`0x2BA01477`** (ARM Cortex-M4 SW-DP, rev 2) |
| `AP 0 IDR` / `AP 255 IDR` | **`0x26E60011`** (Silicon Labs EFR32 Series 1 **AAP** — Authentication Access Port) |
| Debug lock state | **DEBUG-LOCKED** (factory lock active; no AHB-AP exposed). Unlockable via `AAP DEVICE_ERASE` with the Raspberry Pi Debug Probe when ready for custom firmware |
| Main flash dump possible? | No (debug-locked; stopped read-only without erasing) |

## 4. External SPI flash (`IC6`, Winbond `W25Q16JL`, 16 Mbit / 2 MiB)

All 6 SPI/power signals are exposed on large round test pads on the **back** of the PCB (no SOIC-8 clip needed):

| `IC6` pin | Signal | Back PCB pad | Notes |
| :---: | :--- | :--- | :--- |
| 1 | `/CS` | **Pad `54`** | Confirmed by DMM continuity |
| 2 | `DO` (MISO) | **Pad `51`** | Confirmed by DMM continuity |
| 3 | `/WP` | **Pad `60`** | Tied to `VCC` |
| 4 | `GND` | **`GND`** (`02`/`04`/`27`) | Confirmed by DMM continuity |
| 5 | `DI` (MOSI) | **Pad `53`** | Confirmed by DMM continuity |
| 6 | `CLK` | **Pad `52`** | Confirmed by DMM continuity |
| 7 | `/HOLD` | **Pad `60`** | Tied to `VCC` |
| 8 | `VCC` | **Pad `60`** | 0 V in idle (power-gated); `W25Q16JL` is 2.3–3.6 V |

### Dump result (Unit 1 `3EB7KFZQ`, 2026-11-26)

| Item | Result |
| :--- | :--- |
| Setup | FT232H (`AD0` CLK, `AD1` DI, `AD2` DO, `AD3` /CS, `GND`) on `IC6` pins; board at 3.0 V from Otii Arc on `BAT+`; `RES` tied to `GND`; 3.0 V fed directly to `IC6` pin 8 |
| JEDEC ID | `EF 40 15` (flashrom: `W25Q16.V`) |
| Reads | 3 identical reads at 1 MHz and 100 kHz; SHA-256 `010d8bc5dc0c86e46c3ed4cd1ab7dc94b2eccf3af7bb6012ebaef77ed2c47901` (image kept off-repo) |
| Layout | `0x000000`–`0x0007FF`: 16 × 128-byte sound sequence records, each up to 16 big-endian 32-bit `(start, end)` pairs; top byte = flags (`0x80` = tone/silence segment, `0x12`/`0x32` = audio clip) |
| Audio | 19 distinct clips (~200 KB) between `0x016100` and `0x17AFFF`, ~1.5–13 KB each; repeated 8-byte tone/silence pattern at `0x17B000`–`0x183FFF`; byte values cluster in nibbles 2–5; not IMA ADPCM (an IMA decode clips heavily) |
| Empty | `0x184000`–`0x1FFFFF` erased (`FF`) |
| Firmware | **None**: no GBL header, no Cortex-M vector table, no readable strings. Stock application exists only inside the locked EFR32 |

Bench pitfalls: without `RES` low the EFR32 keeps running (periodic ~223 mA battery load-test pulses through `R1`∥`R56`) and drives the bus; without direct VCC the flash returned `FF`/floating data; one early read had bit-shifted bytes from a loose clip, so always compare at least two reads.

## 5. Identified & mapped ICs / peripherals

| Ref | Marking | Package | Measured / traced | Confirmed function |
| :--- | :--- | :--- | :--- | :--- |
| `IC301` | `304A 029J 363` | QFN-32 | Pins 13+14 → Pad `43` → `CN301` speaker; Pins 15+16 → Pad `42` → `CN301` speaker; pins 27/28 → `IC81` pins 12/13; pins 19/20/29 → `IC81` pin 14 net; pins 21/22/30/31 → EFR32 `PB11`/`PA4`/`PB12`/`PB13`; pin 25 → EFR32 `AVDD`; pins 5/6/10/11 → `IC6` `DO`/`CLK`/`DI`/`/CS`; pin 12 → diode (anode) → EFR32 `RESETn`; pins 7/9 → back pads `47`/`46`; pins 3/17/18/23 `GND` | **Smoke controller with BTL speaker driver** (sequences the chamber, drives `M36BN20-A` 8 Ω speaker on `CN301`) |
| `IC81` | `C4660 224KU` | 16-pin | Pin 14 = 1 Ω to EFR32 `PB13` (+18 kΩ `PA1`, 13 kΩ `PA2`); pin 15 VCC; pin 16 = `LED81` cathode; pins 9–11 = `PD81`; pins 12/13 → `IC301` pins 27/28; pin 14 → `IC301` pins 19/20/29; pins 6/7/8 → `IC301` pins 1/2/4 (recheck, the old count was one pin off); pins 1/5 no link; pins 2–4 ~100 kΩ to several nets | **Optical chamber front end** (LED driver + photodiode input) |
| `LED81` | IR LED (chamber) | 2-pin TH | Anode via a diode to `BAT+`, cathode 0 Ω to `IC81` pin 16 (earlier note "EFR32 pin 5 `PF4`" superseded) | **Smoke chamber IR emitter** |
| Load-test MOSFET | not read (near the PCB code `K2226`) | SOT-23 (3-pin) | Gate ← EFR32 `PC6` (pin 43) via 9 kΩ; source GND; drain → `R1`/`R56` (13.5 Ω to `BAT+`) | **Battery load-test switch** (~222 mA) |
| Test button | — | — | GND ↔ EFR32 pin 25 (`PA0`) via 22 Ω; `PA0` pulled up externally | **Test button, active low** |
| `PD81` | Photodiode (chamber) | 3-pin TH | Connected to **`IC81`** (`C4660`) | **Smoke chamber optical receiver** |
| `CP92` | `CP92` | DFN-8 w/ window | Microscope confirms rectangular open sensing window over silicon die (Sensirion SHT3x / Si70xx family) | **Digital temperature & humidity sensor** |
| `CN301` | 3-pin JST | — | Outer pins (black + white wire, center empty) → Pads `43`/`42` → `IC301` pins 13–16 | **Speaker connector** (`M36BN20-A`, 8 Ω 0.5 W) |
| `341 ZF` | `341 ZF` | 6-pad metal lid | Between EFR32 pins 13–16 (`SUBGRF`) and U.FL / wire antenna | **EPCOS/TDK 868 MHz SAW bandpass filter** |

## 6. Radio (bench sniff, 2026-11-26)

| Item | Result |
| :--- | :--- |
| Gateway sniff (`lolin_s2_mini_eval`, `sniff all`) | **Confirmed!** Caught **3 boot packets** on `CC1101#1` (**868.950 MHz, 2-FSK, 38.4 kBaud, sync `D3 91`**) during 3.0 V power-on (`+0.0 s`, `+4.5 s`, `+3.5 s`) |
| Signal strength & freq offset | **RSSI -55 to -56 dBm**, LQI 51–57, freq error `-39.7 kHz` |
| Captured raw 20-byte frames | `EC F1 {0D,19,05} 91 FF DD 84 DB 15 85 2D B0 B1 DB BF F4 4C 62 28 DF` (identical 20-byte boot/supervision header; byte 2 varies) |
| Hardware RF compatibility | **100 % compatible** with the OpenVerisure 868.950 MHz CC1101 gateway |

## 7. Stock-firmware logic sniff (Unit 1, 2026-11-26)

Continuity traced on the bench (EFR32 QFN-48 pin numbers):

| Net | Traced connection |
| :--- | :--- |
| `CP92` power | `CP92` pins 2+3 ← 3-pin load switch (`YK`, above `TP23`) output; switch input = VCC (EFR32 pin 34), switch control ← EFR32 pin 3 (`PF2`). Switch output reads 0 V in idle (sensor power-gated). |
| `CP92` data | `CP92` pin 8 → EFR32 pin 4 (`PF3`); pins 9+10 GND; pins 1, 4–7 NC. Single data line → not I2C. |
| `IC301` control | `IC301` pin 21 → EFR32 pin 31 (`PB11`); pins 19+20 VCC. |
| `IC301` ↔ `IC6` (DMM 2026-11-27) | `IC301` pin 5 → `IC6` pin 2 (`DO`) 0 Ω; pin 6 → `IC6` pin 6 (`CLK`) 0 Ω; pin 10 → `IC6` pin 5 (`DI`); pin 11 → `IC6` pin 1 (`/CS`) 47 Ω; pin 32 → `IC6` pin 4 (`GND`) 1 Ω. `IC301` is the SPI master and reads the sound clips from `IC6` itself. |
| `IC301` → EFR32 reset (DMM 2026-11-28) | `IC301` pin 12 → diode anode; diode cathode (marking `A`, next to `IC6`) → pad `32` → debug pad `RES` = EFR32 `RESETn` (pin 12). Forward 0.6 V; no resistor to `GND` on either side (`RES` and pin 12 read OL; the earlier 2 kΩ was a probe short). `IC301` can only pull `RES` high, never reset the EFR32; most likely a pull-down on `RES` (factory jig or debugger) pulls pin 12 low so `IC301` sees it. |
| `IC301` other pins (DMM recheck 2026-11-28) | Pins 3, 17, 18, 23 = `GND`; pins 19+20 (joined by a trace) and 29 → `IC81` pin 14 (the `PB13` net); pin 21 → EFR32 pin 31 (`PB11`); **pin 22 → EFR32 pin 29 (`PA4`)**; **pin 25 → EFR32 pin 34 (`AVDD`)** (through a 0 Ω resistor; supply, not `GND`); **pin 30 → EFR32 pin 32 (`PB12`)**; **pin 31 → EFR32 pin 33 (`PB13`)**; pins 27/28 → `IC81` pins 12/13; pins 13/14 and 15/16 = speaker (`CN301`); pin 7 → back pad `47`, pin 9 → back pad `46`; pins 8 and 26 no link found; pin 24 open. The earlier `GND` readings on pins 25/30/31 and the `IC81` map on pins 28/29 came from a probe short and an off-by-one count. |
| `IC81` | Pin 14 → EFR32 `PB13` (1 Ω); full pin map in section 5. |

Saleae Logic 4 @ 12 MS/s, board on Otii (3.0–3.3 V on `BAT+`), test button pressed during the capture:

| Signal | Observed |
| :--- | :--- |
| `PB11` → `IC301` pin 21 | High for ~13 s after power-on; high for ~1.1 s on a test-button press; otherwise a 10.6 µs high pulse every 4.000 s (phase restarts after the button event). |
| Speaker line (`CN301`, Logic 4 Ch0 analog) | Idles at 0 V (amp off). ~1 s after `PB11` drops it steps to VCC for 30.5 ms, then is released and decays. No audio tone without the speaker fitted. |
| `CP92` pin 8 (`PF3`) | Single ~45 ms high pulse ~2 s after the button press, slow edges. Same timing as the speaker step, so treat as unconfirmed. |
| `IC6` pin 6 (CLK) | No activity during boot, idle or test: the test sound does not come from `IC6`. |
| `LED81` pads | Static; no IR pulse seen at the probed pad. |

Pitfall: several early captures were flat because the probe clips had no contact; verify each Saleae lead (tap it on `BAT+`) before a long capture.

## 7a. Optical chamber mapping (2026-11-27)

`-DCHAMBER_SCAN` probe on Unit 1 (120 µs `PC6` pulse, ADC on 24 GPIOs, `PD13`/`PD15` enable combinations): no GPIO reacts to the pulse except AVDD (~65 mV sag) and the `PC7` ADC row, which follows `PC6` although `PC7` reads GND on the DMM (APORT mapping suspect), because `PC6` only switches the battery load-test MOSFET (`R1`∥`R56`, ~222 mA), not the IR LED.

Saleae Logic 4 on stock `3EA8VKXK` at 3.0 V (Ch0 analog `IC81` pin 16, Ch1 `PB13`, Ch2 `PB11`, Ch3 `IC81` pin 12):

| Signal | Observed |
| :--- | :--- |
| `PB11` (`IC301` pin 21) | 10 µs high pulse every 4.000 s. |
| `IC81` pin 12 (from `IC301` pin 28) | 2.54 ms high window 6.4 ms after every second `PB11` pulse (8 s period). |
| `IC81` pin 16 (`LED81` cathode) | Idles ~2.18 V; drops to ~1.25 V for 80–90 µs about 2.1 ms into each pin-12 window = IR flash every 8 s. |
| `PB13` (`IC81` pin 14) | High; one 201 µs low pulse ~8.7 s after power-on, otherwise idle. |
| Test button (~10 s hold, speaker fitted) | Short chirp, then a longer quiet tone ~9 s later; the 4 s / 8 s cycle paused ~43 s and then resumed. No test-specific edge on the four probed lines (the ~1.1 s `PB11` high in section 7 was on Unit 1 before unlock). |

Conclusion: `IC301` is the smoke controller and `IC81` its LED/photodiode front end; the EFR32 exchanges `PB11` pulses with `IC301`, and `PB13` (`IC81` pin 14) was seen active only once (201 µs low after power-on). Which line reports smoke/alarm to the EFR32 is still open. Two captures taken after short test presses showed no cycle at all after power-on and stay unexplained (loose clips or a post-test pause).

## 8. Next bench steps

- [x] Dump `IC6` (`W25Q16JL`, 2 MiB): sound table + audio clips only, no firmware (see section 4).
- [ ] Identify the clip audio encoding (optional; needed only to reuse the stock sounds).
- [x] Saleae Logic 4 capture of the stock firmware (see section 7). `IC81` and the `LED81` drive were not resolved.
- [x] Chamber mapping (2026-11-27, see section 7a): `IC301` sequences `IC81`/`LED81`; `PC6` is a battery load test; test button on `PA0`.
- [x] Map `IC301` pins 3, 5–25, 30–32 (2026-11-27, see section 7): `PB11` (pin 21) reaches the EFR32 as a signal; pin 12 only feeds `RESETn` through a diode (anode at `IC301`); pins 5/6/10/11 drive `IC6`.
- [x] Pin 12 diode: anode at `IC301`, cathode at `RES` (2026-11-28).
- [x] Recheck `IC301` pins 3, 17–31 (2026-11-28): new EFR32 links `PA4` (pin 22), `PB12` (pin 30), `PB13` (pin 31), `AVDD` (pin 25).
- [ ] Recheck `IC301` pins 1, 2, 4, 24, 32 (the old `IC81` map was one pin off on the 27–29 side) and whether `IC81` pin 14 is a supply rail (pins 19/20, `PA1`/`PA2` dividers).
- [ ] Saleae `PB11`, `PA4`, `PB12` and `PB13` on Unit 1 with the custom firmware (which leaves these pins undriven) during a ~10 s test-button hold, to see which line `IC301` uses to report the alarm.
- [ ] Unlock Unit 1 (`3EB7KFZQ`) via `AAP DEVICE_ERASE` (keep `3EA8VKXK` and `3EA8W6D6` stock) and flash custom OpenVerisure EFR32FG14 firmware (`gecko_sdk v4.4.6` + RAIL).
