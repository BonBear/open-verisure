# Hardware Mapping & Pinouts: Verisure CC1110F32 and ESP32-C3

This document contains the verified pinout and register configuration for all sensors, LEDs, debug pins and RF components on the Verisure board, as well as the SPI wiring for the ESP32-C3.

---

## 1. Verisure Sensor Board (TI CC1110F32) – GPIO & Sensors

| Sensor / Component | CC1110 Pin | Direction | Register / Bitmask | Function & Behavior |
| :--- | :--- | :--- | :--- | :--- |
| **Reed switch** (Magnet) | `P1_7` (Pin 32) | Input | `P1SEL &= ~0x80; P1DIR &= ~0x80;` | Senses whether the magnet is near (`0`) or away (`1`). |
| **Shock sensor** (Shock/Piezo) | `P1_2` (Pin 1) | Input | `P1SEL &= ~0x04; P1DIR &= ~0x04;` | Digital/pulse input that triggers on vibrations or impacts. |
| **Push button** (`SW1`) | `RESET_N` (Pin 31) | Input (Reset) | Hardware reset via 100 Ω to GND | External hardware reset/pairing button. Detected in software via `SLEEP.RST[1:0] == 00`. |
| **Red LED** (`LED_RED`) | `P0_2` | Output | `P0SEL &= ~0x04; P0DIR \|= 0x04;` | Active high (`1` = on, `0` = off). |
| **Green LED** (`LED_GREEN`) | `P0_4` | Output | `P0SEL &= ~0x10; P0DIR \|= 0x10;` | Active high (`1` = on, `0` = off). |
| **Debug Data (`DD`)** | `P2_1` (Pin 21) | I/O | CC-Debugger interface | Connected to `J2-4` and the ESP32 CC-Flasher (`http://cc.local`). |
| **Debug Clock (`DC`)** | `P2_2` (Pin 22) | Input | CC-Debugger interface | Connected to `J2-5` and the ESP32 CC-Flasher (`http://cc.local`). |
| **Reset (`RESET_N`)** | `RESET_N` (Pin 31 / Pin 6) | Input | Active low | Connected to `J2-6`, the ESP32 CC-Flasher and push button `SW1`. |

---

## 1.1 Verified Mapping of the Flashing Connector (J2)

Physical multimeter measurements against the CC1110 and the voltage rails:

| J2 Pin | Measured Node | Function & Description |
| :---: | :--- | :--- |
| **`J2-1`** | **Pin 3** | Square pad with white silkscreen dot. Aux / UART / GPIO. |
| **`J2-2`** | **Pin 2** | Round pad. Aux / UART / GPIO. |
| **`J2-3`** | **VCC** | **Target 3.3V Power**. Connected directly to the battery positive terminal via fuse `F1` / `BAT+_R`. |
| **`J2-4`** | **`P2_1`** | **`DD` (Debug Data)** for CC-Debugger / ESP-Flasher (verified 2026-11-17 on 2JHL2KMX: DD is J2-4 and DC J2-5, the reverse of earlier notes). |
| **`J2-5`** | **`P2_2`** | **`DC` (Debug Clock)** for CC-Debugger / ESP-Flasher. |
| **`J2-6`** | **Pin 6** | **Reset / Control pin** for the programming interface. |
| **`J2-7`** | **GND** | **System ground (GND)**. Connected directly to the PCB ground plane. |

### Pull-up / Pull-down registers
* `P0SEL`, `P1SEL`, `P2SEL`: Set to `0` for the respective pin to select the plain GPIO function.
* `P1INP` / `P2INP`: Configure pull-up, pull-down or tristate for the ports.

### Verified reference code for sensors and LEDs
```c
#include <cc1110.h>

void setup_gpio(void) {
    // 1. LEDs (P0_2 = Red, P0_4 = Green) as outputs
    P0SEL &= ~0x14;
    P0DIR |= 0x14;
    P0_2 = 0;
    P0_4 = 0;

    // 2. Sensors as inputs: Reed (P1_7), Shock sensor (P1_2), Button (P2_0)
    P1SEL &= ~(0x80 | 0x04);
    P1DIR &= ~(0x80 | 0x04);
    P2SEL &= ~0x01;
    P2DIR &= ~0x01;
}
```

---

## 2. Verisure RF Path & Crystal (From Microscope Analysis)

* **RF outputs (`RF_P` / `RF_N`):** Differential outputs (pins 25 and 26 on the CC1110) pass through a passive LC balun network straight into component `U2`.
* **SAW filter (`U2`, marked `3716`):** Passive **EPCOS/TDK B3716** (B39871B3716U410) surface acoustic wave bandpass filter with center frequency **869.00 MHz** and passband **868.00–870.00 MHz**.
* **Crystal (`X1`):** **27.000 MHz** SMD crystal (engraved "27.000" in the microscope images `PXL_20261015_200824175.jpg` and `PXL_20261015_200955005~2.jpg`). Connected to pins 22/23 on the CC1110.
  * **Frequency registers for 868.950 MHz:** `FREQ2 = 0x20`, `FREQ1 = 0x2E`, `FREQ0 = 0xEF` (theoretical error: only +27 Hz / 0.03 ppm).
  * **Modem registers for 38.383 kBaud:** `MDMCFG4 = 0x8A`, `MDMCFG3 = 0x75`.
---

## 3. ESP32-C3 Super Mini + CC1101 (Receiver & Reference Transmitter)

Both the Receiver (ESP32 #1) and the Reference Transmitter (ESP32 #2) use the same wiring:

| CC1101 Pin | ESP32-C3 Super Mini | Function |
| :--- | :--- | :--- |
| **VCC** | `3V3` | Power supply (3.3 V) |
| **GND** | `GND` | Ground |
| **SCK / SCLK** | `GPIO 4` | SPI Clock |
| **MISO (SO)** | `GPIO 5` | SPI MISO |
| **MOSI (SI)** | `GPIO 6` | SPI MOSI |
| **CSN** | `GPIO 7` | SPI Chip Select |
| **GDO0** | `GPIO 1` | Digital Output / Interrupt |
| **GDO2** | `GPIO 3` | Digital Output / Interrupt |
| *(Built-in LED)* | `GPIO 8` | Blue status LED on the Super Mini (Active LOW) |