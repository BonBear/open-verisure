/**
 * @file main.c
 * @brief Verisure Motion Detector Camera (GW MD CAM2 / SECUR-PCAM-BLACKFIN) - CC1110F32 Firmware
 *
 * 868.950 MHz AES-128 telemetry with PM2 Deep Sleep:
 *  - PIR Motion (P1_7, active LOW) wakes the CPU instantly via Port 1 interrupt.
 *  - Sleep Timer wakes the CPU every 0.5 s to sample the rear tamper switch SW3
 *    (dual pull-up/pull-down scan of P0_7/P0_6/P0_0/P1_6/P2_0 compared with the
 *    boot baseline) and to count down the periodic heartbeat.
 *  - 27 MHz XOSC + radio are only powered while transmitting.
 *
 * Tamper polarity (wall mount): batteries are ALWAYS inserted while the unit is off the
 * wall, so the boot state = SW3 released = TAMPER. SW3 pressed against the wall = CLEAR.
 *
 * Pin map: see Hardware_Pinout_och_Matprotokoll.md (J6: DD=P2_1, DC=P2_2, RESET_N).
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

/** @brief Fixed packet length: 8B Header + 16B AES Ciphertext. */
#define PACKET_LEN        24

/** @brief Sleep Timer tick: WOR_RES=2 (1024 x 32 kHz periods = 31.25 ms) x 16 = 0.5 s. */
#define ST_WOR_RES        0x02
#define ST_WOREVT         16

/** @brief Heartbeat interval in 0.5 s ticks (7200 = 1 hour). */
#define HEARTBEAT_TICKS   7200

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

volatile __xdata uint16_t g_tx_count = 0;
volatile __xdata uint16_t g_tx_fail = 0;

/** @brief Set by Sleep Timer ISR on every 0.5 s tick. */
volatile __data uint8_t g_tick = 0;
/** @brief Set by Port 1 ISR on PIR edge. */
volatile __data uint8_t g_pir_edge = 0;

/* ============================================================================
 * Interrupt Service Routines (PM2 wake-up sources) - kept minimal
 * ============================================================================ */

/** @brief Sleep Timer Event0 (Vector 5). */
void st_isr(void) __interrupt(ST_VECTOR) {
    SLEEP &= ~0x03;   // Cancel pending PM entry
    WORIRQ &= ~0x01;  // Clear EVENT0_FLAG (keep EVENT0_MASK)
    STIF = 0;
    g_tick = 1;
}

/** @brief Port 1 (Vector 15) - PIR ALARM on P1_7. PxIFG must be cleared before PxIF. */
void p1_isr(void) __interrupt(P1INT_VECTOR) {
    SLEEP &= ~0x03;
    P1IFG = 0x00;
    P1IF = 0;
    g_pir_edge = 1;
}

/* ============================================================================
 * Delays
 * ============================================================================ */

/** @brief Busy-wait ms delay calibrated for 27 MHz (takes 2x longer on 13.5 MHz RCOSC). */
void delay_ms(uint16_t ms) {
    while (ms--) {
        for (uint16_t i = 0; i < 2180; i++) {
            __asm nop __endasm;
        }
    }
}

/* ============================================================================
 * AES-128 Coprocessor
 * ============================================================================ */

void aes_load_key(const uint8_t __code *key) {
    uint16_t timeout = 50000;
    ENCCS = (0x02 << 1) | 0x01; // CMD = Load Key, ST = 1
    for (uint8_t i = 0; i < 16; i++) {
        ENCDI = key[i];
    }
    while (!(ENCCS & 0x08) && --timeout);
}

void aes_encrypt_block(const uint8_t *in, uint8_t *out) {
    uint16_t timeout = 50000;
    ENCCS = 0x40 | 0x01; // MODE = ECB, CMD = Encrypt, ST = 1
    for (uint8_t i = 0; i < 16; i++) {
        ENCDI = in[i];
    }
    while (!(ENCCS & 0x08) && --timeout);
    for (uint8_t i = 0; i < 16; i++) {
        out[i] = ENCDO;
    }
}

/* ============================================================================
 * Battery (VDD/3)
 * ============================================================================ */

uint16_t read_battery_mv(void) {
    ADCCON3 = 0x3F; // Internal 1.25V ref, 12-bit, VDD/3
    uint16_t timeout = 5000;
    while (!(ADCCON1 & 0x80) && --timeout);

    uint16_t adc = ((uint16_t)ADCL >> 4) | ((uint16_t)ADCH << 4);
    return (uint16_t)(((uint32_t)adc * 3750UL) / 2047UL);
}

uint8_t calculate_battery_pct(uint16_t mv) {
    if (mv >= 3000) return 100;
    if (mv <= 2200) return 0;
    return (uint8_t)(((uint32_t)(mv - 2200) * 100UL) / 800UL);
}

/* ============================================================================
 * Clock switching: 27 MHz XOSC (TX) <-> 13.5 MHz HS RCOSC (required before PM2)
 * ============================================================================ */

/** @brief Run on 27.000 MHz crystal Y1 (needed for radio). */
static void clock_xosc(void) {
    uint16_t timeout;
    SLEEP &= ~0x04;                               // Power up both HS oscillators
    timeout = 50000;
    while (!(SLEEP & 0x40) && --timeout);         // XOSC_STB
    CLKCON = (CLKCON & 0x80);                     // OSC=0 (XOSC), TICKSPD=0, CLKSPD=0
    timeout = 50000;
    while ((CLKCON & 0x40) && --timeout);
    SLEEP |= 0x04;                                // Power down unused HS RCOSC
}

/** @brief Run on 13.5 MHz HS RCOSC and power down the 27 MHz crystal. */
static void clock_rcosc(void) {
    uint16_t timeout;
    RFST = 0x04;                                  // Radio SIDLE
    SLEEP &= ~0x04;
    timeout = 50000;
    while (!(SLEEP & 0x20) && --timeout);         // HFRC_STB
    CLKCON = (CLKCON & 0x80) | 0x40 | 0x08 | 0x01; // OSC=1, TICKSPD=001, CLKSPD=001
    timeout = 50000;
    while (!(CLKCON & 0x40) && --timeout);
    SLEEP |= 0x04;                                // Power down unused 27 MHz XOSC
}

/* ============================================================================
 * GPIO
 * ============================================================================ */

void setup_gpio(void) {
    // Port 0 inputs; tristate UART pins P0_4..P0_5 to the unpowered DSP
    P0SEL = 0x00;
    P0DIR = 0x00;
    P0INP = 0x30;

    // Port 1 inputs; tristate SPI pins P1_0..P1_5 to the unpowered DSP; pull-up on P1_6/P1_7
    P1SEL = 0x00;
    P1DIR = 0x00;
    P1INP = 0x3F;

    // Port 2: keep P2_1 (DD) / P2_2 (DC) as inputs for the J6 debugger
    P2SEL &= ~0x07;
    P2DIR &= ~0x07;

    // Default: Port 0 pull-DOWN, Port 1 & 2 pull-UP (P1_7 PIR is active LOW)
    P2INP = (P2INP & ~0xC0) | 0x20;
}

/**
 * @brief Dual pull-up / pull-down scan of the candidate switch pins.
 * Masks: P0 0xC1 (P0_7, P0_6, P0_0), P1 0xC0 (P1_7, P1_6), P2 0x01 (P2_0).
 */
static void read_gpio_scan(uint8_t *p0_pu, uint8_t *p0_pd, uint8_t *p1_pu, uint8_t *p1_pd, uint8_t *p2_packed) {
    uint8_t d;
    uint8_t p2_u, p2_d;

    P2INP &= ~0xE0; // All ports pull-UP
    for (d = 0; d < 20; d++) {
        __asm nop __endasm;
    }
    *p0_pu = P0 & 0xC1;
    *p1_pu = P1 & 0xC0;
    p2_u = P2 & 0x01;

    P2INP |= 0xE0;  // All ports pull-DOWN
    for (d = 0; d < 20; d++) {
        __asm nop __endasm;
    }
    *p0_pd = P0 & 0xC1;
    *p1_pd = P1 & 0xC0;
    p2_d = P2 & 0x01;

    P2INP = (P2INP & ~0xC0) | 0x20; // Restore default
    for (d = 0; d < 20; d++) {
        __asm nop __endasm;
    }

    *p2_packed = (uint8_t)((p2_u << 4) | p2_d);
}

/* ============================================================================
 * Radio (868.950 MHz @ 27.000 MHz crystal Y1)
 * ============================================================================ */

void radio_init(void) {
    RFST = 0x04; // SIDLE
    delay_ms(1);

    SYNC1 = 0xD3;
    SYNC0 = 0x91;

    PKTLEN   = PACKET_LEN;
    PKTCTRL1 = 0x04;
    PKTCTRL0 = 0x04;

    CHANNR  = 0x00;
    FSCTRL1 = 0x06;
    FSCTRL0 = 0x00;

    FREQ2 = 0x20; // 868.950 MHz (0x202EEF)
    FREQ1 = 0x2E;
    FREQ0 = 0xEF;

    MDMCFG4 = 0x8A; // 38.383 kBaud, 2-FSK
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

    PA_TABLE0 = 0xC2; // +10 dBm
    PA_TABLE1 = 0xC2;
    PA_TABLE2 = 0xC2;
    PA_TABLE3 = 0xC2;
    PA_TABLE4 = 0xC2;
    PA_TABLE5 = 0xC2;
    PA_TABLE6 = 0xC2;
    PA_TABLE7 = 0xC2;
}

bool radio_send_dma(void) {
    uint16_t timeout;

    RFST = 0x04; // SIDLE
    timeout = 5000;
    while ((MARCSTATE & 0x1F) != 0x01 && --timeout);

    dma_desc.src_h = (uint8_t)(((uint16_t)tx_buf) >> 8);
    dma_desc.src_l = (uint8_t)(((uint16_t)tx_buf) & 0xFF);
    dma_desc.dst_h = 0xDF;
    dma_desc.dst_l = 0xD9;       // X_RFD
    dma_desc.len_h = 0x00;
    dma_desc.len_l = PACKET_LEN;
    dma_desc.cfg0  = 19;         // DMA_TRIG_RADIO
    dma_desc.cfg1  = 0x4A;

    DMA0CFGH = (uint8_t)(((uint16_t)&dma_desc) >> 8);
    DMA0CFGL = (uint8_t)(((uint16_t)&dma_desc) & 0xFF);

    RFTXRXIF = 0;
    RFIF = 0;
    DMAIRQ &= ~0x01;

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

    bool done = (DMAIRQ & 0x01) != 0 || (RFIF & 0x10) != 0;

    RFST = 0x04;
    RFTXRXIF = 0;
    RFIF = 0;

    return (timeout > 0 && done);
}

/* ============================================================================
 * Sleep Timer + PM2
 * ============================================================================ */

/** @brief Start the periodic 0.5 s Sleep Timer Event0 (runs on 32 kHz RCOSC). */
static void sleep_timer_init(void) {
    uint8_t t0;
    uint16_t timeout;

    WORCTRL = 0x04 | ST_WOR_RES; // WOR_RESET + resolution

    // Wait for 2 sleep-timer edges after reset (SWRS033), bounded so it can never hang
    t0 = WORTIME0;
    timeout = 60000;
    while (WORTIME0 == t0 && --timeout);
    t0 = WORTIME0;
    timeout = 60000;
    while (WORTIME0 == t0 && --timeout);

    WOREVT1 = 0x00;
    WOREVT0 = ST_WOREVT;

    WORIRQ = 0x10; // EVENT0_MASK = 1, clear flag
    STIF = 0;
    STIE = 1;
}

/**
 * @brief Enter PM2 until the Sleep Timer tick or a PIR edge on P1_7.
 * @pre CPU runs on HS RCOSC (clock_rcosc()).
 */
static void enter_pm2(void) {
    uint8_t pir_level;
    uint8_t d;

    EA = 0;

    // Arm P1_7 for the edge AWAY from the current level (P1ICON = PICTL bit 1)
    pir_level = P1_7;
    if (pir_level) {
        PICTL |= 0x02;  // Falling edge: motion starts
    } else {
        PICTL &= ~0x02; // Rising edge: motion ends
    }
    P1IEN = 0x80;
    P1IFG = 0x00;
    P1IF = 0;
    IEN2 |= 0x10;       // P1IE

    // Race check: level changed while arming, or a tick is already pending
    if (P1_7 != pir_level || g_tick || g_pir_edge) {
        EA = 1;
        return;
    }

    // Give at least one 32 kHz period between wake-up and re-entering PM2
    for (d = 0; d < 200; d++) {
        __asm nop __endasm;
    }

    SLEEP = (SLEEP & ~0x03) | 0x02; // MODE = PM2
    __asm
        nop
        nop
        nop
    __endasm;

    EA = 1;
    if (SLEEP & 0x03) {   // An ISR clears MODE if it fired in the meantime
        PCON |= 0x01;     // Enter PM2
        __asm nop __endasm;
    }
}

/* ============================================================================
 * Packet build + transmit
 * ============================================================================ */

static void transmit(uint16_t seq, bool is_motion, bool is_tamper, bool is_por,
                     uint8_t p0_pu, uint8_t p0_pd, uint8_t p1_pu, uint8_t p2_packed) {
    uint8_t plain[16];
    uint16_t batt_mv;
    uint8_t flags = 0;

    clock_xosc();
    radio_init();
    aes_load_key(DEVICE_AES_KEY);

    batt_mv = read_battery_mv();

    for (uint8_t i = 0; i < 8; i++) {
        tx_buf[i] = (uint8_t)DEVICE_ID[i];
    }

    if (is_motion) flags |= 0x01;           // Bit 0: Motion
    if (is_tamper) flags |= 0x04;           // Bit 2: Tamper
    if (is_por)    flags |= 0x08;           // Bit 3: Power-on reset
    if (batt_mv < 2400) flags |= 0x10;      // Bit 4: Low battery

    plain[0]  = (uint8_t)(seq >> 8);
    plain[1]  = (uint8_t)(seq & 0xFF);
    plain[2]  = flags;
    plain[3]  = calculate_battery_pct(batt_mv);
    plain[4]  = (uint8_t)(batt_mv >> 8);
    plain[5]  = (uint8_t)(batt_mv & 0xFF);
    plain[6]  = (uint8_t)DEVICE_ID[0];
    plain[7]  = (uint8_t)DEVICE_ID[1];
    plain[8]  = (uint8_t)DEVICE_ID[2];
    plain[9]  = (uint8_t)DEVICE_ID[3];
    plain[10] = 'V';
    plain[11] = 'S';
    plain[12] = p0_pu;
    plain[13] = p0_pd;
    plain[14] = p1_pu;
    plain[15] = p2_packed;

    aes_encrypt_block(plain, &tx_buf[8]);

    g_tx_count++;
    if (!radio_send_dma()) {
        g_tx_fail++;
    }

    clock_rcosc();
}

/* ============================================================================
 * Main
 * ============================================================================ */

void main(void) {
    uint8_t rst_cause = (SLEEP >> 3) & 0x03;
    bool is_por_reset = (rst_cause == 0x00 || rst_cause == 0x01);

    uint8_t p0_pu, p0_pd, p1_pu, p1_pd, p2_packed;
    uint8_t base_p0_pu, base_p0_pd, base_p1_pu, base_p1_pd, base_p2;
    uint16_t seq = 0;
    uint16_t ticks = 0;
    bool last_motion, last_tamper;

    setup_gpio();
    clock_xosc();
    delay_ms(50);

    // Boot baseline = SW3 released (unit off the wall)
    read_gpio_scan(&p0_pu, &p0_pd, &p1_pu, &p1_pd, &p2_packed);
    base_p0_pu = p0_pu;
    base_p0_pd = p0_pd;
    base_p1_pu = p1_pu & 0x7F;
    base_p1_pd = p1_pd & 0x7F;
    base_p2    = p2_packed;

    last_motion = ((p1_pu & 0x80) == 0);
    last_tamper = true; // Released at boot = Tamper

    sleep_timer_init();
    EA = 1;

    // Boot packet (Seq #0)
    transmit(seq++, last_motion, last_tamper, is_por_reset, p0_pu, p0_pd, p1_pu, p2_packed);

    while (1) {
        bool motion, tamper, sw3_pressed, heartbeat;

        enter_pm2();

        // Woken by Sleep Timer tick and/or PIR edge
        if (g_tick) {
            g_tick = 0;
            ticks++;
        }
        g_pir_edge = 0;

        read_gpio_scan(&p0_pu, &p0_pd, &p1_pu, &p1_pd, &p2_packed);
        motion = ((p1_pu & 0x80) == 0);
        sw3_pressed = (p0_pu != base_p0_pu) || (p0_pd != base_p0_pd) ||
                      ((p1_pu & 0x7F) != base_p1_pu) || ((p1_pd & 0x7F) != base_p1_pd) ||
                      (p2_packed != base_p2);
        tamper = !sw3_pressed;

        // Debounce tamper changes with a second sample (~10 ms later on RCOSC)
        if (tamper != last_tamper) {
            delay_ms(5);
            read_gpio_scan(&p0_pu, &p0_pd, &p1_pu, &p1_pd, &p2_packed);
            sw3_pressed = (p0_pu != base_p0_pu) || (p0_pd != base_p0_pd) ||
                          ((p1_pu & 0x7F) != base_p1_pu) || ((p1_pd & 0x7F) != base_p1_pd) ||
                          (p2_packed != base_p2);
            tamper = !sw3_pressed;
            motion = ((p1_pu & 0x80) == 0);
        }

        heartbeat = (ticks >= HEARTBEAT_TICKS);

        if (motion != last_motion || tamper != last_tamper || heartbeat) {
            ticks = 0;
            last_motion = motion;
            last_tamper = tamper;
            transmit(seq, motion, tamper, (is_por_reset && seq < 3), p0_pu, p0_pd, p1_pu, p2_packed);
            seq++;
        }
    }
}
