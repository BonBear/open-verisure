# GW-SD3 smoke detector: bench worksheet

Bench measurements for the Verisure **GW-SD3** smoke detector. Background and part list:
[README.md](../README.md). Procedures, pinouts and erase warnings:
[documentation/bench_recipes.md](../../documentation/bench_recipes.md).

Unit used: ☑ `3EA8VKXK` (reference unit `3EB7KFZQ` kept untouched)  
Date: `2026-11-25/26`  
Tools: DMM, Otii Arc (3.0 V / 0.2 A), USB microscope, Raspberry Pi Debug Probe (CMSIS-DAP v2 + `pyOCD 0.46.1`), OpenVerisure Gateway (`lolin_s2_mini_eval` `sniff all`).

## 1. Power

| Point | Expected | Measured | Notes |
| :--- | :--- | :--- | :--- |
| Battery type / marking | Lithium | `1× CR-2/3AZ` (3.0 V Li-MnO2) | Same cell type as SIREN `GW-SI` |
| Battery connector (`BAT+` / `BAT-`) | 2-pin JST (back) | **`01`, `03`, `29` = `BAT+`**; **`02`, `04`, `27` = `BAT-` (`GND`)** | `01` & `02` are the two large pads under the base jig holes |
| `VCC` pad to GND | ~3.0 V | **3.0 V** | Always-on rail to EFR32 |
| `VCA` pad to GND | switched rail | **0 V** (in idle) | Power-gated analog/audio rail |
| `IC6` (`W25Q16JL`) `VCC` (pad `60`) | 2.3–3.6 V | **0 V** (in idle) | Power-gated for 10-year battery life! Safe for 3.3 V FT232H dump with battery disconnected |
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

Dump pending (FT232H + `flashrom` via pads `51–54`, `60`, `02`).

## 5. Identified & mapped ICs / peripherals

| Ref | Marking | Package | Measured / traced | Confirmed function |
| :--- | :--- | :--- | :--- | :--- |
| `IC301` | `304A 029J 363` | QFN-32 | Pins 13+14 → Pad `43` → `CN301` speaker; Pins 15+16 → Pad `42` → `CN301` speaker | **Audio/voice IC with BTL speaker driver** (drives `M36BN20-A` 8 Ω speaker on `CN301`) |
| `IC81` | `C4660 224KU` | TSSOP-20 | Connected directly to `PD81` (optical chamber photodiode); surrounded by inductors + `PC41` + 100 µF cap | **Optical smoke-chamber AFE** |
| `LED81` | IR LED (chamber) | 2-pin TH | Connected to **EFR32 pin 5 (`PF4`)** | **Smoke chamber IR emitter** |
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

## 7. Next bench steps

- [ ] Dump `IC6` (`W25Q16JL`, 2 MiB) via back pads `51–54`, `60`, `02` (FT232H + `flashrom`) and inspect for GBL OTA images, voice/audio clips, or logs.
- [ ] Optional Saleae Logic 4 capture on `IC81`, `IC301`, `CP92` (I2C/SPI) before unlocking EFR32.
- [ ] Unlock Unit 1 (`3EA8VKXK`) via `AAP DEVICE_ERASE` (keep `3EB7KFZQ` stock) and flash custom OpenVerisure EFR32FG14 firmware (`gecko_sdk v4.4.6` + RAIL).
