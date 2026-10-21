/**
 * @file main.c
 * @brief Verisure Door/Window Sensor (CC1110F32) Firmware
 *
 * This firmware implements the custom, reverse-engineered RF protocol for the
 * Verisure Door & Window Sensor (model with magnetic reed switch, vibration/shock
 * sensor, and tamper switch) powered by a Texas Instruments CC1110F32 SoC (MCS-51 core).
 *
 * ============================================================================
 * ARCHITECTURE OVERVIEW:
 * ============================================================================
 * 1. Microcontroller & Radio:
 *    - SoC: Texas Instruments CC1110F32 (8051 CPU @ 26 MHz, 32 KB Flash, 4 KB XRAM)
 *    - Radio: Built-in Sub-1GHz CC1100/CC1101 transceiver core
 *    - RF Frequency: 868.950 MHz (2-FSK, 38.383 kBaud, 21.4 kHz frequency deviation)
 *    - RF Crystal (X1): 27.000 MHz (Important: FREQ and MDMCFG registers are
 *      specifically calibrated for the 27 MHz reference crystal)
 *    - TX Output Power: +10 dBm (PA_TABLE0 = 0xC2)
 *
 * 2. Hardware Sensors & Pin Mapping:
 *    - P1_7 : Magnetic Reed Switch (0 = Closed/Magnet Present, 1 = Open/Alarm)
 *    - P1_2 : Vibration/Shock Sensor (Pulse generator, edge transitions indicate shock)
 *    - P0_1 : Tamper / Sabotage Switch (0 = Lid closed / GND via 100 Ohm, 1 = Lid removed)
 *    - P0_2 : Red Status LED (Active HIGH)
 *    - P0_4 : Green Status LED (Active HIGH)
 *    - P2_1 : Debug Data (DD) / Flashing Header J2 Pin 5
 *    - P2_2 : Debug Clock (DC) / Flashing Header J2 Pin 4
 *    - RESET: Pin 31 (RESET_N) connected to J2 Pin 6
 *
 * 3. Security & Telemetry Protocol:
 *    - Packet Length: 24 bytes fixed + 2 bytes hardware CC1110 CRC
 *    - Header (Bytes 0..7): 8-character plaintext Device ID (e.g. "2JHL2MVN")
 *    - Ciphertext (Bytes 8..23): 16-byte AES-128-ECB encrypted block containing:
 *      * Bytes 0..1 : 16-bit rolling sequence counter (anti-replay)
 *      * Byte 2     : State bitfield (Bit 0: Reed, Bit 1: Shock, Bit 2: Tamper,
 *                     Bit 3: POR, Bit 4: Low Battery)
 *      * Byte 3     : Battery percentage (0..100%)
 *      * Bytes 4..5 : Battery voltage in millivolts (e.g. 3367 mV)
 *      * Bytes 6..9 : Device ID echo (anti-spoofing verification)
 *      * Bytes 10..11: Magic authentication bytes 'V', 'S' (0x56, 0x53)
 *      * Bytes 12..15: Hardware diagnostic ports (P0, P1, P2, Reset cause)
 * ============================================================================
 */

#include <cc1110.h>
#include <stdint.h>
#include <stdbool.h>

#if __has_include("config.h")
#include "config.h"
#elif __has_include("config.h.example")
#include "config.h.example"
#else
#error "No config.h or config.h.example found! Please create config.h."
#endif

/* ============================================================================
 * Configuration Constants & Register Safety Fixes
 * ============================================================================ */

/** @brief Enable DMA-based radio transmission (1 = DMA Trigger 19, 0 = CPU polled). */
#define USE_DMA          1

/** @brief Fixed packet length: 8B Header + 16B AES Ciphertext. */
#define PACKET_LEN       24

/**
 * @brief True address of FSCAL0 in XDATA space.
 * @details According to CC1110 datasheet SWRU105, 0xDF26 contains FSCAL0.
 * Writing 0xC2 here disables the frequency synthesizer. Must always remain 0x1F.
 */
#define TRUE_FSCAL0      (*((__xdata volatile uint8_t *)0xDF26))

/**
 * @brief True address of PA_TABLE0 in XDATA space.
 * @details 0xDF2E maps to PA_TABLE0 (+10 dBm = 0xC2).
 */
#define TRUE_PA_TABLE0   (*((__xdata volatile uint8_t *)0xDF2E))

/**
 * @brief CC1110 DMA Channel Descriptor Structure (8 bytes).
 * @details Conforms to TI SWRU105 Table 46 (DMA Configuration).
 */
typedef struct {
    uint8_t src_h;  /**< Source address high byte */
    uint8_t src_l;  /**< Source address low byte */
    uint8_t dst_h;  /**< Destination address high byte */
    uint8_t dst_l;  /**< Destination address low byte */
    uint8_t len_h;  /**< VLEN[7:5]=000, Transfer length high 5 bits */
    uint8_t len_l;  /**< Transfer length low 8 bits */
    uint8_t cfg0;   /**< WORDSIZE=0, TMODE=00, TRIG=19 (DMA_TRIG_RADIO) -> 0x13 */
    uint8_t cfg1;   /**< SRCINC=01, DESTINC=00, IRQMASK=1, M8=0, PRIORITY=10 -> 0x4A */
} dma_desc_t;

/* Global variables located in XRAM (__xdata) */
static __xdata dma_desc_t dma_desc;
static __xdata uint8_t tx_buf[PACKET_LEN];

/* Diagnostics and telemetry counters */
volatile __xdata uint8_t g_last_marcstate = 0;
volatile __xdata uint8_t g_last_rfif = 0;
volatile __xdata uint16_t g_tx_count = 0;
volatile __xdata uint16_t g_tx_fail = 0;

/**
 * @brief Busy-wait software millisecond delay.
 * @param ms Number of milliseconds to delay.
 * @note Calibrated for 26.000 MHz system clock (~2100 NOP iterations per ms).
 */
void delay_ms(uint16_t ms) {
    while (ms--) {
        for (uint16_t i = 0; i < 2100; i++) {
            __asm nop __endasm;
        }
    }
}

/* ============================================================================
 * Hardware Cryptographic Engine (AES-128 Coprocessor)
 * ============================================================================ */

/**
 * @brief Load a 128-bit key into the CC1110 AES hardware coprocessor.
 * @param key Pointer to 16-byte key in code memory (__code).
 */
void aes_load_key(const uint8_t __code *key) {
    uint16_t timeout = 50000;
    // Set command to Load Key: CMD = 10b (bit 2:1), Start: ST = 1 (bit 0)
    ENCCS = (0x02 << 1) | 0x01;
    for (uint8_t i = 0; i < 16; i++) {
        ENCDI = key[i];
    }
    // Wait until hardware coprocessor asserts RDY (bit 3)
    while (!(ENCCS & 0x08) && --timeout);
}

/**
 * @brief Encrypt a 16-byte block using hardware AES-128-ECB.
 * @param in Pointer to 16-byte plaintext input buffer.
 * @param out Pointer to 16-byte ciphertext output buffer.
 */
void aes_encrypt_block(const uint8_t *in, uint8_t *out) {
    uint16_t timeout = 50000;
    // Set MODE = 100b (ECB -> bit 6), CMD = 00b (Encrypt -> bit 2:1), ST = 1 (bit 0)
    ENCCS = 0x40 | 0x01;
    for (uint8_t i = 0; i < 16; i++) {
        ENCDI = in[i];
    }
    // Wait for RDY flag
    while (!(ENCCS & 0x08) && --timeout);
    // Read 16-byte encrypted ciphertext from hardware output register
    for (uint8_t i = 0; i < 16; i++) {
        out[i] = ENCDO;
    }
}

/* ============================================================================
 * Battery Voltage Monitoring (Internal ADC)
 * ============================================================================ */

/**
 * @brief Measure the battery supply voltage (VDD) using the internal ADC.
 * @return Battery voltage in millivolts (e.g. 3050 mV).
 * @note Uses the internal 1.25V bandgap voltage reference and VDD/3 channel.
 */
uint16_t read_battery_mv(void) {
    // ADCCON3 configuration:
    // EREF[7:6] = 00 (Internal 1.25V reference)
    // EDIV[5:4] = 11 (12-bit decimation rate)
    // ECH[3:0]  = 1111 (VDD/3 single-ended channel)
    // Value = 0x3F
    ADCCON3 = 0x3F;

    // Poll for End-Of-Conversion (ADCCON1 bit 7 = EOC)
    uint16_t timeout = 5000;
    while (!(ADCCON1 & 0x80) && --timeout);

    // Read 12-bit signed/unsigned result (left-justified across ADCH:ADCL)
    uint16_t adc = ((uint16_t)ADCL >> 4) | ((uint16_t)ADCH << 4);

    // Formula: VDD = 3 * (adc / 2047) * 1250 mV = (adc * 3750) / 2047
    uint32_t mv = ((uint32_t)adc * 3750UL) / 2047UL;
    return (uint16_t)mv;
}

/**
 * @brief Convert millivolts to an approximate battery percentage.
 * @param mv Battery voltage in millivolts.
 * @return Estimated remaining percentage (0..100%).
 * @note Linear approximation curve for a 3.0V Lithium cell (CR123A / CR2).
 *       Full charge >= 3000 mV, cutoff = 2200 mV.
 */
uint8_t calculate_battery_pct(uint16_t mv) {
    if (mv >= 3000) return 100;
    if (mv <= 2200) return 0;
    return (uint8_t)(((uint32_t)(mv - 2200) * 100UL) / 800UL);
}

/* ============================================================================
 * System Clock Initialization (26.000 MHz External Crystal)
 * ============================================================================ */

/**
 * @brief Initialize the high-speed system oscillator to 26 MHz XOSC.
 * @details Powers up the external crystal oscillator and cleanly switches
 * the CPU clock from the internal 13 MHz RC oscillator to the 26 MHz crystal.
 */
void clock_init(void) {
    uint16_t timeout;

    // Turn on both crystal oscillator and RC oscillator (OSC_PD = 0)
    SLEEP &= ~0x04;

    // Wait until 26 MHz XOSC is stable (SLEEP bit 6 = XOSC_STB)
    timeout = 50000;
    while (!(SLEEP & 0x40) && --timeout);

    // Select XOSC as system clock (CLKCON bit 6: OSC = 0) and set TICKSPD/CLKSPD to 26 MHz
    CLKCON &= ~0x47;

    // Confirm that the clock switch has taken effect
    timeout = 50000;
    while ((CLKCON & 0x40) && --timeout);

    // Power down the internal 13 MHz RC oscillator to save energy
    if (SLEEP & 0x40) {
        SLEEP |= 0x04;
    }
}

/* ============================================================================
 * GPIO & Sensor Pin Mapping
 * ============================================================================ */

/**
 * @brief Configure all microcontroller pins for sensors, LEDs, and headers.
 *
 * Pin assignments:
 * - P0_1: Tamper switch (Input with pull-up; 0 = closed/normal, 1 = open/tamper)
 * - P0_2: Red Status LED (Output; 1 = ON)
 * - P0_4: Green Status LED (Output; 1 = ON)
 * - P1_2: Vibration/Shock sensor (Input with pull-up)
 * - P1_7: Magnetic reed switch (Input with pull-up; 0 = closed/magnet, 1 = open)
 * - P2_1: DD (Debug Data) / Pin 5 on J2 header (Output diagnostic)
 * - P2_2: DC (Debug Clock) / Pin 4 on J2 header (Output diagnostic)
 */
void setup_gpio(void) {
    // Port 0 Configuration:
    // P0_1 (Tamper), P0_2 (Red LED), P0_4 (Green LED) as GPIO
    P0SEL &= ~(0x14 | 0x02);
    P0DIR |= 0x14;   // P0_2, P0_4 as outputs
    P0DIR &= ~0x02;  // P0_1 as input
    P0_2 = 0;        // Turn off Red LED
    P0_4 = 0;        // Turn off Green LED

    // Port 1 Configuration:
    // All pins as digital inputs (P1_7 = Reed, P1_2 = Shock)
    P1SEL = 0x00;
    P1DIR = 0x00;

    // Port 2 Configuration:
    // P2_0 as GPIO input; P2_1 (DD) and P2_2 (DC) as outputs
    P2SEL &= ~(0x01 | 0x02 | 0x04);
    P2DIR &= ~0x01; // P2_0 as input
    P2DIR |= 0x06;  // P2_1, P2_2 as outputs
    P2_1 = 0;
    P2_2 = 0;

    // Pull-up resistor activation:
    P0INP &= ~0x02;          // Pull-up enabled on P0_1 (Tamper)
    P1INP = 0x00;            // Pull-ups enabled on Port 1 (Reed & Shock)
    P2INP &= ~(0xE0 | 0x01); // Set Port 0, 1, 2 pull-up mode (not pull-down)
}

/* ============================================================================
 * Radio Transceiver Configuration (868.950 MHz)
 * ============================================================================ */

/**
 * @brief Initialize the built-in CC1100 radio core.
 * @details Configures RF frequency, modulation, data rate, sync word, and power.
 *
 * Register Calculation details:
 * - Reference RF Crystal: 27.000 MHz (X1)
 * - Target Frequency: 868.950 MHz
 *   FREQ = (868.950 * 2^16) / 27.000 = 2109167 = 0x202EEF (FREQ2=0x20, FREQ1=0x2E, FREQ0=0xEF)
 * - Data Rate: 38.383 kBaud
 *   MDMCFG4 = 0x8A, MDMCFG3 = 0x75 (38.386 kBaud at 27 MHz)
 * - Modulation: 2-FSK with 21.4 kHz frequency deviation (DEVIATN = 0x35)
 * - Sync Word: 0xD3 0x91 (30/32 bits match)
 * - Output Power: +10 dBm (PA_TABLE0 = 0xC2)
 */
void radio_init(void) {
    RFST = 0x04; // SIDLE strobe: put radio into idle state
    delay_ms(2);

    // Synchronization Word: 0xD3 0x91 (Matches gateway receiver)
    SYNC1 = 0xD3;
    SYNC0 = 0x91;

    // Packet Configuration: Fixed length 24 bytes + hardware 2-byte CRC
    PKTLEN   = PACKET_LEN;
    PKTCTRL1 = 0x04; // Append status bytes (RSSI, LQI/CRC)
    PKTCTRL0 = 0x04; // Fixed length mode, CRC calculation enabled

    // Intermediate Frequency (IF)
    CHANNR  = 0x00;
    FSCTRL1 = 0x06;
    FSCTRL0 = 0x00;

    // Frequency synthesis (868.950 MHz at 27.000 MHz crystal)
    FREQ2 = 0x20;
    FREQ1 = 0x2E;
    FREQ0 = 0xEF;

    // Modem configuration (38.383 kBaud, 2-FSK, 4B preamble, 30/32 sync bits)
    MDMCFG4 = 0x8A;
    MDMCFG3 = 0x75;
    MDMCFG2 = 0x03; // 2-FSK, 30/32 sync word bits
    MDMCFG1 = 0x22; // 4 bytes preamble
    MDMCFG0 = 0xF8;
    DEVIATN = 0x35; // 21.4 kHz deviation

    // State machine transitions: Autocalibrate IDLE -> TX, return to IDLE after packet
    MCSM2 = 0x07;
    MCSM1 = 0x00; // Return to IDLE after TX
    MCSM0 = 0x18; // Autocalibrate whenever transitioning from IDLE to TX

    FOCCFG = 0x16;
    BSCFG  = 0x6C;

    // Automatic Gain Control (AGC)
    AGCCTRL2 = 0x43;
    AGCCTRL1 = 0x40;
    AGCCTRL0 = 0x91;

    // Front-End configuration
    FREND1 = 0x56;
    FREND0 = 0x10; // Select PA_TABLE0 for transmit power

    // Frequency Synthesizer Calibration registers (SWRS033 Table 61)
    FSCAL3 = 0xE9;
    FSCAL2 = 0x2A;
    FSCAL1 = 0x00;
    FSCAL0 = 0x1F; // Critical: Must remain 0x1F (synthesizer charge pump calibration)

    // Hardware Test Registers (SWRS033 Table 61)
    TEST2 = 0x81;
    TEST1 = 0x35;
    TEST0 = 0x09;

    // Power Amplifier Table: +10 dBm (0xC2) across all entries
    PA_TABLE0 = 0xC2;
    PA_TABLE1 = 0xC2;
    PA_TABLE2 = 0xC2;
    PA_TABLE3 = 0xC2;
    PA_TABLE4 = 0xC2;
    PA_TABLE5 = 0xC2;
    PA_TABLE6 = 0xC2;
    PA_TABLE7 = 0xC2;
}

/* ============================================================================
 * Packet Transmission Engines (DMA vs CPU-Polled)
 * ============================================================================ */

/**
 * @brief Transmit packet using CPU polling via the RFD register.
 * @return True if transmission and CRC completed successfully.
 */
bool radio_send_cpu(void) {
    uint16_t timeout;

    // 1. Ensure radio is in IDLE
    RFST = 0x04; // SIDLE
    timeout = 5000;
    while ((MARCSTATE & 0x1F) != 0x01 && --timeout);

    // 2. Clear interrupt flags
    RFTXRXIF = 0;
    RFIF = 0;

    // 3. Strobe STX to start transmission
    RFST = 0x03; // STX

    // 4. Feed all PACKET_LEN bytes into the TX FIFO via RFD
    for (uint8_t i = 0; i < PACKET_LEN; i++) {
        timeout = 50000;
        while (!RFTXRXIF && --timeout) {
            if ((MARCSTATE & 0x1F) == 0x16) {
                RFST = 0x04; // Abort on TX FIFO underflow
                return false;
            }
        }
        if (!timeout) {
            RFST = 0x04;
            return false;
        }
        RFTXRXIF = 0;
        RFD = tx_buf[i];
    }

    // 5. Wait for packet completion including CRC (RFIF bit 4 = IRQ_DONE)
    timeout = 50000;
    while (!(RFIF & 0x10) && --timeout) {
        if ((MARCSTATE & 0x1F) == 0x16) {
            RFST = 0x04;
            return false;
        }
    }

    RFST = 0x04;
    RFTXRXIF = 0;
    RFIF = 0;

    return (timeout > 0);
}

/**
 * @brief Transmit packet using hardware DMA Trigger 19 (DMA_TRIG_RADIO).
 * @return True if DMA transfer and RF transmission completed successfully.
 * @note This is the factory-recommended transmission mode for the CC1110.
 */
bool radio_send_dma(void) {
    uint16_t timeout;

    // 1. Ensure radio is in IDLE
    RFST = 0x04; // SIDLE
    timeout = 5000;
    while ((MARCSTATE & 0x1F) != 0x01 && --timeout);

    // 2. Configure DMA Channel 0 descriptor
    dma_desc.src_h = (uint8_t)(((uint16_t)tx_buf) >> 8);
    dma_desc.src_l = (uint8_t)(((uint16_t)tx_buf) & 0xFF);
    dma_desc.dst_h = 0xDF;
    dma_desc.dst_l = 0xD9;       // X_RFD register address = 0xDFD9
    dma_desc.len_h = 0x00;
    dma_desc.len_l = PACKET_LEN; // 24 bytes fixed length
    dma_desc.cfg0  = 19;         // Trigger 19: DMA_TRIG_RADIO (0x13)
    dma_desc.cfg1  = 0x4A;       // SRCINC=1, DESTINC=0, IRQMASK=1, PRIORITY=High

    DMA0CFGH = (uint8_t)(((uint16_t)&dma_desc) >> 8);
    DMA0CFGL = (uint8_t)(((uint16_t)&dma_desc) & 0xFF);

    // 3. Clear flags
    RFTXRXIF = 0;
    RFIF = 0;
    DMAIRQ &= ~0x01; // Clear DMA Channel 0 complete flag

    // 4. Arm DMA Channel 0 followed by 9 NOPs as required by TI errata
    DMAARM = 0x01;
    __asm
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
    __endasm;

    // 5. Strobe STX to begin transmission
    RFST = 0x03;

    // 6. Wait until packet and CRC are fully transmitted (RFIF bit 4 = IRQ_DONE)
    timeout = 50000;
    while (!(RFIF & 0x10) && --timeout) {
        if ((MARCSTATE & 0x1F) == 0x16) {
            RFST = 0x04; // SIDLE
            RFIF = 0;
            return false; // Underflow occurred
        }
    }

    bool dma_done = (DMAIRQ & 0x01) != 0 || (RFIF & 0x10) != 0;

    // 7. Clear interrupt flags and return to IDLE
    RFST = 0x04;
    RFTXRXIF = 0;
    RFIF = 0;

    return (timeout > 0 && dma_done);
}

/**
 * @brief Wrapper function to send the current `tx_buf` buffer.
 * @return True on successful transmission.
 */
bool radio_send(void) {
    g_tx_count++;
#if USE_DMA
    bool ok = radio_send_dma();
#else
    bool ok = radio_send_cpu();
#endif
    if (!ok) {
        g_tx_fail++;
        g_last_marcstate = MARCSTATE & 0x1F;
        g_last_rfif = RFIF;
    }
    return ok;
}

/* ============================================================================
 * Main Application Routine
 * ============================================================================ */

void main(void) {
    // 0. Detect boot reset cause immediately from the SLEEP register
    // According to TI CC1110 SWRU105 Table 51, bits 4:3 are RST[1:0]:
    // 00 = Power-On Reset (POR) / Battery insertion
    // 01 = External Reset (RESET_N pin 31, connected to SW1 Tamper switch)
    // 10 = Watchdog Timer Reset
    // 11 = Clock Loss Reset
    uint8_t boot_sleep = SLEEP;
    uint8_t rst_cause = (boot_sleep >> 3) & 0x03;
    bool is_button_reset = (rst_cause == 0x01);
    bool is_por_reset    = (rst_cause == 0x00);

    // 1. Initialize GPIO pins immediately
    setup_gpio();

    // 2. Visual confirmation: Turn on Red LED during boot
    P0_2 = 1;

    // Flash both LEDs if booted from external reset button
    if (is_button_reset) {
        for (uint8_t i = 0; i < 2; i++) {
            P0_2 = 1; P0_4 = 1;
            delay_ms(50);
            P0_2 = 0; P0_4 = 0;
            delay_ms(50);
        }
    }

    // 3. Initialize 26 MHz system clock
    clock_init();

    // 4. Turn on Green LED once clock is stable
    P0_4 = 1;
    delay_ms(200);

    // 5. Initialize CC1100 radio core (868.950 MHz, +10 dBm)
    radio_init();

    // 6. Load AES-128 key into hardware cryptographic coprocessor
    aes_load_key(DEVICE_AES_KEY);

    // Turn off both LEDs prior to main telemetry loop
    P0_2 = 0;
    P0_4 = 0;
    delay_ms(200);

    static uint16_t seq = 0;
    uint8_t p1_2_prev = P1_2;
    uint8_t last_reed = P1_7;
    uint8_t last_tamper = P0_1;
    uint16_t shock_hold_ms = 0;
    bool first_packet = true;

    while (1) {
        bool send_immediate = false;
        bool shock_event = false;

        // First packet after boot/reset is transmitted immediately
        if (!first_packet) {
            // Monitor sensors in real-time during the 1000 ms interval.
            // If any sensor changes state, break immediately to transmit within < 2 ms!
            for (uint16_t ms = 0; ms < 1000; ms++) {
                // 1. Check Magnetic Reed Switch (P1_7)
                uint8_t curr_reed = P1_7;
                if (curr_reed != last_reed) {
                    last_reed = curr_reed;
                    send_immediate = true;
                    break;
                }

                // 2. Check Tamper / Sabotage switch (P0_1)
                // 1 = Cover removed / Alarm, 0 = Cover installed / Secured
                uint8_t curr_tamper = P0_1;
                if (curr_tamper != last_tamper) {
                    last_tamper = curr_tamper;
                    send_immediate = true;
                    break;
                }

                // 3. Check Vibration / Shock Sensor (P1_2) with rapid oversampling
                for (uint8_t s = 0; s < 10; s++) {
                    uint8_t curr_p1_2 = P1_2;
                    if (curr_p1_2 != p1_2_prev) {
                        p1_2_prev = curr_p1_2;
                        shock_event = true;
                        shock_hold_ms = 1000; // Hold shock state for 1 second
                        send_immediate = true;
                        break;
                    }
                }
                if (send_immediate) {
                    break;
                }

                // Decrement shock hold timer
                if (shock_hold_ms > 0) shock_hold_ms--;

                // Real-time LED feedback: Red LED lights up if door is open, shock active, or cover off
                if (curr_reed != 0 || shock_hold_ms > 0 || curr_tamper != 0) {
                    P0_2 = 1;
                } else {
                    P0_2 = 0;
                }

                delay_ms(1);
            }
        } else {
            first_packet = false;
        }

        // Sample current hardware sensor states for telemetry packet
        uint8_t reed_val = P1_7;
        bool is_open = (reed_val != 0);
        bool is_shock = (shock_event || shock_hold_ms > 0);
        bool is_tamper = (P0_1 != 0);

        // Update Red LED status
        if (is_open || is_shock || is_tamper) {
            P0_2 = 1;
        } else {
            P0_2 = 0;
        }

        // Measure battery voltage and calculate percentage
        uint16_t batt_mv = read_battery_mv();
        uint8_t batt_pct = calculate_battery_pct(batt_mv);

        // --------------------------------------------------------------------
        // Packet Construction (24 Bytes)
        // --------------------------------------------------------------------

        // 1. Plaintext Header (Bytes 0..7): 8-character ASCII Device ID
        for (uint8_t i = 0; i < 8; i++) {
            tx_buf[i] = (uint8_t)DEVICE_ID[i];
        }

        // 2. Plaintext Payload (16 Bytes) before AES-128 encryption
        uint8_t plain[16];
        plain[0] = (uint8_t)(seq >> 8);
        plain[1] = (uint8_t)(seq & 0xFF);

        // Status Bitflags
        uint8_t flags = 0;
        if (is_open) flags |= 0x01;                       // Bit 0: Reed switch open
        if (is_shock) flags |= 0x02;                      // Bit 1: Vibration / shock
        if (is_tamper) flags |= 0x04;                     // Bit 2: Tamper / Sabotage (Cover off)
        if (is_por_reset && seq < 3) flags |= 0x08;       // Bit 3: Power-on reset (Battery changed)
        if (batt_mv < 2400) flags |= 0x10;                // Bit 4: Low battery warning (<2.4V)

        plain[2] = flags;
        plain[3] = batt_pct;
        plain[4] = (uint8_t)(batt_mv >> 8);
        plain[5] = (uint8_t)(batt_mv & 0xFF);
        plain[6] = (uint8_t)DEVICE_ID[0]; // Echo Device ID for anti-spoofing
        plain[7] = (uint8_t)DEVICE_ID[1];
        plain[8] = (uint8_t)DEVICE_ID[2];
        plain[9] = (uint8_t)DEVICE_ID[3];
        plain[10] = 'V';                  // Magic bytes "VS" for authentication
        plain[11] = 'S';
        plain[12] = P0;                   // Diagnostic: Port 0 raw states
        plain[13] = P1;                   // Diagnostic: Port 1 raw states
        plain[14] = P2;                   // Diagnostic: Port 2 raw states
        plain[15] = (boot_sleep >> 3) & 0x03; // Diagnostic: Boot reset reason

        // 3. Encrypt the 16 payload bytes directly into tx_buf[8..23]
        aes_encrypt_block(plain, &tx_buf[8]);
        seq++;

        // 4. Visual & Hardware pulse during transmission
        P0_4 = 1;      // Turn on Green LED during RF pulse
        P2_1 = !P2_1;  // Toggle DD pin for oscilloscope / logic analyzer hookup

        bool ok = radio_send();
        P2_2 = ok ? 1 : 0; // DC pin indicates transmission success

        delay_ms(40);
        P0_4 = 0;      // Turn off Green LED

        // Re-evaluate Red LED state
        if (P1_7 != 0 || shock_hold_ms > 0 || P0_1 != 0) {
            P0_2 = 1;
        } else {
            P0_2 = 0;
        }

        // For immediate trigger events, apply a short 60 ms refractory period to prevent RF channel flooding
        if (send_immediate) {
            delay_ms(60);
        }
    }
}
