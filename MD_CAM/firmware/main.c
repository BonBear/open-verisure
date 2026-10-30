/**
 * @file main.c
 * @brief Verisure Motion Detector Camera (GW MD CAM2 / SECUR-PCAM-BLACKFIN) - CC1110F32 Firmware (Phase 1)
 *
 * Implements the 868.950 MHz Sub-1GHz RF telemetry and motion/tamper alarm protocol
 * for the Verisure GW MD CAM2 sensor powered by a Texas Instruments CC1110F32 SoC.
 *
 * ============================================================================
 * VERIFIED HARDWARE PIN MAPPING (GW MD CAM2 - U15 CC1110F32):
 * ============================================================================
 * 1. Sensors & Switches:
 *    - P1_7 (Pin 32) : PIR Motion Sensor ('ALARM' test pad).
 *                      Active LOW: 1 (3.08V) = Idle/Clear, 0 (0.0V) = Motion Detected!
 *    - P0_6 (Pin 12) : Rear Tamper Switch (SW3 plunger against wall, closes to VBAT+).
 *    - P0_7 (Pin 13) : Front Tamper Switch / Auxiliary input candidate.
 *    - RESET_N (Pin 31): Connected to SW1 & J6-4 via 2.7 kOhm resistor.
 *
 * 2. Inter-IC Buses (Blackfin ADSP-BF512 U5 & SPI Flash U3):
 *    - P1_3 (Pin 36) : SPI SCK  (USART0 Alt. 2)
 *    - P1_4 (Pin 35) : SPI MISO (USART0 Alt. 2)
 *    - P1_5 (Pin 34) : SPI MOSI (USART0 Alt. 2)
 *    - P1_2 (Pin 1)  : SPI SS1  (Upper SS pad - USART0 Alt. 2)
 *    - P1_1 (Pin 3)  : SPI SS2  (Lower SS pad)
 *    - P0_4 (Pin 9)  : UART TX  (USART1 Alt. 1)
 *    - P0_5 (Pin 11) : UART RX  (USART1 Alt. 1)
 *
 * 3. Programming Header (J6):
 *    - J6-1 : VDD_SENSE (1 kOhm to DVDD Pin 10)
 *    - J6-2 : P2_1 / DD (Debug Data, Pin 15)
 *    - J6-3 : P2_2 / DC (Debug Clock, Pin 16)
 *    - J6-4 : RESET_N   (Pin 31 via 2.7 kOhm)
 *    - J6-5 : GND
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
#error "No config.h or config.h.example found! Please generate config.h with derive_key.py."
#endif

/** @brief Enable DMA-based radio transmission (1 = DMA Trigger 19, 0 = CPU polled). */
#define USE_DMA                1

/** @brief Fixed packet length: 8B Header + 16B AES Ciphertext. */
#define PACKET_LEN             24

/** @brief Telemetry heartbeat interval in seconds when no events occur (1 hour). */
#define HEARTBEAT_INTERVAL_SEC 3600

/**
 * @brief CC1110 DMA Channel Descriptor Structure (8 bytes, SWRU105 Table 46).
 */
typedef struct {
    uint8_t src_h;
    uint8_t src_l;
    uint8_t dst_h;
    uint8_t dst_l;
    uint8_t len_h;
    uint8_t len_l;
    uint8_t cfg0;
    uint8_t cfg1;
} dma_desc_t;

/* Global variables in XRAM (__xdata) - zero dynamic allocation */
static __xdata dma_desc_t dma_desc;
static __xdata uint8_t tx_buf[PACKET_LEN];

volatile __xdata uint8_t g_last_marcstate = 0;
volatile __xdata uint8_t g_last_rfif = 0;
volatile __xdata uint16_t g_tx_count = 0;
volatile __xdata uint16_t g_tx_fail = 0;

/**
 * @brief Busy-wait software millisecond delay calibrated for 27 MHz XOSC.
 */
void delay_ms(uint16_t ms) {
    while (ms--) {
        for (uint16_t i = 0; i < 2180; i++) {
            __asm nop __endasm;
        }
    }
}

/* ============================================================================
 * Hardware Cryptographic Engine (AES-128 Coprocessor)
 * ============================================================================ */

void aes_load_key(const uint8_t __code *key) {
    uint16_t timeout = 50000;
    ENCCS = (0x02 << 1) | 0x01; // CMD = Load Key (10b), ST = 1
    for (uint8_t i = 0; i < 16; i++) {
        ENCDI = key[i];
    }
    while (!(ENCCS & 0x08) && --timeout);
}

void aes_encrypt_block(const uint8_t *in, uint8_t *out) {
    uint16_t timeout = 50000;
    ENCCS = 0x40 | 0x01; // MODE = ECB (100b), CMD = Encrypt (00b), ST = 1
    for (uint8_t i = 0; i < 16; i++) {
        ENCDI = in[i];
    }
    while (!(ENCCS & 0x08) && --timeout);
    for (uint8_t i = 0; i < 16; i++) {
        out[i] = ENCDO;
    }
}

/* ============================================================================
 * Battery Voltage Monitoring (Internal ADC VDD/3)
 * ============================================================================ */

uint16_t read_battery_mv(void) {
    ADCCON3 = 0x3F; // Internal 1.25V ref, 12-bit decimation, VDD/3 channel
    uint16_t timeout = 5000;
    while (!(ADCCON1 & 0x80) && --timeout);

    uint16_t adc = ((uint16_t)ADCL >> 4) | ((uint16_t)ADCH << 4);
    uint32_t mv = ((uint32_t)adc * 3750UL) / 2047UL;
    return (uint16_t)mv;
}

uint8_t calculate_battery_pct(uint16_t mv) {
    if (mv >= 3000) return 100;
    if (mv <= 2200) return 0;
    return (uint8_t)(((uint32_t)(mv - 2200) * 100UL) / 800UL);
}

/* ============================================================================
 * System Clock Initialization (27.000 MHz External Crystal Y1)
 * ============================================================================ */

void clock_init(void) {
    uint16_t timeout;

    // Power on both oscillators (OSC_PD = 0)
    SLEEP &= ~0x04;

    // Wait for 27 MHz crystal oscillator stability (XOSC_STB = bit 6)
    timeout = 50000;
    while (!(SLEEP & 0x40) && --timeout);

    // Switch system clock to XOSC at full speed (27 MHz)
    CLKCON &= ~0x47;

    timeout = 50000;
    while ((CLKCON & 0x40) && --timeout);

    // Power down internal HS RC oscillator to save power
    if (SLEEP & 0x40) {
        SLEEP |= 0x04;
    }
}

/* ============================================================================
 * GPIO & Sensor Configuration (GW MD CAM2)
 * ============================================================================ */

/**
 * @brief Configure CC1110 ports for PIR motion, Tamper switch, and safe idle state.
 *
 * - P1_7 (Pin 32): PIR ALARM input (Active LOW: 0 = Motion, 1 = Idle).
 * - P0_6 (Pin 12): Rear Tamper switch SW3 (closes to VBAT+). Configured with pull-down on Port 0.
 * - P2_1 / P2_2  : Kept as inputs so J6 Debug interface (DD/DC) is never blocked.
 */
void setup_gpio(void) {
    // All Port 0 pins as GPIO inputs; tristate UART pins P0_4..P0_5 (0x30) to unpowered DSP
    P0SEL = 0x00;
    P0DIR = 0x00;
    P0INP = 0x30;

    // All Port 1 pins as GPIO inputs; tristate SPI pins P1_0..P1_5 (0x3F) to unpowered DSP
    P1SEL = 0x00;
    P1DIR = 0x00;
    P1INP = 0x3F;

    // Port 2 pins as GPIO inputs (leave P2_1=DD and P2_2=DC free for J6 debugger)
    P2SEL &= ~0x07;
    P2DIR &= ~0x07;

    // Default P2INP: Port 0 pull-DOWN (0x20), Port 1 & 2 pull-UP
    P2INP = (P2INP & ~0xC0) | 0x20;
}

/* ============================================================================
 * Radio Transceiver Configuration (868.950 MHz @ 27.000 MHz Crystal Y1)
 * ============================================================================ */

void radio_init(void) {
    RFST = 0x04; // SIDLE
    delay_ms(2);

    // Verisure Sync Word: 0xD3 0x91
    SYNC1 = 0xD3;
    SYNC0 = 0x91;

    // Fixed 24-byte packet + 2-byte hardware CRC
    PKTLEN   = PACKET_LEN;
    PKTCTRL1 = 0x04;
    PKTCTRL0 = 0x04;

    CHANNR  = 0x00;
    FSCTRL1 = 0x06;
    FSCTRL0 = 0x00;

    // 868.950 MHz carrier for 27.000 MHz crystal Y1 (0x202EEF)
    FREQ2 = 0x20;
    FREQ1 = 0x2E;
    FREQ0 = 0xEF;

    // 38.383 kBaud, 2-FSK, 21.4 kHz deviation at 27.000 MHz crystal
    MDMCFG4 = 0x8A;
    MDMCFG3 = 0x75;
    MDMCFG2 = 0x03;
    MDMCFG1 = 0x22;
    MDMCFG0 = 0xF8;
    DEVIATN = 0x35;

    MCSM2 = 0x07;
    MCSM1 = 0x00;
    MCSM0 = 0x18;

    FOCCFG = 0x16;
    BSCFG  = 0x6C;

    AGCCTRL2 = 0x43;
    AGCCTRL1 = 0x40;
    AGCCTRL0 = 0x91;

    FREND1 = 0x56;
    FREND0 = 0x10;

    FSCAL3 = 0xE9;
    FSCAL2 = 0x2A;
    FSCAL1 = 0x00;
    FSCAL0 = 0x1F; // Charge pump calibration must remain 0x1F

    TEST2 = 0x81;
    TEST1 = 0x35;
    TEST0 = 0x09;

    // +10 dBm Output Power
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
 * DMA Trigger 19 Radio Packet Transmission
 * ============================================================================ */

bool radio_send_dma(void) {
    uint16_t timeout;

    RFST = 0x04; // SIDLE
    timeout = 5000;
    while ((MARCSTATE & 0x1F) != 0x01 && --timeout);

    dma_desc.src_h = (uint8_t)(((uint16_t)tx_buf) >> 8);
    dma_desc.src_l = (uint8_t)(((uint16_t)tx_buf) & 0xFF);
    dma_desc.dst_h = 0xDF;
    dma_desc.dst_l = 0xD9;       // X_RFD = 0xDFD9
    dma_desc.len_h = 0x00;
    dma_desc.len_l = PACKET_LEN; // 24 bytes
    dma_desc.cfg0  = 19;         // Trigger 19: DMA_TRIG_RADIO
    dma_desc.cfg1  = 0x4A;       // SRCINC=1, DESTINC=0, High priority

    DMA0CFGH = (uint8_t)(((uint16_t)&dma_desc) >> 8);
    DMA0CFGL = (uint8_t)(((uint16_t)&dma_desc) & 0xFF);

    RFTXRXIF = 0;
    RFIF = 0;
    DMAIRQ &= ~0x01;

    // Arm DMA Channel 0 followed by 9 NOPs (TI CC1110 datasheet requirement)
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

    RFST = 0x03; // STX

    timeout = 50000;
    while (!(RFIF & 0x10) && --timeout) {
        if ((MARCSTATE & 0x1F) == 0x16) {
            RFST = 0x04;
            RFIF = 0;
            return false;
        }
    }

    bool dma_done = (DMAIRQ & 0x01) != 0 || (RFIF & 0x10) != 0;

    RFST = 0x04;
    RFTXRXIF = 0;
    RFIF = 0;

    return (timeout > 0 && dma_done);
}

bool radio_send(void) {
    g_tx_count++;
    bool ok = radio_send_dma();
    if (!ok) {
        g_tx_fail++;
        g_last_marcstate = MARCSTATE & 0x1F;
        g_last_rfif = RFIF;
    }
    return ok;
}

/* ============================================================================
 * Dual Pull-Up / Pull-Down GPIO Diagnostic Scanner
 * ============================================================================ */

static void read_gpio_scan(uint8_t *p0_pu, uint8_t *p0_pd, uint8_t *p1_pu, uint8_t *p1_pd, uint8_t *p2_packed) {
    uint8_t d;

    // 1. Enable internal Pull-UPs on Port 0, Port 1, Port 2 (PDUP0=0, PDUP1=0, PDUP2=0)
    // Mask out unpowered DSP SPI/UART pins (P0 mask 0xC1 = P0_7,P0_6,P0_0; P1 mask 0xC0 = P1_7,P1_6; P2 mask 0x01 = P2_0)
    P2INP &= ~0xE0;
    for (d = 0; d < 40; d++) {
        __asm nop __endasm;
    }
    *p0_pu = P0 & 0xC1;
    *p1_pu = P1 & 0xC0;
    uint8_t p2_u = P2 & 0x01;

    // 2. Enable internal Pull-DOWNs on Port 0, Port 1, Port 2 (PDUP0=1, PDUP1=1, PDUP2=1)
    P2INP |= 0xE0;
    for (d = 0; d < 40; d++) {
        __asm nop __endasm;
    }
    *p0_pd = P0 & 0xC1;
    *p1_pd = P1 & 0xC0;
    uint8_t p2_d = P2 & 0x01;

    // Restore default: Port 0 pull-DOWN (0x20), Port 1 & 2 pull-UP
    P2INP = (P2INP & ~0xC0) | 0x20;
    for (d = 0; d < 20; d++) {
        __asm nop __endasm;
    }

    *p2_packed = (uint8_t)((p2_u << 4) | p2_d);
}

/* ============================================================================
 * Main Application Loop
 * ============================================================================ */

void main(void) {
    uint8_t boot_sleep = SLEEP;
    uint8_t rst_cause = (boot_sleep >> 3) & 0x03;
    // Set POR flag on Power-On Reset (0x00) or External Reset via RESET_N/J6-4/SW1 (0x01)
    // so the Gateway resets its replay sequence counter after reflashing or rebooting.
    bool is_por_reset = (rst_cause == 0x00 || rst_cause == 0x01);

    // 1. Initialize GPIO pins
    setup_gpio();

    // 2. Switch system clock to 27.000 MHz crystal Y1
    clock_init();
    delay_ms(50);

    // 3. Initialize 868.950 MHz radio core
    radio_init();

    // 4. Load derived device AES-128 key into hardware coprocessor
    aes_load_key(DEVICE_AES_KEY);
    delay_ms(50);

    static uint16_t seq = 0;
    uint8_t p0_pu, p0_pd, p1_pu, p1_pd, p2_packed;
    read_gpio_scan(&p0_pu, &p0_pd, &p1_pu, &p1_pd, &p2_packed);

    // Record boot idle baseline for all non-PIR pins to detect SW3 regardless of pin/polarity
    uint8_t idle_p0_pu = p0_pu;
    uint8_t idle_p0_pd = p0_pd;
    uint8_t idle_p1_pu = p1_pu & 0x7F;
    uint8_t idle_p1_pd = p1_pd & 0x7F;
    uint8_t idle_p2    = p2_packed;

    uint8_t last_p0_pu = p0_pu;
    uint8_t last_p0_pd = p0_pd;
    uint8_t last_p1_pu = p1_pu;
    uint8_t last_p1_pd = p1_pd;
    uint8_t last_p2    = p2_packed;
    bool first_packet  = true;

    while (1) {
        bool send_immediate = false;

        if (!first_packet) {
            uint8_t debounce = 0;
            // Poll sensors every 1 ms; transmit immediately on any PIR or switch transition
            for (uint16_t sec = 0; sec < HEARTBEAT_INTERVAL_SEC; sec++) {
                for (uint16_t ms = 0; ms < 1000; ms++) {
                    read_gpio_scan(&p0_pu, &p0_pd, &p1_pu, &p1_pd, &p2_packed);

                    if (p0_pu != last_p0_pu || p0_pd != last_p0_pd ||
                        p1_pu != last_p1_pu || p1_pd != last_p1_pd ||
                        p2_packed != last_p2) {
                        debounce++;
                        if (debounce >= 5) {
                            last_p0_pu = p0_pu;
                            last_p0_pd = p0_pd;
                            last_p1_pu = p1_pu;
                            last_p1_pd = p1_pd;
                            last_p2    = p2_packed;
                            send_immediate = true;
                            break;
                        }
                    } else {
                        debounce = 0;
                    }

                    delay_ms(1);
                }
                if (send_immediate) {
                    break;
                }
            }
        } else {
            read_gpio_scan(&p0_pu, &p0_pd, &p1_pu, &p1_pd, &p2_packed);
            last_p0_pu = p0_pu;
            last_p0_pd = p0_pd;
            last_p1_pu = p1_pu;
            last_p1_pd = p1_pd;
            last_p2    = p2_packed;
            first_packet = false;
        }

        // Sample sensor states:
        // PIR ALARM (P1_7): Active LOW (0 = Motion Detected, 1 = Idle)
        bool is_motion = ((p1_pu & 0x80) == 0);
        // Rear Tamper SW3: True whenever any non-PIR pin on P0, P1, or P2 deviates from its idle state
        bool is_tamper = (p0_pu != idle_p0_pu) ||
                         (p0_pd != idle_p0_pd) ||
                         ((p1_pu & 0x7F) != idle_p1_pu) ||
                         ((p1_pd & 0x7F) != idle_p1_pd) ||
                         (p2_packed != idle_p2);

        uint16_t batt_mv = read_battery_mv();
        uint8_t batt_pct = calculate_battery_pct(batt_mv);

        // 1. Plaintext Header (Bytes 0..7): 8-char ASCII Device ID ("2NCEKS3T")
        for (uint8_t i = 0; i < 8; i++) {
            tx_buf[i] = (uint8_t)DEVICE_ID[i];
        }

        // 2. Plaintext Payload (16 Bytes)
        uint8_t plain[16];
        plain[0] = (uint8_t)(seq >> 8);
        plain[1] = (uint8_t)(seq & 0xFF);

        uint8_t flags = 0;
        if (is_motion) flags |= 0x01;                     // Bit 0: Motion Detected (PIR ALARM = 0V)
        if (is_tamper) flags |= 0x04;                     // Bit 2: Tamper / Sabotage (SW3)
        if (is_por_reset && seq < 3) flags |= 0x08;       // Bit 3: Power-On Reset
        if (batt_mv < 2400) flags |= 0x10;                // Bit 4: Low Battery (<2.4V)

        plain[2]  = flags;
        plain[3]  = batt_pct;
        plain[4]  = (uint8_t)(batt_mv >> 8);
        plain[5]  = (uint8_t)(batt_mv & 0xFF);
        plain[6]  = (uint8_t)DEVICE_ID[0];
        plain[7]  = (uint8_t)DEVICE_ID[1];
        plain[8]  = (uint8_t)DEVICE_ID[2];
        plain[9]  = (uint8_t)DEVICE_ID[3];
        plain[10] = 'V';
        plain[11] = 'S';
        plain[12] = p0_pu;                // Live diagnostic: Port 0 with Pull-UP
        plain[13] = p0_pd;                // Live diagnostic: Port 0 with Pull-DOWN
        plain[14] = p1_pu;                // Live diagnostic: Port 1 with Pull-UP (bit 7 = PIR)
        plain[15] = p2_packed;            // Live diagnostic: Port 2 (hi nibble=PU, lo nibble=PD)

        // 3. Hardware AES-128-ECB encryption
        aes_encrypt_block(plain, &tx_buf[8]);
        seq++;

        // 4. Transmit over 868.950 MHz Sub-1GHz RF via DMA Trigger 19
        radio_send();

        // Short debounce delay after immediate sensor triggers
        if (send_immediate) {
            delay_ms(80);
        }
    }
}
