# Verisure GW MD CAM2: Measurement Log Phase 2 (Camera, Power-Enable & Buses)

> Historical log (status as of 2026-10-30); the current state is in [MD_CAM/README.md](../README.md).

This measurement log is for mapping how the **TI CC1110 (`U15`)** switches on power to the **Blackfin DSP (`U5`)**, the camera and the flash memory, and how the buses (SPI / UART) are connected between them.

> [!CAUTION]
> **Always remove the batteries and disconnect all cables before measuring!**
> All measurements in this document are made **fully unpowered** with the multimeter in **Resistance mode (Ω)** or **Continuity test (Beeper / "beep mode")**.

---

## 📍 Quick Guide: Reference Pins on the CC1110 (`U15`, QFN-36)

When you read the text (*"CC1110 F32 TI..."*) the right way up, **Pin 1 is at the top left**:

```
             P1_3  P1_4  P1_5 [P1_6] P1_7  RST   RBIAS AVDD  AVDD
             (36)  (35)  (34)  (33)  (32)  (31)  (30)  (29)  (28)
           +------------------------------------------------------+
  (1) P1_2 | •                                                    | (27) AVDD
  (2) DVDD |                                                      | (26) AVDD
  (3) P1_1 |                                                      | (25) AVDD
  (4)[P1_0]|                                                      | (24) RF_N
  (5)[P0_0]|                      CC1110F32                       | (23) RF_P
  (6) P0_1 |                        (U15)                         | (22) AVDD
  (7) P0_2 |                                                      | (21) XOSC_Q1
  (8) P0_3 |                                                      | (20) XOSC_Q2
  (9) P0_4 |                                                      | (19) DCOUPL
           +------------------------------------------------------+
             (10)  (11)  (12)  (13)  (14)  (15)  (16)  (17)  (18)
             DVDD  P0_5  P0_6 [P0_7] P2_0  P2_1  P2_2  P2_3  P2_4
                   (RX)  (SW3)             (DD)  (DC)  (32.768kHz)
```

**The 4 unknown control pins on the CC1110 are:**
* **Pin 4 (`P1_0`):** Left side, 4th leg from the top.
* **Pin 5 (`P0_0`):** Left side, 5th leg from the top.
* **Pin 13 (`P0_7`):** Bottom row, 4th leg from the left.
* **Pin 33 (`P1_6`):** Top row, 4th leg from the left.

---

## 🔬 Part 1: Find Power-Enable (Which pin switches on 3.3V / 1.2V?)

All voltage rails (`3.3V`, `2.5V`, `1.8V`, `1.2V`) are normally 0 V. They are controlled by a transistor circuit (MOSFET, often marked `Q...`) or the regulator's `EN` pin near the inductors/capacitors by the `3.3V` test pad.

Place **one multimeter probe on each CC1110 candidate pin**, and search with the other probe on the transistors / small components around the `3.3V` / `1.2V` test pads:

| CC1110 Candidate Pin | Has contact (0 Ω / beep) with component | Note / Resistance value |
| :--- | :--- | :--- |
| **Pin 4 (`P1_0`)** | `[ X ]` Yes &nbsp;&nbsp; `[  ]` No | Component: `______Q7___________` &nbsp;&nbsp; Resistance: `__________ Ω` |
| **Pin 5 (`P0_0`)** | `[  ]` Yes &nbsp;&nbsp; `[  ]` No | Component: `____________________` &nbsp;&nbsp; Resistance: `__________ Ω` |
| **Pin 7 (`P0_2`)** | `[ X ]` Yes &nbsp;&nbsp; `[  ]` No | Component: `________Q13________` &nbsp;&nbsp; Resistance: `____0_____ Ω` |
| **Pin 13 (`P0_7`)** | `[  ]` Yes &nbsp;&nbsp; `[  ]` No | Component: `____________________` &nbsp;&nbsp; Resistance: `__________ Ω` |
| **Pin 33 (`P1_6`)** | `[  ]` Yes &nbsp;&nbsp; `[  ]` No | Component: `____________________` &nbsp;&nbsp; Resistance: `__________ Ω` |



*Tip:* If you see a small transistor (3-leg or 6-leg SOT-23 marked e.g. `Q1`, `Q2` etc.) or a small 5-leg regulator IC next to the `3.3V` pad, measure against its legs!

---

## 💾 Part 2: Is the CC1110's SPI bus connected to the U3 SPI Flash (`DSP V175`)?

The chip **`U3`** (SOIC-8) has a white label with the text **`DSP V175`**.
SOIC-8 has Pin 1 at the bottom left (at the dot/bevel) and is counted counter-clockwise:
* **Pin 1:** `/CS` (Chip Select)
* **Pin 2:** `SO` (Serial Output / MISO)
* **Pin 5:** `SI` (Serial Input / MOSI)
* **Pin 6:** `SCK` (Serial Clock)
* **Pin 4:** `GND` | **Pin 8:** `VCC` (3.3V)

Measure with the multimeter from the **CC1110's SPI test pads** to **`U3`'s legs**:

| CC1110 Test Pad | Measure against U3 (`DSP V175`) | Does it beep? (0 Ω) | Measured Resistance (Ω) |
| :--- | :--- | :---: | :---: |
| **`SCK`** (Pin 36) | **U3 Pin 6** (`SCK`) | `[X]` Yes &nbsp; `[  ]` No | `____0_____ Ω` |
| **`MOSI`** (Pin 34) | **U3 Pin 5** (`SI`) | `[X]` Yes &nbsp; `[  ]` No | `____0_____ Ω` |
| **`MISO`** (Pin 35) | **U3 Pin 2** (`SO`) | `[X]` Yes &nbsp; `[  ]` No | `____0____ Ω` |
| **`SS` (Upper)** (Pin 1) | **U3 Pin 1** (`/CS`) | `[  ]` Yes &nbsp; `[X]` No | `__________ Ω` |
| **`SS` (Lower)** (Pin 3) | **U3 Pin 1** (`/CS`) | `[  ]` Yes &nbsp; `[X]` No | `__________ Ω` |

---

## 📟 Part 3: The UART bus (`TX` and `RX`)

There are two test pads marked **`TX`** and **`RX`** by the CC1110 (connected to CC1110 Pin 9 and Pin 11 respectively):

1. Do the test pads **`TX`** and **`RX`** run via traces to the **Blackfin DSP (`U5`)**?
   * `TX`: `[  ]` Yes &nbsp;&nbsp; `[  ]` No &nbsp;&nbsp; (Resistance to U5: `__________ Ω`)
   * `RX`: `[  ]` Yes &nbsp;&nbsp; `[  ]` No &nbsp;&nbsp; (Resistance to U5: `__________ Ω`)
2. Does `TX` or `RX` have any series resistor (e.g. 100 Ω or 1 kΩ) in the trace?
   * Answer: `No, not that I can find. I measured on the back where I think U5 is, don't know if those pads go straight through. Can't measure on the front`

---

## 💡 Part 4: The flash LED connectors `J2` and `J3`

The LED flash board plugs into the two 4-pin connectors **`J2`** and **`J3`**.

Measure with the multimeter whether any of the 4 CC1110 candidate pins (`P1_0`, `P0_0`, `P0_7`, `P1_6`) go to the pins on `J2` or `J3`:

| CC1110 Pin | Goes to connector J2 or J3 (Pin 1..4)? | Note / Function |
| :--- | :--- | :--- |
| **Pin 4 (`P1_0`)** | `[X]` None &nbsp; `[  ]` J2 Pin `__` &nbsp; `[  ]` J3 Pin `__` | `____________________` |
| **Pin 5 (`P0_0`)** | `[X]` None &nbsp; `[  ]` J2 Pin `__` &nbsp; `[  ]` J3 Pin `__` | `____________________` |
| **Pin 13 (`P0_7`)** | `[  ]` None &nbsp; `[X]` J2 Pin `_4_` &nbsp; `[  ]` J3 Pin `__` | `_______6 kOhm_______` |
| **Pin 33 (`P1_6`)** | `[X]` None &nbsp; `[  ]` J2 Pin `__` &nbsp; `[  ]` J3 Pin `__` | `____________________` |

---

## 📝 Results & Summary (2026-10-30, partly superseded)

> Several of these early conclusions were corrected by later measurements (2026-11-11 to 2026-11-14); the corrections are marked **Corrected** below. The current pinout is in [Hardware_pinout_and_measurements.md](Hardware_pinout_and_measurements.md).

* **Power-Enable & Voltage Control for the Blackfin DSP (`U5`):**
  - **`P0_2` (Pin 7):** Enables the transistor `Q13` for the **1.2V Blackfin Core rail**.
  - **`P1_0` (Pin 4):** Drives the gate of `Q7`. **Corrected:** Q7 holds the Blackfin **/RESET** (P1_0 = 1 holds the Blackfin in reset, 0 releases it); it is not a 3.3 V rail.
  - **`P1_6` (Pin 33) & `P0_7` (Pin 13):** Enables auxiliary rails / trigger.
  - **`P0_0` (Pin 5):** First taken for the Blackfin `/RESET`. **Corrected:** P0_0 is a probable **supply enable** for the Blackfin; /RESET is controlled by P1_0 via Q7.
    - With `P0_0 = 0` the Blackfin is held in reset and the board draws only **20–22 mA**.
    - With `P0_0 = 1` the Blackfin starts and runs the factory firmware with a stable current consumption of **92–95 mA**.

* **U3 SPI Flash Memory (`DSP V175`):**
  - **IC:** EON Silicon Solution (cFeon) `EN25F40` 4 Megabit / 512 KB Serial NOR Flash.
  - **JEDEC ID:** **`0x1C 0x31 0x13`** (Read out and confirmed via Logic 4 and CC1110 SPI Master).
  - **Chip Select (`/CS`, Pin 1):** First assumed to be driven by the Blackfin's `SPI0_SEL1`. **Corrected:** U3 /CS follows the CC1110's **`P1_1`** (see Steps E–H below: `P1_1 = 0` silences U3).

* **The Inter-Processor Bus (CC1110 <-> Blackfin ADSP-BF512):**
  - **SPI Master (`USART0 Alt. 2`):** `SCK` = `P1_3`, `MISO` = `P1_4`, `MOSI` = `P1_5`.
  - **Chip Select & Bus Switch:** 
    - **`P1_2` (`SS Upper`):** First taken for the Blackfin's Slave Select. **Corrected:** P1_2 **enables the SPI path to Blackfin SPI1** (port H, PH0..PH3); the boot selector also reads it on PH0 at power-on.
    - **`P1_1` (`SS Lower`):** Active-high **Bus Switch Enable**. When `P1_1 = 1` the SPI bus is fully open towards the Blackfin and can clock 4096 clocks continuously at 844 kHz. When `P1_1 = 0` the bus is clamped low. During boot `P1_1 = 1` must be held active.
  - **Measured continuity (Multimeter):** `U3 Pin 2` (`SO`) beeps with direct continuity (0 Ω) to `CC1110 Pin 35` (`MISO`), which guarantees that the Saleae probes sit directly on the bus.
  - **DSP Slave behaviour:** The Blackfin DSP actively drives MISO to `0x00` (`SENDZ = 1`) while waiting for its internal image-processing pipeline to fill the transmit buffer with a captured image.
  - **LED board (`P/N3302042 C`):** Connected to `J2`/`J3`. A red indicator LED lights up at power-on. The high-power flash is controlled via combined logic (Blackfin/P0_7/light sensor).

* **Verified Boot and Signal Sequence from Reference Board #2 (Logic 4):**
  - **Corrected:** the Blackfin boots from the **EON EN25F20** on SPI0 (PG12–PG15), **not** from U3. The U3 reads below are therefore not the boot ROM loading its program.
  - At cold start the Blackfin ADSP-BF512 starts as **SPI Master** at 2.000 MHz.
  - For 20.7 ms the Blackfin reads from the U3 Flash (`EN25F40`) via opcodes `0x05` (RDSR), `0x9F` (RDID) and `0x0B` (Fast Read at 0x000000, 0x020000, 0x040000, 0x07F000, 0x07E200, 0x07E000).
  - After 20.7 ms U3 `/CS` is set high and the Blackfin configures SPI0 as **SPI Slave**.
  - The CC1110 then takes over as **SPI Master** in Mode 0 / Mode 3 with 16-bit word framing and toggles `/SPISS` (`P1_2`).
  - Current consumption at 3.00 V: **3.0 mA** in idle, **73–75 mA** during image capture/DSP phase.

* **Live Shutter Test with button SW3 on Board #1 (2026-10-30):**
  - **Shutter trigger:** Pressing SW3 was detected immediately by the CC1110 (100 Hz micro-pull scan on `P1_6`).
  - **RF telemetry:** Immediately sent opcode `0xEE` ("button":"ON") to the ESP32-S2 Gateway (`seq: 25429`, RSSI -34 dBm, LQI 116).
  - **Voltage sequencing & Current profile:** The bench power supply (3.00 V) jumped from 3.0 mA idle current to **75.0–76.0 mA** for exactly **1.84 seconds** (44–46 active samples), and then returned deterministically to 3.0 mA.
  - **SPI communication:** The CC1110 generated 16384 clock pulses at 844 kHz (2048 bytes read out) and sent command sweeps (0x0004, 0x0003, 0x0001, 0x0100, 0x0002, 0x0200, 0x8001, 0x55AA).
  - **DSP MISO behaviour:** The Blackfin responded with a stable logic zero (`0x00`, `SENDZ=1`) during all read-outs.
  - **Bus isolation:** Analysis confirmed that `P1_1` must be actively driven HIGH (1) to open the bus switch; when `P1_1` is floating or 0 the bus is clamped to 0V.
  - **LED & Debug Halt:** "Detect CC" on the ESP CC-Flasher puts the CC1110 in debug halt (internal pull-ups), which pulls `P0_7` high and lights the red diode `LD1` via `Q2`. On "Reset / RunCC" (`/cc_exit_dbg`) the firmware's `io_init()` grounds the pin `P0_7` to 0V and turns off the diode.

* **Steps E–H: Bus clamping, Unclamping and U3 reading during Blackfin operation (2026-10-30):**
  - **Clamping:** If the Blackfin is booted with `P1_2 = 1` (/SPISS deasserted) its internal SPI slave hardware actively drives the lines `SCK`, `MISO` and `MOSI` low against the CC1110's internal pull-ups (`0, 0, 0` -> `P1 = 0xC5`).
  - **Unclamping:** By pulling `P1_2 = 0` (/SPISS active low) during reset release / voltage ramp-up and then releasing `P1_2 = 1`, the Blackfin releases the bus to full High-Z (`P1 = 0xFD` with pull-ups).
  - **U3 NOR Flash during Blackfin operation:** In the unclamped state the CC1110 as SPI Master can read the `U3` JEDEC ID (`0x1C 0x31 0x13`) without errors via `P1_1 = 1` even while the Blackfin is running at full power (71–75 mA).
  - **U3 Deselect:** When `P1_1 = 0` U3 is silenced completely (`0xFF 0xFF 0xFF`), which leaves the Blackfin as the only slave on the SPI bus.
  - **Flasher optimisation:** In `build_and_flash_md_cam.py` the firmware binary is now uploaded *before* `cc_init` is called, which minimises the time in debug halt and prevents unnecessary current consumption and diode indication.
