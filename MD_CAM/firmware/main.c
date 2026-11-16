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
 * Pin map: see docs/Hardware_pinout_and_measurements.md (J6: DD=P2_1, DC=P2_2, RESET_N).
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
#define BIG_PACKET_LEN    56   // picture packet (type 0x82): 8 B ID + 3 AES blocks, 40 B of JPEG

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
static __xdata uint8_t tx_buf[BIG_PACKET_LEN];
static uint8_t s_tx_len = PACKET_LEN;   // bytes radio_send_dma sends (PKTLEN must match)

volatile __xdata uint8_t g_tx_count = 0;
volatile __xdata uint8_t g_tx_fail = 0;


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
/* Watchdog (WDCTL, 32 kHz clock, also runs in PM2): resets the chip if it is not fed for
 * ~1 s, e.g. if the CPU never wakes from PM2 again (seen once on 2026-11-16: no poll after
 * the first, only a power cycle helped). Started in cam_alarm_loop; fed in delay_ms, after
 * every sleep and per picture chunk. Feeding before it is started has no effect. */
#define WDT_FEED() do { WDCTL = 0xA8; WDCTL = 0x58; } while (0)

void delay_ms(uint16_t ms) {
    while (ms--) {
        if (WDCTL & 0x08)           // per ms: delay_ms(1100) takes ~3 s (runs ~2.75x slow)
            WDT_FEED();
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

// After an encrypt/decrypt ENCCS.RDY did not come up before ENCDO is read, so waiting for
// it with a 50000 timeout cost 44 ms per block (measured with Timer 1): a picture chunk
// took 52 ms instead of ~8 ms. Wait at most 120 short loop passes (~0.1 ms) instead, far
// more than one AES block needs; the key load still waits for RDY (it does come up there).
static void aes_wait(void) {
    uint8_t d;
    for (d = 0; d < 120 && !(ENCCS & 0x08); d++)
        ;
}

void aes_encrypt_block(const uint8_t *in, uint8_t *out) {
    ENCCS = 0x40 | 0x01; // MODE = ECB, CMD = Encrypt, ST = 1
    for (uint8_t i = 0; i < 16; i++) {
        ENCDI = in[i];
    }
    aes_wait();
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
 * UART1 (USART1 Alt. 1: P0_4=TX, P0_5=RX @ 57600 baud)
 * ============================================================================ */

/* ============================================================================
 * SPI0 Master (USART0 Alt. 2: P1_3=SCK, P1_4=MISO, P1_5=MOSI; P1_2=SS1, P1_1=SS2)
 * ============================================================================ */

/* ============================================================================
 * GPIO
 * ============================================================================ */

void setup_gpio(void) {
    // Port 0 & Port 1 base configuration
    P0SEL = 0x00;
    P1SEL = 0x00;
    P2SEL &= ~0x07;

    // CC1110 SPI pins kept tri-state
    P1DIR &= ~0x38; // P1_3..P1_5 = inputs
    P1INP |= 0x38;  // tri-state, no pull resistor

    // Regulator control pins as outputs, driven LOW (0 V):
    P0DIR |= 0x85;  // P0_0 (/RESET), P0_2 (1.2V core), P0_7 (aux/Q2) as outputs
    P0 &= ~0x85;    // all driven to 0.0 V
    P2DIR |= 0x01;  // P2_0 as output
    P2_0 = 0;

    // BMODE and DSP pins (P0_1, P0_3, P0_6) as INPUTS with PULL-UP (exactly as in the verified JEDEC test):
    P0DIR &= ~0x4A;
    P0INP &= ~0x4A;
    P2INP &= ~0x20;

    P1DIR |= 0x07;  // P1_0 (3.3V), P1_1 (bus switch), P1_2 (SS) = outputs
    P1_0 = 0;       // 3.3V Switched rail OFF
    P1_1 = 0;       // bus switch OFF (isolated while idle)
    P1_2 = 1;       // SS idle HIGH

    // Keep switch SW3 (P1_6) and the PIR (P1_7) as inputs with pull-UP:
    P1DIR &= ~0xC0;
    P1INP &= ~0xC0; // Enable pull
    P2INP &= ~0x40; // Port 1 pull-UP (the PIR on P1_7 needs it)
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
    dma_desc.len_l = s_tx_len;
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
    if (p0_pu != 0) {
        plain[12] = p0_pu;
        plain[13] = p0_pd;
        plain[14] = p1_pu;
        plain[15] = p2_packed;
    } else {
        plain[12] = P0;
        plain[13] = P1;
        plain[14] = P1;
        plain[15] = P2;
    }

    aes_encrypt_block(plain, &tx_buf[8]);

    g_tx_count++;
    if (!radio_send_dma()) {
        g_tx_fail++;
    }

    // Keep 27 MHz crystal running for high-speed SPI with Blackfin DSP
    // clock_rcosc();
}

/* ============================================================================
 * Main
 * ============================================================================ */



// Picture packet of 56 bytes (gateway packet type 0x82, cryptoProcessBigPacket): device ID,
// then 3 AES blocks chained as CBC with a zero IV (C0 = E(P0), C1 = E(P1 ^ C0), C2 = E(P2 ^ C1)),
// so a block cannot be swapped in from another packet without breaking 'VS'. Plain: seq,
// 0x82, chunk number, flags (0x80 first, 0x40 last, bits 0..5 length), 40 data bytes, 'V' 'S'.
// The gateway receives 58-byte frames and tells this from the 24-byte packets by the CRC.
static __xdata uint8_t s_big_plain[48];

static void transmit_big_chunk(uint8_t type, uint16_t seq, uint16_t chunk_idx, uint8_t chunk_len,
                               bool is_first, bool is_last, const uint8_t *data) {
    uint8_t i, b, flags = chunk_len & 0x3F;

    aes_load_key(DEVICE_AES_KEY);
    for (i = 0; i < 8; i++)
        tx_buf[i] = (uint8_t)DEVICE_ID[i];
    if (is_first) flags |= 0x80;
    if (is_last)  flags |= 0x40;
    s_big_plain[0] = (uint8_t)(seq >> 8);
    s_big_plain[1] = (uint8_t)(seq & 0xFF);
    s_big_plain[2] = type;          // 0x82 picture chunk, 0x83 resent chunk
    s_big_plain[3] = (uint8_t)(chunk_idx >> 8);
    s_big_plain[4] = (uint8_t)(chunk_idx & 0xFF);
    s_big_plain[5] = flags;
    for (i = 0; i < 40; i++)
        s_big_plain[6 + i] = (i < chunk_len) ? data[i] : 0x00;
    s_big_plain[46] = 'V';
    s_big_plain[47] = 'S';
    for (b = 0; b < 3; b++) {
        if (b) {
            for (i = 0; i < 16; i++)
                s_big_plain[16 * b + i] ^= tx_buf[8 + 16 * (b - 1) + i];
        }
        aes_encrypt_block(&s_big_plain[16 * b], &tx_buf[8 + 16 * b]);
    }
    PKTLEN = BIG_PACKET_LEN;
    s_tx_len = BIG_PACKET_LEN;
    radio_send_dma();
    PKTLEN = PACKET_LEN;
    s_tx_len = PACKET_LEN;
}


/* ============================================================================
 * Camera settings (cam_alarm_loop): the Blackfin (snap_link.c) is powered only for pictures
 * ============================================================================ */
#define BF_IMG_CMD 0xD3          // 0xD3 colour 320x240 as delta (only changed blocks), 0xD1 full colour, 0xD0 grayscale
#define BF_IMG_HIRES_ON_MOTION 1 // the 2nd picture of a motion event is 640x480 colour (0xD2/0xD6, ~18 KB, ~12 s)
#define BF_PIR_HOLD_S 30         // a motion event lasts this long after the last PIR activity
#define BF_TICKS_PER_S 36        // delay_ms(10) takes ~27.5 ms here (delay_ms runs ~2.75x slow)
#define BF_IMG_PAUSE_TICKS (5 * BF_TICKS_PER_S)   // pause between pictures during motion (~5 s)
#define BF_IMG_PIC_TICKS (7 * BF_TICKS_PER_S)     // the PIR hold time one picture counts for
#define BF_ARMED_AT_BOOT 1       // alarm state until the gateway has answered a poll (armed: fail safe)
#define BF_POLL_S 30             // poll the gateway (alarm state, picture requests) this often
#define BF_RX_WINDOW 40          // listen window after a poll, in delay_ms(1) steps (~2.75 ms each: ~110 ms)
#define BF_BOOT_MIN_MS 50        // after /RESET release P1_2 stays 1 this long (~140 ms) so the boot
                                 // selector sees PH0 low and starts snap_link, not the factory firmware
#define BF_BOOT_MAX_TRIES 200    // then ping (0xE5 -> 0xA5) every ~30 ms until snap_link answers,
                                 // at most ~6 s (it used to be a fixed wait of ~3 s)
#define BF_ARMED_IDLE_S 1800     // armed without motion: one picture this often (0 = never)
#define BF_SLEEP_IDLE 1          // 1: with the Blackfin off, wait in PM2 (0.5 s timer / PIR edge) instead of
                                 // busy-waiting: idle below 1 mA (PSU resolution) instead of ~3 mA. A first
                                 // try (2026-11-16) never woke from the sleep timer and needed a power
                                 // cycle; a later build ran fine (likely code placement), and the watchdog
                                 // (WDT_FEED) now resets the chip if it ever hangs like that again.
#define BF_AUTO_FLASH 1          // pictures while armed (motion and the idle one): snap_link takes a
                                 // grayscale flash picture instead when the room is dark (0xD5 for 0xD3,
                                 // 0xD6 for 0xD2). The flash board is infrared, so nobody sees it
#define BF_CMD_HOLD 0xE8         // downlink command: power the Blackfin and keep it on BF_HOLD_S
#define BF_HOLD_S 600            // (no picture); for JTAG work on the Blackfin
#define BF_KEEP_ON_S 15         // Blackfin stays powered this long after a picture (~0.14 A), so its
                                 // SDRAM keeps the delta reference and a 0xD3 request within that
                                 // time is sent as a delta; 0 = power off right after each picture
                                 // (120 s until 2026-11-16: it cost ~17 As per picture, ~20x a cold picture
                                 // of ~6 s now; to be decided again after measuring with the Otii)

// Blackfin SPI1 as seen from the CC1110 (measured 2026-11-11, only with P1_2 = 1):
// P1_4 -> PH1 SPI1_SCK, P1_5 -> PH3 SPI1_MOSI, PH2 SPI1_MISO -> P1_3, and P1_2 = 1
// pulls PH0 SPI1_SSEL low. Mode 0 (CPOL 0, CPHA 0), MSB first, slow bit-bang.
static uint8_t bf_spi1_byte(uint8_t out) {
    uint8_t i, in = 0, d;
    for (i = 0; i < 8; i++) {
        P1_5 = (out & 0x80) ? 1 : 0;
        out <<= 1;
        for (d = 0; d < 20; d++) ;
        P1_4 = 1;
        in = (uint8_t)((in << 1) | P1_3);
        for (d = 0; d < 20; d++) ;
        P1_4 = 0;
    }
    return in;
}

// One framed byte to the Blackfin software SPI slave (snap_link.c). The pause after the
// byte gives the Blackfin time to prepare its next answer before the next select.
static uint8_t bf_link_xfer(uint8_t tx) {
    uint8_t rx, d;
    P1_2 = 1;
    rx = bf_spi1_byte(tx);
    P1_2 = 0;
    for (d = 0; d < 40; d++) ;
    return rx;
}

#define BF_IMG_CHUNK 40          // JPEG bytes per RF picture packet (type 0x82, 56 bytes)
static uint8_t s_img_blk[BF_IMG_CHUNK];

#define BF_IMG_TIMING 0          // test: after a picture, RF B7 with the mean time per chunk of the
                                 // SPI read, the RF send and the whole loop (units of 0.25 ms, Timer 1).
                                 // 2026-11-16 after the AES fix: 0.75 / 8 / 9 ms (before: 0.5 / 52 / 53)
                                 // also RF B6 after power-on: pings until snap_link answered (37, ~1.2 s)
#if BF_IMG_TIMING
static uint16_t t1_now(void) {
    uint8_t l = T1CNTL;          // reading T1CNTL latches T1CNTH
    return ((uint16_t)T1CNTH << 8) | l;
}
#endif

// One picture from the Blackfin (snap_link.c): cmd 0xD0..0xD6 grabs and encodes it (a few
// seconds; the Blackfin does not answer meanwhile), so poll 0xB0/0xA0 until the answer is
// 0x5A. Then read the length (2 bytes, little endian) and the data, 40 bytes per 56-byte RF
// packet (type 0x82, chunk number, length, first/last flags; transmit_big_chunk); the gateway
// places the chunks by number. RF on a failure: B8, tries, status. Returns true if the
// picture was sent. (Before 2026-11-16: 8 bytes per 24-byte packet, ~9 ms each.)
static uint16_t s_pic_len;      // length of the last picture, still in snap_link's send buffer

static bool bf_send_picture(uint8_t cmd, uint16_t *p_seq) {
    uint8_t st = 0, tries, i, n;
    uint16_t len, off, idx;
    bf_link_xfer(cmd);
    // ~0.83 s per try: 60 tries ~50 s. A cold 640x480 (0xD2) needed ~13 s, but snap_link's
    // AEC wait after power-up can add up to ~15 s more in a scene that keeps changing
    for (tries = 0; tries < 60; tries++) {
        delay_ms(300);
        bf_link_xfer(0xB0);
        st = bf_link_xfer(0xA0);      // answer to 0xB0
        if (st == 0x5A)
            break;
    }
    if (st != 0x5A) {
        transmit((*p_seq)++, false, false, false, 0xB8, tries, st, 0);
        return false;
    }
    len = bf_link_xfer(0x00);
    len |= (uint16_t)bf_link_xfer(0x00) << 8;
    if (len < 4 || len > 65000u) {
        transmit((*p_seq)++, false, false, false, 0xB8, 0xFF, (uint8_t)len, (uint8_t)(len >> 8));
        return false;
    }
    s_pic_len = len;
#if BF_IMG_TIMING
    {
    uint32_t t_spi = 0, t_tx = 0, t_all = 0;
    uint16_t a, b, c, z = 0;
    T1CTL = 0x0D;                // free running, tick 128/27 MHz = 4.74 us
    c = t1_now();
#endif
    for (off = 0, idx = 0; off < len; off += n, idx++) {
        n = (len - off) > BF_IMG_CHUNK ? BF_IMG_CHUNK : (uint8_t)(len - off);
#if BF_IMG_TIMING
        a = t1_now();
#endif
        for (i = 0; i < n; i++)
            s_img_blk[i] = bf_link_xfer(0x00);
#if BF_IMG_TIMING
        b = t1_now();
        t_spi += (uint16_t)(b - a);
#endif
        transmit_big_chunk(0x82, (*p_seq)++, idx, n, idx == 0, off + n >= len, s_img_blk);
        WDT_FEED();
#if BF_IMG_TIMING
        a = t1_now();
        t_tx += (uint16_t)(a - b);
        t_all += (uint16_t)(a - c);
        c = a;
        z++;
#endif
    }
#if BF_IMG_TIMING
    // 0.25 ms = 52.7 ticks
    transmit((*p_seq)++, false, false, false, 0xB7, (uint8_t)(t_spi / z / 53), (uint8_t)(t_tx / z / 53),
             (uint8_t)(t_all / z / 53));
    }
#endif
    return true;
}

/* ============================================================================
 * Alarm-driven camera
 *
 * Disarmed: the Blackfin stays unpowered; the PIR only sends motion ON/OFF (B9).
 * Armed: on motion the Blackfin is powered, pictures are taken every ~5 s (the 2nd one
 * 640x480) until BF_PIR_HOLD_S after the last PIR activity, and it is switched off again when the motion event ends. A picture requested from Home
 * Assistant is taken in either state.
 *
 * Every BF_POLL_S the camera sends a poll (telemetry packet, plain[12] = 0xBA, [13..14] =
 * challenge, [15] = state: bit 0 armed, bit 1 Blackfin powered, bit 2 a command arrived
 * and the gateway has not seen this yet) and listens BF_RX_WINDOW for the gateway's answer
 * (868Receiver/src/downlink.h): 8 bytes device ID + AES(device key, 'D' 'L' seq challenge
 * armed cmd id[0..3] 0 0 'V' 'S'). The challenge is new for every poll (next_challenge)
 * and hidden inside the encrypted poll, so a recorded answer fits a later poll only by
 * chance (1 in 65536 for a matching sequence number).
 * ============================================================================ */
static __xdata uint8_t rx_buf[PACKET_LEN + 2];
static __xdata uint8_t s_dl_plain[16];
static __xdata uint16_t s_boot_noise;  // radio noise taken at boot
static __xdata uint32_t s_poll_ctr;
static __xdata uint8_t s_chal_in[16], s_chal_out[16];
static __xdata uint16_t s_chal;      // challenge of the last poll
static __xdata uint8_t s_dl_cmd;     // picture requested by the gateway, 0 = none
static bool s_armed = BF_ARMED_AT_BOOT;
static bool s_cmd_ack;               // report in polls that a command arrived, until answered

// Fresh 16-bit challenge for every poll: AES(device key, boot noise | poll counter). It is
// encrypted inside the poll, so a listener cannot tell which recorded answer would fit.
static uint16_t next_challenge(void) {
    uint8_t i;
    s_poll_ctr++;
    for (i = 0; i < 16; i++)
        s_chal_in[i] = 0;
    s_chal_in[0] = (uint8_t)(s_boot_noise >> 8);
    s_chal_in[1] = (uint8_t)s_boot_noise;
    s_chal_in[2] = (uint8_t)(s_poll_ctr >> 24);
    s_chal_in[3] = (uint8_t)(s_poll_ctr >> 16);
    s_chal_in[4] = (uint8_t)(s_poll_ctr >> 8);
    s_chal_in[5] = (uint8_t)s_poll_ctr;
    s_chal_in[6] = 'C';
    aes_load_key(DEVICE_AES_KEY);
    aes_encrypt_block(s_chal_in, s_chal_out);
    return ((uint16_t)s_chal_out[0] << 8) | s_chal_out[1];
}

static void aes_decrypt_block(const uint8_t *in, uint8_t *out) {
    uint8_t i;
    ENCCS = 0x40 | 0x02 | 0x01;      // MODE = ECB, CMD = Decrypt, ST = 1
    for (i = 0; i < 16; i++)
        ENCDI = in[i];
    aes_wait();
    for (i = 0; i < 16; i++)
        out[i] = ENCDO;
}

// Receive one packet (24 bytes + RSSI + LQI/CRC) within 'window' delay_ms(1) steps.
// Returns true for a packet with a good CRC in rx_buf.
static bool radio_receive_dma(uint16_t window) {
    uint16_t t;
    bool done;

    RFST = 0x04; // SIDLE
    t = 5000;
    while ((MARCSTATE & 0x1F) != 0x01 && --t);

    dma_desc.src_h = 0xDF;
    dma_desc.src_l = 0xD9;           // X_RFD
    dma_desc.dst_h = (uint8_t)(((uint16_t)rx_buf) >> 8);
    dma_desc.dst_l = (uint8_t)(((uint16_t)rx_buf) & 0xFF);
    dma_desc.len_h = 0x00;
    dma_desc.len_l = PACKET_LEN + 2;
    dma_desc.cfg0  = 19;             // DMA_TRIG_RADIO, single
    dma_desc.cfg1  = 0x1A;           // SRCINC 0, DESTINC 1, IRQMASK, high priority

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

    RFST = 0x02; // SRX (MCSM1: back to IDLE after the packet)
    for (t = 0; t < window && !(RFIF & 0x10); t++)
        delay_ms(1);
    // a packet whose sync word came late in the window: let it finish (~7 ms on air)
    for (t = 0; t < 10 && !(RFIF & 0x10) && (PKTSTATUS & 0x08); t++)
        delay_ms(1);
    done = (RFIF & 0x10) != 0;

    RFST = 0x04;
    if (done) {
        t = 1000;
        while (!(DMAIRQ & 0x01) && --t);
        done = t != 0;
    }
    DMAARM = 0x81;                   // abort channel 0 if still armed
    RFTXRXIF = 0;
    RFIF = 0;
    return done && (rx_buf[PACKET_LEN + 1] & 0x80) != 0;
}

// 16 bits from the radio noise floor (RSSI LSBs), taken once at boot.
static uint16_t radio_noise_nonce(void) {
    uint16_t n = 0;
    uint8_t i;
    clock_xosc();
    radio_init();
    RFST = 0x02; // SRX
    delay_ms(2);
    for (i = 0; i < 64; i++) {
        n = (uint16_t)((n << 3) | (n >> 13)) ^ RSSI;
        delay_ms(1);
    }
    RFST = 0x04;
    return n;
}

// Poll the gateway and take its answer. Returns true for a valid answer.
static uint16_t s_rs_idx;       // resend request from the last poll answer (s_rs_count 0 = none)
static uint8_t s_rs_count;

static bool cam_poll(uint16_t *p_seq, bool motion, bool bf_on) {
    uint16_t seq = (*p_seq)++;
    uint8_t i, state = 0;

    if (s_armed) state |= 0x01;
    if (bf_on) state |= 0x02;
    if (s_cmd_ack) state |= 0x04;
    s_chal = next_challenge();
    transmit(seq, motion, false, false, 0xBA, (uint8_t)(s_chal >> 8), (uint8_t)s_chal, state);
    if (!radio_receive_dma(BF_RX_WINDOW))
        return false;
    for (i = 0; i < 8; i++)
        if (rx_buf[i] != (uint8_t)DEVICE_ID[i])
            return false;
    aes_load_key(DEVICE_AES_KEY);
    aes_decrypt_block(&rx_buf[8], s_dl_plain);
    if (s_dl_plain[0] != 'D' || s_dl_plain[1] != 'L' || s_dl_plain[14] != 'V' || s_dl_plain[15] != 'S')
        return false;
    if (s_dl_plain[2] != (uint8_t)(seq >> 8) || s_dl_plain[3] != (uint8_t)seq)
        return false;
    if (s_dl_plain[4] != (uint8_t)(s_chal >> 8) || s_dl_plain[5] != (uint8_t)s_chal)
        return false;
    for (i = 0; i < 4; i++)
        if (s_dl_plain[8 + i] != (uint8_t)DEVICE_ID[i])
            return false;
    // the gateway has seen this poll (and with it any acknowledgement)
    s_cmd_ack = false;
    if (s_dl_plain[6] <= 1)
        s_armed = s_dl_plain[6] != 0;
    if ((s_dl_plain[7] >= 0xD1 && s_dl_plain[7] <= 0xD6) ||   // 0xD4 flash, 0xD5/0xD6 flash in the dark
        s_dl_plain[7] == BF_CMD_HOLD) {
        s_dl_cmd = s_dl_plain[7];
        s_cmd_ack = true;
    } else if (s_dl_plain[7] >= 0xF0) {
        // resend request: count - 1 in the low nibble, first chunk in [12..13]
        s_rs_idx = ((uint16_t)s_dl_plain[12] << 8) | s_dl_plain[13];
        s_rs_count = (s_dl_plain[7] & 0x0F) + 1;
    }
    return true;
}

// Resend chunks the gateway missed (downlink 0xF0 | count - 1, chunk number in [12..13]): snap_link
// seeks to the chunk (0xE1 hi lo) in the picture it still holds, and the chunks go out as type
// 0x83 so the gateway patches them into that picture instead of starting a new one.
static void bf_resend(uint16_t idx, uint8_t count, uint16_t *p_seq) {
    uint16_t off = idx * BF_IMG_CHUNK;
    uint8_t i, n;
    if (!s_pic_len || off >= s_pic_len)
        return;
    bf_link_xfer(0xE1);
    bf_link_xfer((uint8_t)(idx >> 8));
    bf_link_xfer((uint8_t)idx);
    for (; count && off < s_pic_len; count--, idx++, off += n) {
        n = (s_pic_len - off) > BF_IMG_CHUNK ? BF_IMG_CHUNK : (uint8_t)(s_pic_len - off);
        for (i = 0; i < n; i++)
            s_img_blk[i] = bf_link_xfer(0x00);
        transmit_big_chunk(0x83, (*p_seq)++, idx, n, idx == 0, off + n >= s_pic_len, s_img_blk);
        WDT_FEED();
    }
}

// Power the Blackfin as bf_jtag_hold() does, without its long JTAG pauses: P1_2 = 1 at
// reset release makes the boot selector start snap_link (0x8000). Then the link pins.
static uint8_t s_boot_tries;    // pings until snap_link answered (BF_IMG_TIMING reports it)

static void bf_power_on(void) {
    P0DIR |= 0x85;   // P0_0, P0_2 (1.2V), P0_7 (aux) = outputs
    P0_0 = 0;
    P0DIR &= ~0x4A;  // P0_1, P0_3, P0_6 (Blackfin BMODE/DSP) back to inputs with pull-up,
    P0INP &= ~0x4A;  // as setup_gpio() has them for the boot (bf_power_off drives them low)
    P1SEL &= ~0x3E;
    P1DIR |= 0x07;
    P1_1 = 0;
    P1_2 = 1;
    P1DIR &= ~0x38;  // SCK, MISO, MOSI tri-state during boot
    P1INP |= 0x38;

    P1_0 = 1;        // Q7 holds /RESET low
    delay_ms(10);
    P0_2 = 1;
    delay_ms(10);
    P0_7 = 1;
    delay_ms(50);
    P0_0 = 1;
    delay_ms(20);
    P1_0 = 0;        // release /RESET
    delay_ms(BF_BOOT_MIN_MS);

    P1_2 = 0;
    P1DIR |= 0x34;   // P1_2 (select), P1_4 (SCK), P1_5 (MOSI)
    P1DIR &= ~0x08;  // P1_3 (MISO) input
    P1_4 = 0;
    // snap_link answers 0xE5 with 0xA5 in the next transfer once it is in its command loop
    // (boot ROM, selector, PLL, SDRAM test, sensor setup); two answers in a row, so a floating
    // MISO cannot pass by chance
    for (s_boot_tries = 0; s_boot_tries < BF_BOOT_MAX_TRIES; s_boot_tries++) {
        bf_link_xfer(0xE5);
        if (bf_link_xfer(0xE5) == 0xA5 && bf_link_xfer(0x00) == 0xA5)
            break;
        delay_ms(10);
    }
}

// Back to the idle state of setup_gpio(): /RESET low (P0_0), aux and 1.2 V rails off in the
// reverse order of bf_power_on(), Q7 off (P1_0), bus tri-state. P0_1, P0_3 and P0_6 are
// then driven low: as pull-up inputs they keep Q2 on the PIR board conducting, so the red
// LD1 lit and the board drew ~20 mA instead of ~3 mA (memory PIR_INDICATOR_LED).
static void bf_power_off(void) {
    s_pic_len = 0;   // the picture in snap_link's buffer is gone
    P0_0 = 0;
    P0_7 = 0;
    P0_2 = 0;
    P1_0 = 0;
    P1_1 = 0;
    P1_2 = 1;
    P1DIR &= ~0x38;
    P1INP |= 0x38;
    P0 &= ~0x4A;
    P0DIR |= 0x4A;
}

#if BF_SLEEP_IDLE
// With the Blackfin off, sleep in PM2 until the next 0.5 s sleep-timer tick or a PIR edge on
// P1_7, instead of busy-waiting on the crystal (board ~22 mA awake, ~3 mA in PM2 as measured
// with the earlier low-power build). Returns the loop ticks that passed: a whole timer period
// after a tick, 1 after a PIR edge (exact time unknown; the loop reads P1_7 right away).
static uint16_t cam_sleep(void) {
    g_tick = 0;
    g_pir_edge = 0;
    clock_rcosc();
    enter_pm2();
    WDT_FEED();
    clock_xosc();
    return g_tick ? (uint16_t)(BF_TICKS_PER_S * ST_WOREVT / 32) : 1;
}
#endif

static void cam_alarm_loop(uint16_t *p_seq) {
    uint16_t poll = BF_POLL_S * BF_TICKS_PER_S;   // first poll right away
    uint16_t hold = 0, t, keep = 0;
    uint32_t idle = 0;
    uint8_t ev_pic = 0, cmd;
    bool active = false, bf_on = false, was_armed = s_armed;

    bf_power_off();
    s_boot_noise = radio_noise_nonce();
#if BF_SLEEP_IDLE
    sleep_timer_init();
#endif
    WDCTL = 0x08;                   // watchdog on, ~1 s (32768 periods of 32 kHz); cannot be stopped
    for (;;) {
        if (P1_7 == 0) {
            if (!active) {
                active = true;
                ev_pic = 0;
                // poll first (it already carries the motion flag) for a fresh alarm state,
                // so the gateway is not busy with the B9 packet while the camera listens
                poll = 0;
                cam_poll(p_seq, true, bf_on);
                transmit((*p_seq)++, true, false, false, 0xB9, 1, 0, 0);
            }
            hold = BF_PIR_HOLD_S * BF_TICKS_PER_S;
        }
        if (poll >= BF_POLL_S * BF_TICKS_PER_S) {
            poll = 0;
            cam_poll(p_seq, active, bf_on);
        }
        if (s_rs_count) {
            if (bf_on) {
                bf_resend(s_rs_idx, s_rs_count, p_seq);
                keep = BF_KEEP_ON_S * BF_TICKS_PER_S;
                poll = BF_POLL_S * BF_TICKS_PER_S;   // poll again right away for the next request
            }
            s_rs_count = 0;
        }
        if (s_armed && !was_armed)
            idle = 0;               // the armed idle picture counts from arming
        was_armed = s_armed;

        cmd = 0;
        if (s_dl_cmd) {
            cmd = s_dl_cmd;
            s_dl_cmd = 0;
        } else if (s_armed && active) {
            cmd = BF_IMG_CMD;
#if BF_IMG_HIRES_ON_MOTION
            if (ev_pic == 1)
                cmd = 0xD2;
#endif
#if BF_AUTO_FLASH
            if (cmd == 0xD3)
                cmd = 0xD5;
            else if (cmd == 0xD2)
                cmd = 0xD6;
#endif
            if (ev_pic < 2)
                ev_pic++;
        } else if (s_armed && BF_ARMED_IDLE_S && idle >= (uint32_t)BF_ARMED_IDLE_S * BF_TICKS_PER_S) {
            cmd = BF_IMG_CMD;
#if BF_AUTO_FLASH
            if (cmd == 0xD3)
                cmd = 0xD5;
#endif
        }

        if (cmd == BF_CMD_HOLD) {
            // service (MQTT "HOLD"): Blackfin on without a picture, e.g. for flashing it over
            // JTAG (~90 s), which BF_KEEP_ON_S alone does not cover
            if (!bf_on) {
                bf_power_on();
                bf_on = true;
            }
            keep = BF_HOLD_S * BF_TICKS_PER_S;
        } else if (cmd) {
            if (!bf_on) {
                bf_power_on();
#if BF_IMG_TIMING
                transmit((*p_seq)++, false, false, false, 0xB6, s_boot_tries, 0, 0);
#endif
                bf_on = true;
            }
            // keep it powered only after a good picture: a hung Blackfin gets power-cycled
            keep = bf_send_picture(cmd, p_seq) ? BF_KEEP_ON_S * BF_TICKS_PER_S : 0;
            idle = 0;
            // a picture takes 20..80 s over RF but counts as BF_IMG_PIC_TICKS, so during
            // motion the poll came only every ~3rd picture (seen: 172 s without one, so a
            // disarm waited that long): poll after every picture instead
            poll = BF_POLL_S * BF_TICKS_PER_S;
            hold = hold > BF_IMG_PIC_TICKS ? hold - BF_IMG_PIC_TICKS : 0;
            if (active) {
                for (t = 0; t < BF_IMG_PAUSE_TICKS; t++) {
                    delay_ms(10);
                    poll++;
                    if (P1_7 == 0)
                        hold = BF_PIR_HOLD_S * BF_TICKS_PER_S;
                    else if (hold)
                        hold--;
                }
            }
        } else {
            uint16_t n = 1;
#if BF_SLEEP_IDLE
            if (!bf_on)
                n = cam_sleep();
            else
#endif
                delay_ms(10);
            poll += n;
            idle += n;
            keep = keep > n ? keep - n : 0;
            if (P1_7 != 0)
                hold = hold > n ? hold - n : 0;
        }

        if (active && !hold && P1_7 != 0) {
            active = false;
            transmit((*p_seq)++, false, false, false, 0xB9, 0, 0, 0);
        }
        if (bf_on && !s_dl_cmd && !keep && (!active || !s_armed)) {
            bf_power_off();
            bf_on = false;
        }
    }
}

void main(void) {
    uint16_t seq = 25000;

    setup_gpio();
    clock_xosc();

    radio_init();
    delay_ms(50);

    // Initial announce packet
    transmit(seq++, false, false, true, 0, 0, 0, 0);
    delay_ms(100);

    // seq continues above the announce (25000, whose power-on flag resets the gateway's
    // replay filter), so the first poll is accepted before any picture packet is sent
    cam_alarm_loop(&seq);
}
