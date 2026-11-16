# Verisure GW MD CAM2: Hardware Mapping & Measurement Log

This document contains the verified hardware mapping and the measurement results for Verisure's motion detector with camera (**GW MD CAM2**, PCB: `SECUR-PCAM-BLACKFIN P/N3204965 D`, LED board: `LED BOARD P/N3302042 C`).

---

## 🎉 Summary of Your Measurements (Spot on!)

Your measurements were a **100 % hit** and match exactly both the microscope image of `U15` (`images/Photo_2026_10_22_00_00_49.jpeg`) and TI's official datasheet (`SWRS033`) for the CC1110F32:

1. **The programming header is `J6`!**
   * `J6-2` = **Pin 15 (`P2_1` / `DD` - Debug Data)**
   * `J6-3` = **Pin 16 (`P2_2` / `DC` - Debug Clock)**
   * `J6-4` = **`RESET_N`** (also goes to the reset button `SW1` to `GND`, just like on the door sensor!)
   * `J6-5` = **`GND`**
   * `J6-1` = Has `1 kΩ` to Pin 10 (`DVDD`). *(Important: When flashing we therefore feed 3.3V directly to the `VBAT` pad or battery plus instead of `J6-1`, so that the 1 kΩ resistor does not throttle the current!)*
2. **The PIR motion sensor (`ALARM`) is fully mapped:**
   * Goes directly to **CC1110 Pin 32 (`P1_7`)** – exactly the same pin Verisure used for the magnetic contact on the door sensor!
   * **Active LOW:** Sits at **`3.08 V`** at rest and drops to **`0 V`** on detected motion.
3. **The tamper switch on the back (`SW3`):**
   * One side goes to **`VBAT+`** and the other side goes directly to **CC1110 Pin 33 (`P1_6`)**! *(Previously wrongly assumed to be P0_6)*.
   * **Polarity:** 
     * **Cover off (Tamper Alarm):** Switch open -> Internal pull-down pulls `P1_6` to **`0 V`**.
     * **Mounted on wall (Secured):** Switch closed to `VBAT+` -> `P1_6` becomes **`3.0 V`** (`1`).
4. **Why `3.3V`, `2.5V`, `1.8V` and `1.2V` were `0 V` during the battery measurement:**
   * The 3 batteries (3V CR123A/lithium) are **in parallel (`VBAT = 3.12 V`)** and supply **only the CC1110 and the PIR sensor** in idle mode!
   * All voltage regulators for the camera, Blackfin DSP, SDRAM and the flash LED board (`3.3V`, `2.5V`, `1.8V`, `1.2V`) are **completely switched off (0 V)** until the CC1110 actively chooses to switch them on! The reason they did not switch on when you waved is that Verisure's original firmware in the CC1110 is "disarmed" and ignores PIR alarms until the alarm central requests an image.
5. **The SPI and UART buses between the CC1110 and the Blackfin DSP are fully identified:**
   * **SPI (`USART0 Alt. 2`):** `SCK` = Pin 36 (`P1_3`), `MISO` = Pin 35 (`P1_4`), `MOSI` = Pin 34 (`P1_5`), `SS` (upper) = Pin 1 (`P1_2`), `SS` (lower) = Pin 3 (`P1_1`).
   * **Towards the Blackfin (verified 2026-11-11):** The CC pins reach Blackfin **SPI1** on port H when P1_2 = 1: P1_4 -> PH1 (SPI1_SCK), P1_3 -> PH2 (SPI1_MISO), P1_5 -> PH3 (SPI1_MOSI), PH0 (SPI1_SSEL) = 0. SCK and MISO are thus swapped compared with the CC labels above. The PH functions are taken from the Linux kernel's BF51x portmux and have not been checked against ADI's datasheet.
   * **UART (`USART1 Alt. 1`):** `TX` = Pin 9 (`P0_4`), `RX` = Pin 11 (`P0_5`).

---

## 📍 Correct Pin Guide: TI CC1110F32 (`U15`, QFN-36)

When you read the text (*"CC1110 F32 TI..."*) the right way up, **Pin 1 is at the top left**:

```
             P1_3  P1_4  P1_5  P1_6  P1_7  RST   RBIAS AVDD  AVDD
             (36)  (35)  (34)  (33)  (32)  (31)  (30)  (29)  (28)
           +------------------------------------------------------+
  (1) P1_2 | •                                                    | (27) AVDD
  (2) DVDD |                                                      | (26) AVDD
  (3) P1_1 |                                                      | (25) AVDD
  (4) P1_0 |                                                      | (24) RF_N
  (5) P0_0 |                      CC1110F32                       | (23) RF_P
  (6) P0_1 |                        (U15)                         | (22) AVDD
  (7) P0_2 |                                                      | (21) XOSC_Q1 (27MHz)
  (8) P0_3 |                                                      | (20) XOSC_Q2 (27MHz)
  (9) P0_4 |                                                      | (19) DCOUPL/AGND
           +------------------------------------------------------+
             (10)  (11)  (12)  (13)  (14)  (15)  (16)  (17)  (18)
             DVDD  P0_5  P0_6  P0_7  P2_0  P2_1  P2_2  P2_3  P2_4
                   (RX)  (SW3)             (DD)  (DC)  (32.768kHz)
```

---

## 📋 Verified Pinout Table for the CC1110 (`U15`)

| CC1110 Pin # | Port / Name | Measured Connection on the PCB | Function |
| :---: | :---: | :--- | :--- |
| **Pin 1** | **`P1_2`** | Test pad **`SS` (Upper)** | **Enable for the SPI path to Blackfin SPI1**: 1 = PH1..PH3 follow P1_4/P1_3/P1_5 and PH0 (SPI1_SSEL) = 0; 0 = PH0..PH3 read as 1 regardless of CC (verified 2026-11-11) |
| **Pin 2** | `DVDD` | Capacitor `C77` | Supply decoupling |
| **Pin 3** | **`P1_1`** | Test pad **`SS` (Lower)** | Assumed bus switch enable. Does **not** affect Blackfin PH0..PH3 in any state (verified 2026-11-11); role towards U3 not investigated |
| **Pin 4** | **`P1_0`** | Gate of **`Q7`** (N-MOSFET, SOT-89 "N06") | **Blackfin /RESET holder**: 1 = Q7 conducts and pulls B10 (/RESET) low, 0 = Blackfin is released (verified 2026-11-11) |
| **Pin 5** | **`P0_0`** | Blackfin reset circuit | Probably **supply enable for the Blackfin**: the current goes from 20 to 92 mA when P0_0 is set (/RESET is controlled by P1_0 via Q7) |
| **Pin 6** | `P0_1` | *(Unknown load)* | High gives +7 mA; not visible on Blackfin PF/PG/PH (verified 2026-11-11) |
| **Pin 7** | **`P0_2`** | Transistor **`Q13`** | **Power-Enable for the 1.2V Core rail** (Blackfin VDDINT) |
| **Pin 8** | **`P0_3`** | *(Unknown load)* | **Must be high**: low gives -20 mA (76 instead of 95-97 mA) and the Blackfin appears to be switched off (verified 2026-11-11) |
| **Pin 9** | **`P0_4`** | Test pad **`TX`** | UART TX (`USART1 Alt. 1`). **Not** visible on Blackfin PF/PG/PH (verified 2026-11-11) |
| **Pin 10** | `DVDD` | Capacitor `C91` (+ 1 kΩ to `J6-1`) | Digital 3V supply |
| **Pin 11** | **`P0_5`** | Test pad **`RX`** | UART RX (`USART1 Alt. 1`). **Not** visible on Blackfin PF/PG/PH (verified 2026-11-11) |
| **Pin 12** | **`P0_6`** | *(Not connected / Unused)* | Unused |
| **Pin 13** | **`P0_7`** | Connector **`J2 Pin 4`** / DSP rail | **Power-Enable / Switched Auxiliary rail** (6 kΩ to J2-4) |
| **Pin 14** | `P2_0` | *(Not connected)* | Unused |
| **Pin 15** | **`P2_1` (`DD`)** | **`J6-2`** | **Debug Data (`DD`)** for the CC-Flasher |
| **Pin 16** | **`P2_2` (`DC`)** | **`J6-3`** | **Debug Clock (`DC`)** for the CC-Flasher |
| **Pin 17** | `P2_3` (`XOSC32K_Q1`) | Crystal `Y2` (32.768 kHz) | Sleep Timer crystal |
| **Pin 18** | `P2_4` (`XOSC32K_Q2`) | Crystal `Y2` (32.768 kHz) | Sleep Timer crystal |
| **Pin 19–20** | `XOSC_Q1/Q2` | Crystal `Y1` (**27.000 MHz**) | RF and system crystal |
| **Pin 22–23** | `RF_P` / `RF_N` | Balun $\rightarrow$ Spring antenna | 868.950 MHz Sub-1GHz RF |
| **Pin 31** | **`RESET_N`** | **`J6-4`** & **`SW1`** | **Hardware Reset** for the CC-Flasher |
| **Pin 32** | **`P1_7`** | Test pad **`ALARM`** (from PIR) | **PIR Motion alarm** (`3.08V` = Rest, `0V` = Motion) |
| **Pin 33** | **`P1_6`** | **`SW3`** (Spring switch, back side) | **Sabotage switch (Tamper)** – closes to `VBAT+` (0V = Tamper, 3V = Secured) |
| **Pin 34** | **`P1_5`** | Test pad **`MOSI`** | SPI MOSI. Towards the Blackfin: **PH3 = SPI1_MOSI** when P1_2 = 1 (verified 2026-11-11). U3 Pin 5 |
| **Pin 35** | **`P1_4`** | Test pad **`MISO`** | Towards the Blackfin: **PH1 = SPI1_SCK** when P1_2 = 1 (verified 2026-11-11), i.e. the clock line towards the Blackfin. U3 Pin 2 |
| **Pin 36** | **`P1_3`** | Test pad **`SCK`** | Towards the Blackfin: **PH2 = SPI1_MISO** when P1_2 = 1 (verified 2026-11-11). U3 Pin 6 |

---

## 💡 The LED Board (`LED BOARD P/N3302042 C`)

From your new images (`images/led_board_front.jpg` and `images/led_board_back.jpg`), which connect to the 4-pin connectors `J3` and `J2`:
* **4 High-power LEDs (Flash/IR):** Light up the room when taking pictures in the dark.
* **Light sensor (up on the tab above the camera lens):** Measures whether the room is dark or light.
* **Driver circuit on the back (`U2`):** `MCP6042I` (dual rail-to-rail operational amplifier) + the transistors `Q3`/`Q6` and `1R0` (1.0 Ω) current-sense resistors, which form a constant-current driver for the flash LEDs.

---

## ✅ Check Measurements before Flashing (Confirmed!)

1. **Confirm power supply (`VBAT` to CC1110):**
   * Does the test pad **`VBAT`** beep directly (0 Ω) to **CC1110 Pin 10 (`DVDD`)**?
   * **Answer:** **[X] Yes (Beeps directly!)** $\rightarrow$ We connect `3V3` from the `ESP_CC_Flasher` to the `VBAT` pad (with the batteries removed). *(The 1 kΩ resistor on `J6-1` is only a `V_SENSE` protection output for the factory test jig).*

2. **Confirm `RESET_N` (`J6-4` to CC1110 Pin 31):**
   * Measure the resistance (Ω) between **`J6-4`** and **CC1110 Pin 31** (`RESET_N`).
   * **Answer:** **`2.7 kΩ`** $\rightarrow$ There is a `2.7 kΩ` ESD protection resistor in series between `J6-4`/`SW1` and Pin 31 (`RESET_N`). Since the CC1110 has an internal pull-up of `20 kΩ`, the `2.7 kΩ` gives a low level of ~0.37 V and works as is for debug reset (should the edge against expectations turn out too slow, the Pin 31 via/resistor is directly above `U15`).

---

## 🔌 Connecting the ESP_CC_Flasher (Once the 2 points above are confirmed)

| ESP_CC_Flasher Pin | Connects to on MD_CAM | Description |
| :--- | :--- | :--- |
| **`3V3`** | Test pad **`VBAT`** *(with batteries removed!)* | Direct 3.3V supply to the CC1110 (avoids the 1 kΩ on `J6-1`) |
| **`GND`** | **`J6-5`** *(or test pad `GND`)* | System ground |
| **`CC_DD` (GPIO 23)** | **`J6-2`** | Debug Data (`P2_1` / Pin 15) |
| **`CC_DC` (GPIO 19)** | **`J6-3`** | Debug Clock (`P2_2` / Pin 16) |
| **`CC_RST` (GPIO 33)** | **`J6-4`** | Hardware Reset (`RESET_N` / Pin 31) |
