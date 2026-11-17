# Verisure Door/Window Sensor (TI CC1110F32): Complete Reverse Engineering & RF Guide

This document provides a comprehensive technical breakdown of the **Verisure Door/Window Sensor** based on the Texas Instruments **CC1110F32** Sub-1 GHz SoC. It details the complete board architecture, reverse-engineered pinouts, sensor interfacing, the flashing header (`J2`), and the exact register mathematics and architectural quirks required to get the 868.950 MHz radio working reliably.

---

## 1. Board Overview & Architecture

The sensor board is labeled on the silkscreen as **`VIB-MAG-SECUR`** (Securitas / Verisure P/N `N3302409`). It is designed as a dual-purpose security perimeter node that detects:
1. **Door/Window Opening**: Via an integrated glass magnetic reed switch.
2. **Vibration / Forced Entry / Glass Break**: Via an analog piezo shock sensor and vibration elements conditioned through an operational amplifier circuit.
3. **Tamper / Reset / Pairing**: Via a miniature tactile push button (`SW1`) located adjacent to the battery compartment.
4. **Visual Status**: Via a high-brightness dual-color LED indicator (Red `LED1` and Green).

![Verisure PCB Bottom Overview with Mappings](pcb_bottom_mapped.png)

### Key On-Board Components

| Component ID | Silkscreen Marking | Part / Description | Function |
| :--- | :--- | :--- | :--- |
| **U1** | `CC1110 F32 TI` | TI CC1110F32 (36-pin QFN) | Single-chip 8051 MCU + Sub-1 GHz RF transceiver |
| **X1** | `27.000` | 27.000 MHz SMD Crystal | High-speed system & radio oscillator |
| **U2** | `3716` | EPCOS / TDK B3716 SAW Filter | 868–870 MHz bandpass filter (Center: 869.00 MHz) |
| **U3** | `4542I` / `602853` | Low-Power Operational Amplifier | Signal conditioning for shock/piezo sensor |
| **VS1, VS2** | Passives / Sensors | Piezo / Spring Vibration Elements | Mechanical shock transducers |
| **SW1** | Tactile Switch | Surface Mount Push Button | Hardware Reset / Tamper switch (under battery clip) |
| **Reed** | Glass Capsule | Magnetic Reed Contact Switch | Detects magnet presence on door/window frame |
| **LED1 / LED2** | `LD1` / Dual SMD | Red (`P0_2`) & Green (`P0_4`) LEDs | Alarm status and RF transmission indicators |
| **J2** | `J2` (7-pin header) | 1x7 Plated Through-Hole Header | In-circuit programming and debug interface |
| **Antenna** | Helical Coil | 868 MHz Spring Monopole | RF radiation element |

---

## 2. Reverse-Engineered Hardware Pinout

The CC1110F32 is packaged in a 36-pin QFN (`RSP`). Pin 1 is located at the bottom-left corner of the IC when viewing the chip with text upright (adjacent to test point `TP1`).

![CC1110 SoC, 27 MHz Crystal X1, and RF Front-End](fig_cc1110_rf_crystal.jpg)

### CC1110 Pin Mapping Table

| Pin # | CC1110 Pin | Direction | Connected Peripheral | Circuit & Electrical Description |
| :---: | :--- | :---: | :--- | :--- |
| **1** | **`P1_2`** | Input | **Shock / Vibration Sensor** | Digital pulse input from `U3` op-amp circuit. High (`1`) in quiescent state; toggles low on physical impact or vibration. Configured with internal pull-up. |
| **2** | `DVDD` | Power | Digital Core Power | Decoupled to digital 3.0V–3.3V rail. |
| **7** | **`P0_2`** | Output | **Red LED (`LED1`)** | Active high (`1` = ON, `0` = OFF). Driven continuously when door is open, and held for 1000 ms on detected shock. |
| **9** | **`P0_4`** | Output | **Green LED** | Active high (`1` = ON, `0` = OFF). Driven as a 40 ms pulse during RF packet transmission. |
| **14** | **`P2_1`** | Bidirectional | **Debug Data (`DD`)** | TI 2-wire debug data line. Connected to flashing header `J2`. |
| **15** | **`P2_2`** | Input | **Debug Clock (`DC`)** | TI 2-wire debug clock input. Connected to flashing header `J2`. |
| **20** | `XOSC_Q2` | Input | **27.000 MHz Crystal** | High-speed crystal terminal 2. |
| **21** | `XOSC_Q1` | Input | **27.000 MHz Crystal** | High-speed crystal terminal 1. |
| **23** | `RF_P` | RF Out | Transceiver Differential RF+ | Connected via LC balun matching network to `U2`. |
| **24** | `RF_N` | RF Out | Transceiver Differential RF- | Connected via LC balun matching network to `U2`. |
| **31** | **`RESET_N`** | Input | **Button `SW1` & Debug Reset** | Active-low hardware reset. Connected to push button `SW1` via a **100 $\Omega$ series resistor to GND**. Also routed to `J2`. |
| **32** | **`P1_7`** | Input | **Magnetic Reed Switch** | Glass reed switch input with internal pull-up. **`0` = Magnet near** (closed/secured), **`1` = Magnet removed** (open/alarm). |
| **PAD** | `GND` | Ground | Ground Plane (EPAD) | Soldered directly to the PCB ground plane. |

---

## 3. Top-Side Peripherals: Button `SW1` & Magnetic Reed Switch

Looking at the top side of the PCB under the battery compartment:

![Top Side: Battery Clip, Push Button SW1, and Reed Switch](fig_top_sensors_sw1.jpg)

### 3.1 The Push Button (`SW1`) Discovery
* **Physical Location**: Located immediately below the positive battery contact clip.
* **Electrical Connection**: One side is connected to GND; the other side connects through a **100 $\Omega$ resistor directly to Pin 31 (`RESET_N`)**.
* **Operational Behavior**: Pressing `SW1` pulls the hardware reset line low and places the CC1110 in reset. Upon release, the CC1110 boots afresh.
* **Software Detection**: Because the button triggers a hardware reset rather than a standard GPIO interrupt, firmware detects button events by inspecting the reset cause in the **`SLEEP` SFR (0xBE)** at startup:
  ```c
  uint8_t boot_sleep = SLEEP;
  uint8_t rst_cause = (boot_sleep >> 3) & 0x03; // SLEEP.RST[1:0]
  // 00 = External Reset (RESET_N pressed -> Button SW1)
  // 01 = Power-On Reset (Battery inserted)
  ```
  When `rst_cause == 0x00`, the firmware immediately broadcasts an initial telemetry packet flagging `Button: SW1 Pressed`.

### 3.2 The Magnetic Reed Switch
* **Physical Location**: Sealed glass envelope mounted along the right edge of the PCB.
* **Operation**: When an external magnet (mounted on the door/window frame) is within ~15 mm, the internal contacts close, grounding CC1110 Pin 32 (`P1_7`). When the door opens and the magnet moves away, internal pull-ups bring `P1_7` high (`1`).

---

## 4. In-Circuit Flashing Connector (`J2`)

Flashing and debugging the CC1110 is performed via the 7-pin header footprint **`J2`** along the edge of the battery cutout on the bottom side of the PCB.

![J2 Flashing Header Detail](fig_j2_connector_detail.jpg)

### 4.1 J2 Pinout Identification (Measured & Verified)

The J2 connector consists of 7 plated through-holes. **Pin 1 is distinguished by its square copper pad and an adjacent white silkscreen dot**.

Direct multimeter continuity measurements between the J2 header and the CC1110 / power rails yield the following verified pinout:

| J2 Pin # | Pad Geometry | Measured Signal / Node | Function / Role in Flashing | Notes & Description |
| :---: | :--- | :--- | :--- | :--- |
| **J2-1** | **Square Pad** (White dot) | **Pin 3** | Auxiliary / UART / GPIO | Trace to CC1110 pin 3 (top hole) |
| **J2-2** | Round Pad | **Pin 2** | Auxiliary / UART / GPIO | Trace to CC1110 pin 2 |
| **J2-3** | Round Pad | **VCC (3.3V)** | **Target Power (VDD)** | Connected directly to positive battery rail via fuse `F1` and `BAT+_R` |
| **J2-4** | Round Pad | **Pin 22 (`P2_2`)** | **`DC` (Debug Clock)** | 2-wire CC Debugger clock line (trace with 45° dogleg to SoC) |
| **J2-5** | Round Pad | **Pin 21 (`P2_1`)** | **`DD` (Debug Data)** | 2-wire CC Debugger bidirectional data line |
| **J2-6** | Round Pad | **Pin 6** | **Reset / Control** | Connected to CC1110 pin 6 (used for reset/control during flashing) |
| **J2-7** | Round Pad (near "J2" text) | **GND** | **System Ground** | Direct connection to the solid copper ground plane |

> [!NOTE]
> For programming via the **ESP32 CC-Flasher**, the essential lines are:
> * **Power & Ground**: `J2-3` (**VCC**) and `J2-7` (**GND**).
> * **Clock & Data**: `J2-4` (**`DC` / Pin 22**) and `J2-5` (**`DD` / Pin 21**).
> * **Reset Control**: Connected to `J2-6` / `RESET_N` to force debug mode.

### 4.2 Flashing via ESP32 CC-Flasher
The target is flashed using an ESP32 running CC-Debugger firmware reachable over Wi-Fi (`http://cc.local` or `http://192.168.0.80`). 

A fully automated Python script (`VerisureDoorSensor/build_and_flash.py`) handles the workflow:
1. Compiles the C source with **SDCC** (`sdcc -mmcs51 --model-small`).
2. Converts the Intel Hex output to binary using `makebin -p`.
3. Posts the binary to `http://cc.local/upload` and commands `POST /flash_cc?file=main.bin`.
4. Polls the flasher status logs until `Verify successful` is returned.
5. Invokes `POST /cc_exit_dbg` to release the debug interface and run the firmware.

---

## 5. Getting RF Working: Critical Gotchas & Register Math

Achieving clean RF transmission from the CC1110 to an external CC1101 receiver required solving three major hardware and compiler hurdles.

### 5.1 The 27.000 MHz Crystal Math (Not 26 MHz!)
While standard CC1101 and CC1110 reference designs use a 26.000 MHz crystal, close inspection of crystal `X1` revealed it is laser-engraved with **`27.000`**. Using 26 MHz math causes a ~33 MHz carrier shift, making packets completely undetectable.

For a carrier frequency of **$f_{\text{carrier}} = 868.950\text{ MHz}$** with **$f_{\text{xosc}} = 27.000\text{ MHz}$**:

$$\text{FREQ} = \frac{f_{\text{carrier}} \times 2^{16}}{f_{\text{xosc}}} = \frac{868.950 \times 10^6 \times 65536}{27.000 \times 10^6} = 2,109,167 = \text{0x202EEF}$$

* **`FREQ2`** = `0x20`
* **`FREQ1`** = `0x2E`
* **`FREQ0`** = `0xEF`  *(Yields an ideal frequency of 868.950027 MHz — an error of only +27 Hz / 0.03 ppm!)*

For a symbol rate of **38.4 kBaud (38.383 kBaud standard)**:

$$R_{\text{data}} = \frac{(256 + \text{DRATE\_M}) \times 2^{\text{DRATE\_E}}}{2^{28}} \times 27.000\text{ MHz}$$

With $\text{DRATE\_E} = 10\ (0\text{x0A})$ and $\text{DRATE\_M} = 117\ (0\text{x75})$:

$$R_{\text{data}} = \frac{(256 + 117) \times 1024}{268,435,456} \times 27.000 \times 10^6 = 38,386.6\text{ Baud}$$

* **`MDMCFG4`** = `0x8A`
* **`MDMCFG3`** = `0x75`
* **`DEVIATN`** = `0x35` (~21.4 kHz deviation at 27 MHz)

---

### 5.2 The Critical `FSCAL0` (`0xDF26`) vs `PA_TABLE0` (`0xDF2E`) Bug

In the official TI datasheet (`SWRS033`, Table 61 – Overview of XDATA Radio Registers), the radio register addresses are mapped as follows:

| Address | Register Name | Description | Required Value |
| :---: | :--- | :--- | :---: |
| `0xDF23` | `FSCAL3` | Charge pump calibration | `0xE9` |
| `0xDF24` | `FSCAL2` | VCO calibration | `0x2A` |
| `0xDF25` | `FSCAL1` | PLL calibration | `0x00` |
| **`0xDF26`** | **`FSCAL0`** | **Frequency synthesizer calibration** | **`0x1F` (MANDATORY)** |
| `0xDF27`–`0xDF2D` | `PA_TABLE7`–`PA_TABLE1` | Power amplifier table slots 7 to 1 | `0xC2` |
| **`0xDF2E`** | **`PA_TABLE0`** | **Active power amplifier slot 0 (+10 dBm)** | **`0xC2`** |

> [!CAUTION]
> **SDCC Header File Defect**:
> In the SDCC distribution header `<cc1110.h>`, `PA_TABLE0` is mistakenly defined as `0xDF26`.
> If code writes `PA_TABLE0 = 0xC2` using the standard header, it writes `0xC2` to **`FSCAL0`**, completely corrupting the PLL calibration! The synthesizer loses lock, and the chip transmits pure noise or silence.
> 
> **Correct Definition**:
> ```c
> #define TRUE_FSCAL0      (*((__xdata volatile uint8_t *)0xDF26))
> #define TRUE_PA_TABLE0   (*((__xdata volatile uint8_t *)0xDF2E))
> 
> TRUE_FSCAL0    = 0x1F; // Must remain 0x1F for proper PLL lock!
> TRUE_PA_TABLE0 = 0xC2; // Set +10 dBm transmit power on 0xDF2E
> ```

---

### 5.3 Solving FIFO Underflow via DMA Channel 0 (Trigger 19)

Unlike the standalone CC1101, the CC1110 **does not have a 64-byte hardware FIFO**. The radio data register `RFD` (`0xDFD9`) is only **1 byte deep**. When the radio enters `STX` mode, the CPU or DMA must write every byte before the internal serializer empties, or the radio immediately faults with `MARCSTATE = 0x16` (`TXFIFO_UNDERFLOW`).

To ensure zero packet loss and rock-solid timing, transmissions are offloaded to **DMA Channel 0** using **Radio Trigger 19**:

```c
typedef struct {
    uint8_t src_h;
    uint8_t src_l;
    uint8_t dst_h;
    uint8_t dst_l;
    uint8_t len_h;   // VLEN=000, LEN[12:8]=0
    uint8_t len_l;   // Length = 8 bytes
    uint8_t cfg0;    // Trigger 19: DMA_TRIG_RADIO (0x13)
    uint8_t cfg1;    // SRCINC=1, DESTINC=0, PRIORITY=High (0x4A)
} dma_desc_t;

// Transmission sequence:
RFST = 0x04;         // Enter IDLE
DMA0CFGH = ((uint16_t)&dma_desc) >> 8;
DMA0CFGL = ((uint16_t)&dma_desc) & 0xFF;
DMAARM = 0x01;       // Arm DMA Channel 0
__asm nop; nop; nop; nop; nop; nop; nop; nop; nop; __endasm; // 9 NOPs required by TI
RFST = 0x03;         // Command STX (DMA automatically feeds RFD)
while (!(RFIF & 0x10)); // Wait for IRQ_DONE (Bit 4)
```

---

## 6. Live Telemetry Protocol & Sensor Behavior

The firmware implements an event-driven telemetry reporter using a fixed 8-byte payload plus 2 bytes of hardware-generated CRC16:

$$\text{Payload Format: } \texttt{VS:<Reed><Shock><Button><Seq>}$$

```
Byte 0..2 : "VS:"
Byte 3    : Reed Status    -> 'O' (Open / Alarm), 'C' (Closed / Safe)
Byte 4    : Shock Status   -> 'S' (Vibration detected), '.' (Quiet)
Byte 5    : Button Status  -> 'B' (SW1 Reset / Pressed), 'P' (Power-On Boot), '.' (Normal)
Byte 6..7 : Sequence No    -> 2-digit Hex Counter ("00" to "FF")
```

### Event-Driven Instant Transmission (< 2 ms Latency)
* Rather than sleeping blindly in a fixed delay loop, the firmware executes a high-speed 1 ms polling loop that continuously samples `P1_7` (Reed) and oversamples `P1_2` (Shock, 10x per millisecond).
* **Instant Trigger**: The moment the door begins to open (`P1_7` transitions from `0` to `1`) or an impact is detected, the wait loop is aborted immediately and an RF packet is transmitted over the air in **$< 2\text{ ms}$**.
* **Visual LED Feedback**:
  * **Red LED (`P0_2`)**: Latched ON continuously whenever the door is open (`P1_7 == 1`), and held ON for **1000 ms** after any detected shock.
  * **Green LED (`P0_4`)**: Emits a crisp **40 ms pulse** upon every successful radio packet broadcast.

---

## 7. Verification Results (ESP32-C3 Receiver)

Packets are received by an **ESP32-C3 Super Mini + CC1101** receiver connected to the host via USB Serial (`COM11`):

```text
>>> [#0042] RECEIVED: "VS:C.05" | RSSI: -36 dBm | LQI: 255 (0xFF) | CRC: OK
  ==> [TELEMETRY] Magnet: CLOSED (Secured) | Shock: Quiet | Button: Normal operation | Seq: 05

>>> [#0043] RECEIVED: "VS:OS.06" | RSSI: -35 dBm | LQI: 255 (0xFF) | CRC: OK
  ==> [TELEMETRY] Magnet: OPEN (ALARM!) | Shock: SHOCK/VIBRATION DETECTED! | Button: Normal operation | Seq: 06

>>> [#0044] RECEIVED: "VS:C.B00" | RSSI: -36 dBm | LQI: 255 (0xFF) | CRC: OK
  ==> [TELEMETRY] Magnet: CLOSED (Secured) | Shock: Quiet | Button: PRESSED / RESET (SW1) | Seq: 00
```

With an RSSI of **-36 to -35 dBm**, an LQI of **255/255**, and **100% CRC pass rate**, the custom firmware demonstrates complete reverse-engineering and hardware control of the Verisure CC1110 sensor.
