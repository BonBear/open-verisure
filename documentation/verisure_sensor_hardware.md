# Verisure Sensor PCB: Hardware Mapping & Silicon Analysis (TI CC1110F32)

This document provides the complete physical and logical hardware mapping of the Verisure wireless door and window sensor (`SD-MAG-VIB`), based on the Texas Instruments **CC1110F32** (SoC featuring an 8051 core and integrated Sub-1GHz radio).

---

## 📌 GPIO and Peripheral Mapping

| Component / Sensor | CC1110 Pin | I/O Direction | Register Configuration | Behavior & Polarity |
| :--- | :---: | :---: | :--- | :--- |
| **Reed Switch** (Magnet) | **`P1_7`** (Pin 32) | Input | `P1SEL &= ~0x80; P1DIR &= ~0x80; P1INP &= ~0x80;` | Internal pull-up.<br>• **`0` = Magnet nearby** (Door closed / Secure)<br>• **`1` = Magnet removed** (Door open / Alarm) |
| **Shock Sensor** (Piezo/Vib) | **`P1_2`** (Pin 1) | Input | `P1SEL &= ~0x04; P1DIR &= ~0x04; P1INP &= ~0x04;` | Digital pulse input that rapidly toggles on physical shock/vibration. |
| **Tamper Switch** (Cover) | **`P0_1`** (Pin 15) | Input | `P0SEL &= ~0x02; P0DIR &= ~0x02; P0INP &= ~0x02;` | Internal pull-up.<br>• **`0` = Cover ON** (button depressed to GND via 100 $\Omega$ ➔ Secure)<br>• **`1` = Cover OFF** (button released / open ➔ TAMPER) |
| **Red LED** (`LED_RED`) | **`P0_2`** (Pin 16) | Output | `P0SEL &= ~0x04; P0DIR \|= 0x04;` | Active high (`1` = ON, `0` = OFF). Illuminates during alarm events (door open, shock, tamper). |
| **Green LED** (`LED_GREEN`) | **`P0_4`** (Pin 18) | Output | `P0SEL &= ~0x10; P0DIR \|= 0x10;` | Active high (`1` = ON, `0` = OFF). Emits a crisp 40 ms pulse during radio transmission. |
| **Debug Data (`DD`)** | **`P2_1`** (Pin 21) | I/O / Output | `P2SEL &= ~0x02; P2DIR \|= 0x02;` | Toggles on each transmitted packet as a test point for oscilloscope/logic analyzer. |
| **Debug Clock (`DC`)** | **`P2_2`** (Pin 22) | Input / Output | `P2SEL &= ~0x04; P2DIR \|= 0x04;` | Driven high (`1`) once the RF packet has successfully cleared the transmitter. |

---

## 🔍 Tamper Button (`P0_1`): Discovery and Verification

By transmitting real-time port diagnostic telemetry (`P0`, `P1`, `P2`) to the gateway serial monitor, the exact circuit layout of the tamper switch was determined:
1. **Resting State (Cover removed, button open):**
   ```text
   Pins : P0=0xEB P1=0x7F P2=0x1D
   ```
   `0xEB = 1110 1011`$_2$ ➔ Bit 1 (`P0_1`) is `1`.
2. **Button Depressed (Cover installed and secured):**
   ```text
   Pins : P0=0xE9 P1=0x7F P2=0x1D
   ```
   `0xE9 = 1110 1001`$_2$ ➔ Bit 1 (`P0_1`) is pulled to `0` (GND via 100 $\Omega$).
3. **Conclusion:** Tamper alarm is asserted when `P0_1 == 1` (cover removed), and cleared when `P0_1 == 0` (cover closed).

---

## 📡 RF Hardware, SAW Filter, and Crystal Calculations

### 1. The 27.000 MHz Crystal Oscillator (`X1`)
Unlike standard Texas Instruments reference designs which utilize 26.000 MHz crystals, crystal `X1` on the Verisure PCB is laser-engraved with **`27.000`**. Applying 26 MHz formulas shifts the synthesized carrier by ~33 MHz, preventing reception.

Formula for carrier frequency synthesis with $f_{\text{xosc}} = 27.000\text{ MHz}$ and target $f_{\text{carrier}} = 868.950\text{ MHz}$:
$$\text{FREQ} = \frac{f_{\text{carrier}} \times 2^{16}}{f_{\text{xosc}}} = \frac{868.950 \times 10^6 \times 65536}{27.000 \times 10^6} = 2\,109\,167 = \text{0x202EEF}$$

* **`FREQ2`** = `0x20`
* **`FREQ1`** = `0x2E`
* **`FREQ0`** = `0xEF` (Yields 868.950027 MHz, an error of only +27 Hz / 0.03 ppm!)

Formula for symbol rate at standard 38.383 kBaud:
$$R_{\text{data}} = \frac{(256 + 117) \times 2^{10}}{2^{28}} \times 27.000\text{ MHz} = 38\,386.6\text{ Baud}$$
* **`MDMCFG4`** = `0x8A`
* **`MDMCFG3`** = `0x75`

### 2. Surface Acoustic Wave (SAW) Filter (`U2`, marked `3716`)
Between the CC1110 balanced RF output and the helical spring antenna sits an RF SAW bandpass filter: **EPCOS/TDK B3716** (B39871B3716U410) with passband **868.00–870.00 MHz**. It attenuates harmonics and blocks out-of-band interference outside the 868 MHz European ISM band.

---

## ⚠️ Critical Silicon Traps & Solutions

### Trap 1: The SDCC Header Bug on `PA_TABLE0` vs `FSCAL0`
In standard SDCC `<cc1110.h>`, `PA_TABLE0` is erroneously mapped to register address `0xDF26`.
* Address `0xDF26` is actually **`FSCAL0`** (PLL synthesizer frequency calibration control register).
* Writing `PA_TABLE0 = 0xC2` using the standard header inadvertently writes `0xC2` directly into `FSCAL0`, breaking PLL lock completely and producing carrier failure.
* **Correct Definitions:**
  ```c
  #define TRUE_FSCAL0      (*((__xdata volatile uint8_t *)0xDF26))
  #define TRUE_PA_TABLE0   (*((__xdata volatile uint8_t *)0xDF2E))

  TRUE_FSCAL0    = 0x1F; // MUST be 0x1F for PLL lock!
  TRUE_PA_TABLE0 = 0xC2; // Sets +10 dBm transmit power at correct address 0xDF2E
  ```

### Trap 2: TX FIFO Underflow and DMA Trigger 19
Unlike the discrete CC1101 transceiver, the CC1110 SoC **does not feature a 64-byte hardware FIFO**. The RF data register `RFD` (`0xDFD9`) is **only 1 byte deep**. If the 8051 CPU fails to load each byte before the modulator shifts it out, the radio faults with `MARCSTATE = 0x16` (`TXFIFO_UNDERFLOW`).
* **Solution:** Transmission is governed by **DMA Channel 0** using **Trigger 19 (`DMA_TRIG_RADIO`)**:
  ```c
  dma_desc.cfg0 = 0x13; // Trigger 19: RADIO
  dma_desc.cfg1 = 0x4A; // High priority, SRCINC=1, DESTINC=0
  ```
  The internal DMA engine feeds `RFD` synchronously to the radio symbol clock with zero CPU jitter.
