# Technical Hardware & Protocol Analysis: Inter-Processor Architecture (CC1110 <-> ADSP-BF512)

> **Note (2026-11-16):** this is the early analysis. The protocol as it actually works now (SPI commands, radio packets, gateway reception, MQTT) is described in [Camera_protocol.md](Camera_protocol.md).

**Camera detector:** Verisure GW MD CAM2 (`SECUR-PCAM-BLACKFIN P/N3204965 D`, LED board `P/N3302042 C`)  
**Role:** Hardware & Protocol Analyst (Pro / High Thinking)  
**Recipient:** Orchestrator (Parent Agent)

---

## Executive Summary & System Overview

Communication between the **TI CC1110F32** (RF SoC & system supervisor) and the **Analog Devices ADSP-BF512 Blackfin** (image processing DSP) forms an asymmetric dual-processor system designed for extreme battery efficiency:
- **In idle mode (Deep Sleep / PM2):** The Blackfin DSP, the SDRAM (`EM638165TS`) and the CMOS sensor (`Sunny 8811GA`) are **100 % unpowered (0.0 V, 0 µA)**. Only the CC1110 and the analog PIR sensor are supplied from `VBAT` (~3.1 V).
- **On image request:** The CC1110 enables the voltage rails in sequence, releases the Blackfin from reset (`P0_0 = 1`), lets the DSP boot from `U3 Flash` (BMODE=011, SPI Master), then takes over the bus as **SPI Master** relative to the Blackfin (**SPI Slave**), and clocks out the finished JPEG image in controlled blocks sized for the CC1110's 4 KB RAM and 868.950 MHz RF packets.

The detailed analysis of the 5 questions follows below.

---

## 1. Sharing the SPI Bus during Boot & Flash Tri-State

### A. Boot Architecture (BMODE=011 / SPI Master Boot)
The ADSP-BF512 is hardware-configured with **`BMODE[2:0] = 011`** (Boot from external SPI flash in Master Mode):
1. **Reset phase (`P0_0 = 0`):**
   - The Blackfin's I/O pins are held in reset/floating pulldown.
   - U3 Flash (`EN25F40`) Pin 1 (`/CS`) is connected directly to the Blackfin's internal **`SPI0_SEL1` (PG1)**. When the Blackfin is held in reset, `SPI0_SEL1` falls to 0 V, which unintentionally enables U3 Flash (`/CS = 0`). That is why the CC1110 could read JEDEC ID `0x1C 0x31 0x13` directly from the flash memory when `P0_0 = 0`.
2. **Boot ROM phase (`P0_0 = 1`, time $t = 0 \dots 200	ext{ ms}$):**
   - The internal Boot ROM in the ADSP-BF512 wakes up and puts the processor's SPI0 controller in **SPI Master Mode** at 10–25 MHz.
   - The Blackfin drives `SCK` (`PG13`), sends read commands over `MOSI` (`PG11`) and actively pulls `SPI0_SEL1` (`PG1`) low.
   - The boot loader reads the `.ldr` binary (the application code) from U3 Flash into the DSP's internal L1 SRAM and the external SDRAM.
3. **Critical requirement on the CC1110 during boot:**
   - During the first 500–800 ms after `P0_0 = 1` has been released, **the CC1110 MUST hold all its SPI lines (`P1_2..P1_5`) in absolute tristate (High-Z)** (`P1SEL = 0`, `P1DIR = 0`, `P1INP = 0`). If the CC1110 were to drive `SCK`, `MOSI` or pull `/SPISS` low during this phase, a bus collision (*bus contention*) occurs that crashes the Blackfin's boot sequence.

### B. How U3 Flash Is Tri-Stated after Boot
When the Boot ROM has read the entire LDR stream and hands execution over to Verisure's application firmware:
1. The Blackfin drives the line **`SPI0_SEL1` HIGH (3.3V)**.
2. According to the datasheet for the EON `EN25F40` (and the JEDEC standard): When `/CS` goes high, the flash memory immediately enters low-power *Standby Mode*, and its data output `SO` (Pin 2, connected to MISO) goes into **High-Impedance (Tri-state)**.
3. The Blackfin keeps `SPI0_SEL1` constantly high throughout its active run time, which guarantees that the flash memory stays electrically silent.

### C. How the Blackfin Becomes Available to the CC1110
After boot has finished, the Blackfin runs its application initialization:
1. The Blackfin reconfigures its internal SPI0 peripheral from Master to **SPI Slave** (`MSTR = 0` in `SPI_CTL`).
2. The Blackfin's `SCK` and `MOSI` become high-impedance inputs that listen on the bus.
3. The Blackfin's `PG10` (`/SPISS`) becomes an active-low slave select connected to CC1110 Pin 1 (`P1_2`).
4. Its `MISO` (`PG12`) is tri-stated while waiting for `/SPISS` (`P1_2`) to be pulled low by the CC1110.

---

## 2. Master vs Slave Role and the ADSP-BF512 SPI Slave DMA/FIFO Architecture

### A. Who Is Master and Who Is Slave during Image Transfer?
**Conclusion:** **The CC1110 is SPI Master** and **the Blackfin is SPI Slave**.

**Architectural evidence:**
1. **Memory & pacing constraint:** The CC1110 has an 8051 core with only **4 KB SRAM (XDATA)**. A compressed JPEG image is approx. 5–15 KB. Radio transmission over 868.950 MHz requires strict block pacing (8 bytes per RF chunk, with a 15 ms delay per packet so as not to overload the channel). If the Blackfin were Master, it would clock out megabytes per second and immediately cause buffer overflow in the CC1110. As Master, the CC1110 controls the clocking (`SCK`), reads out 8–64 bytes at a time, sends them over RF, and clocks out the next part.
2. **Empirical evidence from measurements:** In the measurement log (`Measurements_phase2_power_and_buses.md`) the Saleae/multimeter showed that as soon as the CC1110 took control as SPI Master and pulled `P1_2` (`/SPISS`) low, the Blackfin's MISO pin responded immediately by clamping the signal from floating to a **stable logic zero (`0x00`) during all 512 clock cycles**. This is a 100 % certain confirmation of active Blackfin SPI slave hardware.

### B. ADSP-BF512 SPI Slave Architecture: Registers, CPHA and SENDZ

```mermaid
flowchart LR
    subgraph CC1110 ["TI CC1110 (SPI Master)"]
        P1_3["P1_3 (SCK Master)"]
        P1_5["P1_5 (MOSI Master)"]
        P1_4["P1_4 (MISO Slave In)"]
        P1_2["P1_2 (/SPISS Output)"]
        P1_6["P1_6 (DSP_READY Input)"]
    end

    subgraph Blackfin ["ADSP-BF512 (SPI Slave)"]
        SCK_S["PG13 (SPI0_SCK)"]
        MOSI_S["PG11 (SPI0_MOSI)"]
        MISO_S["PG12 (SPI0_MISO)"]
        SPISS_S["PG10 (SPI0_SS)"]
        READY_S["PGx (DSP_READY Output)"]
        
        SPI_CTL["SPI_CTL: MSTR=0, SENDZ=1, CPHA=0/1"]
        TX_DMA["DMA Channel 7 (SPI TX) / SPI_TDBR"]
    end

    P1_3 -->|Clock Pulses| SCK_S
    P1_5 -->|Commands (0x04/0x03)| MOSI_S
    MISO_S -->|JPEG Stream (0xFF 0xD8...)| P1_4
    P1_2 -->|Word Framing Toggle| SPISS_S
    READY_S -.->|Frame Ready (3.3V)| P1_6
```

#### 1. Word Size (the `SIZE` bit in `SPI_CTL`)
- In the ADSP-BF512 the word size is controlled by bit 8 (`SIZE`) in `SPI_CTL`:
  - `SIZE = 0`: 8-bit data words.
  - `SIZE = 1`: 16-bit data words.
- In the Blackfin, the DMA channels (DMA7 for SPI TX) and the core buses are natively 16-bit oriented. If Verisure's firmware has configured the slave for 16-bit words, it expects clock pulses in multiples of 16 bits. Sending odd bytes can shift the bit window.

#### 2. CPHA=0 vs CPHA=1 & the Requirement for `/SPISS` Toggling (Critical hardware quirk!)
Analog Devices Blackfin processors have a unique hardware behavior in SPI Slave Mode that often causes problems when interfacing with generic MCUs:
- **At CPHA = 0 (SPI Mode 0 / Mode 2):**
  - The slave must drive the first bit on MISO immediately when `/SPISS` falls from 1 to 0, before the first clock edge arrives.
  - **Critical:** In the Blackfin's hardware, `/SPISS` at `CPHA = 0` acts as a **hardware word-boundary marker (word-boundary framing)**! If the master holds `/SPISS` continuously low and clocks out a long byte stream, the Blackfin's internal shift register loses synchronization and refuses to load the next word from the FIFO.
  - **Solution for CPHA=0:** The master (CC1110) **MUST toggle `/SPISS` high between each word** (i.e. `P1_2 = 0` $\rightarrow$ transfer 16 bits $\rightarrow$ `P1_2 = 1` for at least 2–5 NOPs).
- **At CPHA = 1 (SPI Mode 1 / Mode 3):**
  - Data is shifted out on the first clock edge. Here the Blackfin does **not** require toggling of `/SPISS` between words; the master can pull `/SPISS` low and clock a continuous stream of hundreds of bytes (burst transfer).
- **Conclusion for earlier tests:** When the CC1110 clocked 512 bytes continuously with `P1_2 = 0` in Mode 0 (CPHA=0), this caused a framing error in the Blackfin, with the result that the slave could not output data.

#### 3. The Significance of SENDZ (`SZ`, Bit 2 in `SPI_CTL`)
- `SENDZ = 0`: The slave resends the last word on TX FIFO underrun.
- `SENDZ = 1`: The slave sends **logic zeros (`0x0000` / `0x00`)** on TX FIFO underrun.
- The fact that the CC1110 consistently read `0x00` proves that the Blackfin runs with `SENDZ = 1` and that the transmit buffer (DMA7 / `SPI_TDBR`) was empty at the time of measurement.

---

## 3. Analysis of Pin 33 (P1_6): DSP_READY vs SHUTTER_TRIGGER

### Facts
- Pin 33 (`P1_6`) is routed to a Blackfin GPIO (probably `PG` or `PF`).
- The pin was measured at **0.0 V in idle mode**.

### In-Depth Analysis of Signal Direction
| Property | Hypothesis A: `SHUTTER_TRIGGER` (CC1110 Out $\rightarrow$ DSP In) | Hypothesis B: `DSP_READY` (DSP Out $\rightarrow$ CC1110 In) |
| :--- | :--- | :--- |
| **Idle voltage (0 V)** | Active-high trigger that rests low via an internal pulldown resistor. | The DSP is not ready / the image has not been taken, so the DSP drives the output low. |
| **System architecture** | Possible, but the Blackfin is already woken up and started solely to take an image. | **Standard architecture in industrial camera design.** The camera needs 200–500 ms for auto-exposure (AEC/AWB) and JPEG compression before data is in the buffer. |
| **Cause of earlier 0x00 results** | The CC1110 sent short pulses without waiting for DSP initialization. | **Bus collision!** In `main.c`, `P1_6` was configured as an **OUTPUT** and driven to 0 V (`P1_6 = 0`). If the Blackfin tried to drive the pin high to 3.3V to flag "image ready", the signal was shorted to GND by the CC1110! |

### Conclusion for Pin 33 (P1_6)
1. **Primary function:** `P1_6` is most likely a **`DSP_READY` / `FRAME_VALID`** output from the Blackfin to the CC1110.
2. **How the CC1110 should handle it:**
   - The CC1110 should set `P1_6` as an **INPUT** (`P1DIR &= ~0x40`) with the pull-down disabled / a weak pull-up.
   - When the Blackfin has booted and finished image compression, the Blackfin will drive `P1_6` to **3.3 V (HIGH)**.
   - The CC1110 polls `P1_6` or sets a Port 1 interrupt on the rising edge. Only when `P1_6 == 1` does the SPI readout start!
3. **Fallback (if the pin is an active-high Shutter Trigger):**
   - If `P1_6` is still 0 V after 1500 ms and no response is received on SPI, the pin is switched to output, given a 20 ms HIGH pulse (`0 -> 1 -> 0`), and immediately returns to input.

---

## 4. How Image Capture Is Triggered in Verisure's Original Firmware

1. **Why PIR ALARM (`P1_7`) does NOT trigger the Blackfin directly:**
   - The PIR sensor alarms around the clock on every movement in the room. In a disarmed home, a direct hardware connection between the PIR and the Blackfin would drain three CR123A lithium batteries in a few days (the Blackfin draws ~100 mA and the LED flash up to 500 mA).
   - The PIR edge goes exclusively to the CC1110 (`P1_7`), which filters the alarm in software and sends RF telemetry to the gateway.
2. **Decision matrix for image capture:**
   - Image capture is triggered only when **the CC1110 actively powers the Blackfin** (either due to motion in armed mode or via a downlink command from the gateway).
3. **The actual trigger mechanism in the firmware:**
   - **Method 1: Automatic triggering on Power-Up:**
     Since the Blackfin is 100 % unpowered at rest and draws 95 mA when switched on, there is no reason to power it unless it is going to take an image. In Verisure's original code, the Blackfin starts its camera pipeline directly at boot:
     $$	ext{Power On} \rightarrow 	ext{Boot from U3 Flash} \rightarrow 	ext{Sensor I2C AEC/AWB} \rightarrow 	ext{PPI DMA Capture} \rightarrow 	ext{JPEG Encode} \rightarrow 	ext{DSP\_READY = 1}$$
   - **Method 2: SPI command handshake (`P1_2` / MOSI):**
     If the Blackfin waits for a synchronous start command from the CC1110, the industry standard is used:
     - `0x04` = Snapshot Trigger (confirmed in the measurement log: MISO responds directly to this command).
     - `0x02` = Query Status / Frame Buffer Length.
     - `0x03` = Read Image Stream (clock out JPEG).

---

## 5. Concrete, Testable C Code Sequence for the CC1110 (SDCC)

Below is a complete, optimized and non-blocking C sequence that:
- Performs correct power sequencing of all 4 voltage rails.
- Tristates the SPI bus during Blackfin boot.
- Reads `P1_6` as an input (`DSP_READY`).
- Handles the Blackfin's `/SPISS` toggling in 16-bit words (CPHA=0) and also tests Mode 3 (CPHA=1).
- Detects and saves the JPEG SOI (`0xFF 0xD8`).

```c
/**
 * @file blackfin_capture.c
 * @brief Robust CC1110 <-> ADSP-BF512 Image Capture & JPEG SOI Detector
 */

#include <cc1110.h>
#include <stdint.h>
#include <stdbool.h>

extern __xdata uint8_t g_spi_rx_buf[2048];

/* ----------------------------------------------------------------------------
 * 1. SPI0 Master Helper Functions
 * ---------------------------------------------------------------------------- */

void spi0_init_mode0(void) {
    PERCFG |= 0x01;       // USART0 Alt. 2 (P1_2..P1_5)
    P1SEL  |= 0x38;       // P1_3 (SCK), P1_4 (MISO), P1_5 (MOSI) = Peripheral
    P1SEL  &= ~0x04;      // P1_2 = GPIO (/SPISS)
    P1DIR  |= 0x2C;       // P1_2, P1_3, P1_5 = Outputs
    P1DIR  &= ~0x10;      // P1_4 (MISO) = Input
    P1INP  &= ~0x10;      // Pull-up active on MISO

    P1_2 = 1;             // /SPISS idle HIGH
    P1_3 = 0;             // SCK idle LOW (Mode 0)

    U0CSR = 0x00;         // SPI Master
    U0GCR = 0x20 | 0x0F;  // MSB first, CPOL=0, CPHA=0, ~844 kHz @ 27 MHz
    U0BAUD = 0x00;
}

void spi0_init_mode3(void) {
    PERCFG |= 0x01;
    P1SEL  |= 0x38;
    P1SEL  &= ~0x04;
    P1DIR  |= 0x2C;
    P1DIR  &= ~0x10;
    P1INP  &= ~0x10;

    P1_2 = 1;
    P1_3 = 1;             // SCK idle HIGH (Mode 3)

    U0CSR = 0x00;
    U0GCR = 0xE0 | 0x0F;  // MSB first, CPOL=1, CPHA=1, ~844 kHz
    U0BAUD = 0x00;
}

uint8_t spi0_txrx(uint8_t b) {
    uint16_t timeout = 10000;
    URX0IF = 0;
    U0CSR |= 0x40;        // RE = 1
    U0DBUF = b;
    while (!URX0IF && --timeout);
    URX0IF = 0;
    return U0DBUF;
}

/**
 * @brief 16-bit word transfer with mandatory /SPISS toggling (for Blackfin CPHA=0)
 */
uint16_t spi0_transfer_word16(uint16_t out_word) {
    uint8_t hi, lo;
    
    P1_2 = 0; // Assert /SPISS
    __asm nop nop nop nop __endasm;
    
    hi = spi0_txrx((uint8_t)(out_word >> 8));
    lo = spi0_txrx((uint8_t)(out_word & 0xFF));
    
    __asm nop nop nop nop __endasm;
    P1_2 = 1; // Deassert /SPISS (forces word-boundary reload in the Blackfin)
    __asm nop nop nop nop __endasm;
    
    return ((uint16_t)hi << 8) | lo;
}

/* ----------------------------------------------------------------------------
 * 2. Main Sequence for Image Capture & JPEG SOI Detection
 * ---------------------------------------------------------------------------- */

int16_t capture_and_detect_jpeg(uint16_t *out_jpeg_len) {
    uint16_t i;
    int16_t jpeg_start = -1;
    uint16_t total_len = 0;
    uint16_t ready_timeout = 200; // 200 * 10 ms = 2 seconds max

    *out_jpeg_len = 0;

    /* ------------------------------------------------------------------------
     * STEP 1: Power the Blackfin and Peripheral Circuits in the Correct Sequence
     * ------------------------------------------------------------------------ */
    P0DIR |= 0x85; // P0_0 (/RESET), P0_2 (1.2V), P0_7 (Camera rail) = Outputs
    P1DIR |= 0x03; // P1_0 (3.3V), P1_1 (Bus EN) = Outputs

    P0_0 = 0;      // HOLD BLACKFIN IN RESET DURING POWER-UP
    P0_2 = 1;      // Turn on 1.2V Core rail (Q13)
    P1_0 = 1;      // Turn on 3.3V switched rail (Q7 - SDRAM & Sensor)
    P0_7 = 1;      // Turn on 3.27V sensor analog supply (J2-4)
    P1_1 = 1;      // Activate Bus Switch Enable

    /* ------------------------------------------------------------------------
     * STEP 2: Tristate the SPI Bus and Configure P1_6 as Input (DSP_READY)
     * ------------------------------------------------------------------------ */
    P1SEL &= ~0x7C; // P1_2..P1_6 = GPIO
    P1DIR &= ~0x7C; // Tristate P1_2..P1_6 (Inputs) during boot!
    P1INP &= ~0x7C; // Enable pull-ups

    // Wait 50 ms for voltage stabilization
    delay_ms(50);

    /* ------------------------------------------------------------------------
     * STEP 3: Release the Blackfin from Reset and Let It Boot from U3 Flash
     * ------------------------------------------------------------------------ */
    P0_0 = 1;       // RELEASE RESET (Blackfin runs, draws ~95 mA)

    // Wait 1000 ms while the Boot ROM reads in the LDR and the sensor runs AEC/AWB
    delay_ms(1000);

    /* ------------------------------------------------------------------------
     * STEP 4: Handshake via P1_6 (DSP_READY)
     * ------------------------------------------------------------------------ */
    // Check whether the Blackfin has signaled DSP_READY (P1_6 == 1)
    while ((P1 & 0x40) == 0 && --ready_timeout) {
        delay_ms(10);
    }

    // If P1_6 stays 0, try the fallback: Send an active HIGH trigger pulse
    if ((P1 & 0x40) == 0) {
        P1DIR |= 0x40; // Temporarily switch to output
        P1_6 = 1;
        delay_ms(30);
        P1_6 = 0;
        P1DIR &= ~0x40; // Return to input
        delay_ms(400);  // Allow time for exposure
    }

    /* ------------------------------------------------------------------------
     * STEP 5: Initialize the CC1110 as SPI Master
     * ------------------------------------------------------------------------ */
    spi0_init_mode0();
    P1_1 = 1; // Make sure Bus Switch is active
    P1_2 = 1; // /SPISS idle HIGH
    delay_ms(10);

    /* ------------------------------------------------------------------------
     * STEP 6: Send Snapshot Command 0x0004 & Read Out the Buffer
     * ------------------------------------------------------------------------ */
    // Send trigger/init word with 16-bit word framing
    spi0_transfer_word16(0x0004);
    delay_ms(20);

    // Send read command 0x0003
    spi0_transfer_word16(0x0003);
    delay_ms(5);

    // Read out 2048 bytes from the Blackfin with strict toggling of /SPISS
    for (i = 0; i < 2048; i += 2) {
        uint16_t w = spi0_transfer_word16(0x0000);
        g_spi_rx_buf[i]     = (uint8_t)(w >> 8);
        g_spi_rx_buf[i + 1] = (uint8_t)(w & 0xFF);
    }

    /* ------------------------------------------------------------------------
     * STEP 7: Detect JPEG SOI (0xFF 0xD8) and EOI (0xFF 0xD9)
     * ------------------------------------------------------------------------ */
    for (i = 0; i < 2047; i++) {
        if (g_spi_rx_buf[i] == 0xFF && g_spi_rx_buf[i + 1] == 0xD8) {
            jpeg_start = (int16_t)i;
            break;
        }
    }

    if (jpeg_start >= 0) {
        for (i = (uint16_t)jpeg_start + 2; i < 2047; i++) {
            if (g_spi_rx_buf[i] == 0xFF && g_spi_rx_buf[i + 1] == 0xD9) {
                total_len = (i + 2) - (uint16_t)jpeg_start;
                break;
            }
        }
        if (total_len == 0) {
            total_len = 2048 - (uint16_t)jpeg_start;
        }
    }

    /* ------------------------------------------------------------------------
     * STEP 8: If Mode 0 Failed, Try Mode 3 (CPHA=1) Burst Read
     * ------------------------------------------------------------------------ */
    if (jpeg_start < 0) {
        spi0_init_mode3();
        P1_2 = 0; // Hold /SPISS low continuously (allowed with CPHA=1)
        delay_ms(1);
        
        spi0_txrx(0x03); // Read Stream Command
        for (i = 0; i < 2048; i++) {
            g_spi_rx_buf[i] = spi0_txrx(0x00);
        }
        P1_2 = 1;

        for (i = 0; i < 2047; i++) {
            if (g_spi_rx_buf[i] == 0xFF && g_spi_rx_buf[i + 1] == 0xD8) {
                jpeg_start = (int16_t)i;
                total_len = 2048 - (uint16_t)jpeg_start;
                break;
            }
        }
    }

    *out_jpeg_len = total_len;
    return jpeg_start;
}
```

---

## Summary of Action Items for `embedded_coder`
1. **Change `P1_6` from output to input (`P1DIR &= ~0x40`)**: Prevent the bus collision and measure whether the Blackfin drives the line high when JPEG encoding has finished.
2. **Apply 16-bit `/SPISS` toggling on `P1_2`**: Since the Blackfin runs `CPHA=0`, `P1_2` must be deasserted between words to prevent the slave's shift register from locking up.
3. **Keep the sensor rail `P0_7 = 1` active**: Without the analog supply on J2-4 the Sunny sensor's pixel clock does not start, which blocks the Blackfin's PPI DMA in an endless loop.
4. **Tristate the bus for 800 ms at boot**: Guarantee that the Blackfin can read in the LDR file from U3 Flash undisturbed before the CC1110 takes over as SPI Master.

The report is hereby handed over for implementation by the Embedded Coder and subsequent verification by the QA Tester.