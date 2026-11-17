# Project Overview: Verisure CC1110 to ESP32-C3 (CC1101) RF Link

This document is the main index for the project. To save context window, the technical documentation in `VerisureDoorSensor/docs/` is split into modular files. Load only the sub-files or images needed for the current task.

---

## 1. Project Goals

1. **Transmitter (Verisure board, TI CC1110F32):** Read the board's sensors (reed switch on `P1_7`, shock sensor on `P1_2`, button on `P2_0`) and transmit an 8-byte packet (`"HB:ALIVE"`) on **868.950 MHz** (2-FSK, 38.4 kBaud, sync word `0xD3 0x91`, fixed length with hardware CRC).
2. **Receiver (ESP32-C3 Super Mini #1 + CC1101):** Receive the packet via SPI, verify the hardware CRC and log payload, RSSI and frequency offset via USB Serial (`COM11`).
3. **Reference node (ESP32-C3 Super Mini #2 + GERUI CC1101):** Identically wired ESP32-C3 + CC1101 that acts as a reference transmitter/receiver.
4. **Automated HIL loop (Hardware-in-the-Loop):** Build with SDCC, flash the CC1110 directly from the terminal via the ESP32 CC-Flasher (`http://cc.local`) and verify results in real time via the ESP32 receiver's serial port.

---

## 2. Current Status & Verified Baseline

* **ESP32 #2 $\rightarrow$ ESP32 #1 (100 % Verified):** The reference link between the two ESP32-C3 units works flawlessly on 868.950 MHz and gives `RECEIVED: "HB:ALIVE" | RSSI: -42 dBm | Offset: -4.8 kHz | CRC: OK` every other second.
* **Verisure CC1110 $\rightarrow$ ESP32 #1 (100 % Verified with Telemetry):** 
  - **Crystal:** Verified as **27.000 MHz** (marked "27.000" on X1). The radio registers `FREQ2..0 = 0x202EEF` and `MDMCFG3 = 0x75` give exactly 868.950 MHz and 38.38 kBaud.
  - **Transmission:** DMA Channel 0 with Trigger 19 (`DMA_TRIG_RADIO`) targeting `0xDFD9` (`X_RFD`) delivers continuous transmissions without underflow.
  - **Link performance:** The receiver logs the packets with **`RSSI: -36 dBm`**, **`LQI: 0xFF` (255/255)** and **`CRC: OK`**!
  - **Event-driven transmission & Telemetry:** Packet format `VS:<Reed><Shock><Button><Seq>`:
    - **Immediate transmission on trigger:** As soon as the magnetic contact (`P1_7`) changes state or the shock sensor (`P1_2`) registers an impact, the wait state is aborted at once and a packet is transmitted immediately (< 2 ms latency).
    - **Red LED (`P0_2`):** Lights red as long as the magnet is open (`P1_7 == 1`, alarm state) or during a shock/vibration event (held on for 1000 ms). Off when the door is closed and things are quiet.
    - **Green LED (`P0_4`):** Gives a short 40 ms pulse on every active radio transmission.
    - **Button (`SW1` on Pin 31 / `RESET_N`):** Hardware reset button; reports `'B'` during the first 5 packets after a button press.
  - **HIL CLI loop:** Automated with `python VerisureDoorSensor/build_and_flash.py` (compiles with SDCC, uploads and flashes via ESP_CC_Flasher at `http://192.168.0.80`, and monitors `COM11`).

---

## 3. Modular File Index in `VerisureDoorSensor/docs/` (Lazy-Loading Guide)

* **`verisure_door_sensor_guide.md`** $\rightarrow$ **Complete main documentation:** Detailed walkthrough of the hardware, pinouts, button (`SW1`), magnetic contact, shock sensor, the J2 connector for flashing, 27 MHz crystal calculations, the `FSCAL0` vs `PA_TABLE0` pitfall, DMA Trigger 19 and the telemetry protocol (with embedded images).
* **`hardware_mapping.md`** $\rightarrow$ *Open when working with GPIO, sensors (`P1_7` reed, `P1_2` shock, button via `RESET_N`), LEDs (`P0_2`/`P0_4`), SPI wiring on the ESP32-C3 or the RF hardware (`U2` SAW filter).*
* **`registers.md`** $\rightarrow$ *Open when configuring or verifying the radio registers (`FREQ`, `MDMCFG`, `DEVIATN`, `PKTCTRL`, `PA_TABLE0`).*
* **`cc1110_quirks_and_dma.md`** $\rightarrow$ *Open when writing transmit logic in C for the CC1110, setting up DMA Channel 0 (Trigger 19), handling SDCC compiler flags or automating flashing against `http://cc.local`.*

### Image files in `VerisureDoorSensor/docs/` (Open when visual hardware verification is needed)
* `pcb_bottom_mapped.png`: Overview of the bottom side with mapping of the J2 connector, U1, battery connections and sensors.
* `fig_j2_connector_detail.jpg`: Detailed close-up of the J2 flashing connector (Pin 1 square pad).
* `fig_top_sensors_sw1.jpg`: Detailed close-up of the top side: battery clip, button `SW1` and reed switch.
* `fig_cc1110_rf_crystal.jpg`: Close-up of the CC1110, the 27.000 MHz crystal `X1`, balun and RF path.
* `U2-1.jpeg` & `U2-2.jpeg`: Microscope images of the RF balun and passive SAW filter `U2` (EPCOS B3716, 868–870 MHz).
* `CC1110.jpeg`: Microscope image of the CC1110F32 chip.
* `PXL_20261015_200824175.jpg` & `PXL_20261015_200955005~2.jpg`: Close-ups of the 27.000 MHz crystal `X1`.
* `board-top.jpg`, `board-bottom-1.jpg`, `board-bottom-2.jpg`: High-resolution original photos of the circuit board.