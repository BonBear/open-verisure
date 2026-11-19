/**
 * @file main.c
 * @brief Verisure Motion Detector Camera (GW MD CAM2 / SECUR-PCAM-BLACKFIN) - CC1110F32 Firmware
 *
 * 868.950 MHz AES-128 telemetry with PM2 Deep Sleep:
 *  - PIR Motion (P1_7, active LOW) wakes the CPU instantly via Port 1 interrupt.
 *  - Sleep Timer wakes the CPU every 0.5 s to sample the rear tamper switch SW3 on P1_6
 *    (tamper_read) and to count down the poll interval.
 *  - The 27 MHz XOSC runs whenever the CPU is awake: cam_sleep() stops it only for PM2 (via the
 *    HS RCOSC) and starts it again at every wake-up, i.e. at each 0.5 s tick. The radio is on
 *    only to send and for the listen window after a poll.
 *
 * Tamper: SW3 closes P1_6 to VBAT when the camera is pressed against the wall (secure);
 * released = cover off the wall = tamper (flag bit 2, event 0xB5 on every change).
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

/** @brief Sleep Timer tick 0.5 s. The 32 kHz RC oscillator is calibrated to f_xosc / 750, which
 *  is 36.0 kHz with this board's 27 MHz crystal (not 32 kHz): with WOR_RES=2 and EVENT0=16 a tick
 *  was 0.455 s and the camera's 30 s poll came every 27.3 s (2026-11-18). WOR_RES=1 (32 periods
 *  of 27.8 us) x 563 = 0.5004 s. */
#define ST_WOR_RES        0x01
#define ST_WOREVT         563

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

#include "../../cc1110_common/aes.c"

/* ============================================================================
 * Battery (VDD/3)
 * ============================================================================ */

/* VDD/3 against the internal 1.25 V reference. The first conversion is thrown away. */
uint16_t read_battery_mv(void) {
    uint8_t n;
    uint16_t timeout;
    for (n = 0; n < 2; n++) {
        ADCCON3 = 0x3F; // Internal 1.25V ref, 12-bit, VDD/3
        timeout = 5000;
        while (!(ADCCON1 & 0x80) && --timeout);
        if (n == 0)
            (void)ADCL, (void)ADCH;   // reading ADCH clears EOC for the next conversion
    }

    uint16_t adc = ((uint16_t)ADCL >> 4) | ((uint16_t)ADCH << 4);
    return (uint16_t)(((uint32_t)adc * 3750UL) / 2047UL);
}

/* After PM2 the reading is ~110 mV low for several ms (2026-11-17, Otii at 3.0 V: 2861 mV
 * ~3 ms after waking, 2969 mV after ~11 ms more, 2976 mV at boot), presumably while the
 * reference settles; two conversions in a row did not help. So a reading right after PM2
 * waits ~11 ms first (~1 uA on average at one poll per 2 minutes). */
uint16_t read_battery_idle_mv(void) {
    delay_ms(4);   // delay_ms runs ~2.75x slow: ~11 ms
    return read_battery_mv();
}

/* The camera runs on CR123A lithium cells (3.0 V nominal), whose idle voltage stays flat until
 * near the end, while a worn cell sags under load. The Blackfin needs at least ~2.4 V at VBAT
 * (Otii 2026-11-17: pictures work from 3.0 V down to 2.4 V; it draws 170-190 mA with peaks of
 * 0.3-0.54 A), so the voltage measured while the Blackfin is powered is the better indicator.
 * s_bat_load_mv is the lowest VBAT seen while it was powered last time (0 = no picture since
 * boot). The percentage is the lower of the idle and the load estimate, both linear: an
 * estimate, not a coulomb count. */
#define BAT_IDLE_EMPTY_MV 2600   // idle: 0 %
#define BAT_IDLE_FULL_MV  3000   // idle: 100 %
#define BAT_LOAD_EMPTY_MV 2450   // Blackfin powered: 0 % (pictures stop at ~2.4 V)
#define BAT_LOAD_FULL_MV  2850   // Blackfin powered: 100 %
#define BAT_LOW_IDLE_MV   2700   // low-battery flag below this idle voltage
#define BAT_LOW_LOAD_MV   2600   // or below this voltage under load (~0.2 V above the limit)

static __xdata uint16_t s_bat_load_mv;   // lowest VBAT while the Blackfin was last powered
static __xdata uint16_t s_bat_load_min;  // lowest VBAT of the current power-on
static __xdata uint16_t s_bat_idle_mv;   // last VBAT with the Blackfin off (0 = none yet)
static bool s_bf_powered;                // set by bf_power_on/off: transmit() reads under load

static uint8_t bat_scale(uint16_t mv, uint16_t empty, uint16_t full) {
    if (mv >= full) return 100;
    if (mv <= empty) return 0;
    return (uint8_t)(((uint32_t)(mv - empty) * 100UL) / (full - empty));
}

uint8_t calculate_battery_pct(uint16_t mv) {
    uint8_t pct = bat_scale(mv, BAT_IDLE_EMPTY_MV, BAT_IDLE_FULL_MV);
    if (s_bat_load_mv) {
        uint8_t load = bat_scale(s_bat_load_mv, BAT_LOAD_EMPTY_MV, BAT_LOAD_FULL_MV);
        if (load < pct) pct = load;
    }
    return pct;
}

static bool battery_low(uint16_t mv) {
    return mv < BAT_LOW_IDLE_MV || (s_bat_load_mv && s_bat_load_mv < BAT_LOW_LOAD_MV);
}

/** @brief Load voltage for the poll state bits 7:3: 0 = unknown, else 2275 + 25 * code mV
 *  (1 = 2300 mV or less, 31 = 3050 mV or more). */
static uint8_t bat_load_code(void) {
    uint16_t c;
    if (!s_bat_load_mv) return 0;
    if (s_bat_load_mv <= 2300) return 1;
    c = (s_bat_load_mv - 2275) / 25;
    return c > 31 ? 31 : (uint8_t)c;
}

/* ============================================================================
 * Clock switching: 27 MHz XOSC (TX) <-> 13.5 MHz HS RCOSC (required before PM2)
 * ============================================================================ */

#include "../../cc1110_common/clock.c"


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
    P1_2 = 0;       // link enable off: high costs ~30 uA in PM2 with the Blackfin off (Otii 2026-11-16)

    // Keep switch SW3 (P1_6) and the PIR (P1_7) as inputs with pull-UP:
    P1DIR &= ~0xC0;
    P1INP &= ~0xC0; // Enable pull
    P2INP &= ~0x40; // Port 1 pull-UP (the PIR on P1_7 needs it)
}

/**
 * @brief Read the tamper switch SW3. @return true = tamper (switch released, cover off the wall).
 *
 * SW3 closes P1_6 to VBAT, so with the port's pull-up (which the PIR on P1_7 needs) the pin
 * reads 1 either way. For the read Port 1 pulls down for ~45 us: P1_6 then reads 0 with the
 * switch open. P1_7 is tri-stated meanwhile, so it keeps its level and no PIR edge comes from
 * the pull-down (a pull-down left on gives false PIR alarms, see .agents/memory.md). The pull-up
 * is idle-safe: no current flows through SW3 either way.
 */
static bool tamper_read(void) {
    uint8_t d;
    bool open;

    P1INP |= 0x80;          // P1_7 tri-state
    P2INP |= 0x40;          // Port 1 pull-down (P1_6 still has its pull enabled)
    for (d = 0; d < 120; d++) {   // ~45 us, far longer than the pin's RC with ~20 kOhm
        __asm nop __endasm;
    }
    open = P1_6 == 0;
    P2INP &= ~0x40;         // pull-up again
    P1INP &= ~0x80;         // P1_7 pulled up again
    return open;
}

static bool s_tamper;       // reported in flags bit 2
#define TAMPER_MIN_GAP (2 * BF_TICKS_PER_S)     // ~2 s between two 0xB5 packets at first,
#define TAMPER_MAX_GAP (300U * BF_TICKS_PER_S)  // doubling per change up to ~5 min,
#define TAMPER_CALM    (600U * BF_TICKS_PER_S)  // back to the start after ~10 min without a change
#define TAMPER_PIC_GAP (600U * BF_TICKS_PER_S)  // at most one tamper picture per ~10 min

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

    MDMCFG4 = 0x8A; // 38.418 kBaud at 27 MHz, 2-FSK
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

    // With the Blackfin on, measure VBAT while the PA sends: the radio's TX current on top of
    // the Blackfin's is the deepest sag a picture causes. The CPU only waits here anyway.
    if (s_bf_powered) {
        uint16_t mv;
        timeout = 5000;
        while ((MARCSTATE & 0x1F) != 0x13 && --timeout);   // TX
        mv = read_battery_mv();
        if (timeout && mv < s_bat_load_min)
            s_bat_load_min = mv;
    }

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

#include "../../cc1110_common/sleep_timer.c"

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
    for (d = 200; d != 0; d--) {    // counting down: SDCC loop reversal warns (158) on an up-count >= 128
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
 * Persistent packet counter (replay protection across resets)
 * ============================================================================
 * As in the door sensor (VerisureDoorSensor/main.c, same log format): every 24-byte packet
 * carries a 32-bit counter, its low 16 bits in plain[0..1] and its high 16 bits in plain[14..15]
 * (flag 0x20). The upper 24 bits, the "block", are logged in the last two flash pages before
 * they are used: a new block at every boot and after every 256 packets. So the counter never
 * goes back, not even after a reset, the gateway drops every packet that is not newer than the
 * last one, and its poll answer can echo the counter instead of a challenge (a recorded answer
 * never fits a later poll). Before this the camera restarted its 16-bit seq at every reset and
 * the gateway had to accept its power-on packet, which re-opened all recorded packets.
 * The log: 4-byte entries, appended; 0xFFFFFFFF = empty. An entry is [b0, check, b1, b2] with the
 * 24-bit block b2:b1:b0 and check = b0 ^ b1 ^ b2 ^ 0xA5. A write cut short leaves b2 = 0xFF and
 * blocks never reach 0xFF0000, so such an entry, or one with a wrong check byte, is skipped.
 * Every entry is read back. When the page of the newest entry is full, the other page is erased
 * and used. Reflashing erases the log: build_and_flash_md_cam.py builds the firmware with
 * CTR_SEED = the block above the gateway's last accepted counter (GET /api/replay).
 * Picture chunks (56-byte packets) do not use up counter values: each picture starts with a
 * 24-byte packet of type 0xB2, and all its chunks (also resent ones) carry that packet's counter
 * (see transmit_big_chunk), which binds them to it.
 */
#define CTR_PAGE0        30                      // pages 30 and 31: 0x7800-0x7FFF (--code-size 0x7800)
#define CTR_ENTRIES      256                     // 1 KB page / 4 bytes
#define CTR_BLOCK_MAX    0xFEFFFFUL              // b2 = 0xFF marks a cut write
#ifndef CTR_SEED
#define CTR_SEED         1UL                     // first block when the log is empty
#endif
#define CTR_PAGE_ADDR(p) ((uint16_t)(p) << 10)
#define X_FWDATA_H       0xDF                    // FWDATA in XDATA space (DMA destination)
#define X_FWDATA_L       0xAF
#define DMA_TRIG_FLASH   18

static __xdata uint8_t ctr_buf[4];
static __xdata uint8_t ctr_page;     // page the next entry goes to
static __xdata uint16_t ctr_next;    // next free entry in ctr_page (CTR_ENTRIES = full)
static __xdata uint32_t s_ctr;       // counter of the next 24-byte packet
static __xdata uint32_t s_ctr_first; // first counter of this boot (its block is already logged)
static __xdata uint32_t s_pic_tag;   // counter of the start packet (0xB2) of the last picture
static __xdata uint8_t s_rst_cause;  // SLEEP.RST at boot, sent in flags bits 7:6

#include "../../cc1110_common/ctr_log.c"

/* A block whose log entry failed is retried before every packet in it: packets in an unlogged
 * block would be reused after the next reset (the gateway drops them, and the device would be
 * silent until its counter passes them again, ~10 days for a door sensor). 0 = none (block 0 is
 * never used). The boot also skips one block (see ctr_boot), which covers one unlogged block. */
static __xdata uint32_t ctr_pending;

/** @brief Find the newest valid block and log the next one, which this boot uses (on the
 *  crystal). Sets s_ctr to the first counter value of this boot. */
static void ctr_boot(void) {
    uint32_t best = 0, v;
    bool found = false;
    uint8_t pg;
    uint16_t i, used[2];

    for (pg = 0; pg < 2; pg++) {
        used[pg] = 0;                    // slots up to the last written one (valid or not)
        for (i = 0; i < CTR_ENTRIES; i++) {
            const __code uint8_t *e = ctr_entry(CTR_PAGE0 + pg, i);
            if (ctr_erased(e))
                continue;
            used[pg] = i + 1;
            if (ctr_decode(e, &v) && (!found || v > best)) {
                best = v;
                found = true;
                ctr_page = CTR_PAGE0 + pg;
            }
        }
    }
    if (!found)
        ctr_page = CTR_PAGE0;
    ctr_next = used[ctr_page - CTR_PAGE0];
    // An empty log means the chip was just flashed: start at CTR_SEED (build_and_flash_md_cam.py
    // sets it above the gateway's last accepted counter, so no 'replay reset' is needed)
    // A found log skips one block (best + 2), in case the last block before the reset could not
    // be logged (ctr_pending): its packets may have reached the gateway.
    best = found ? best + 2 : (CTR_SEED >= 1 && CTR_SEED <= CTR_BLOCK_MAX ? CTR_SEED : 1);
    ctr_pending = ctr_log(best) ? 0 : best;
    s_ctr = best << 8;
    s_ctr_first = s_ctr;
}

/* ============================================================================
 * Packet build + transmit
 * ============================================================================ */

// 24-byte packet: plain[0..1] counter low 16 bits, [2] flags, [3] battery %, [4..5] battery mV,
// [6..9] device ID[0..3], [10..11] 'V' 'S', [12] marker (0xBA poll, 0xB2 picture start,
// 0xB3..0xB9 events/diagnostics, 0 announce), [13] its argument, [14..15] counter high 16 bits. Returns the counter it used.
static uint32_t transmit(bool is_motion, bool is_por, uint8_t marker, uint8_t arg) {
    uint8_t plain[16];
    uint16_t batt_mv;
    uint8_t flags = 0;
    uint32_t ctr = s_ctr;

    clock_xosc();
    // a new block is logged before its first packet (the boot's block already is)
    if ((uint8_t)ctr == 0 && ctr != s_ctr_first)
        ctr_pending = ctr_log(ctr >> 8) ? 0 : ctr >> 8;
    else if (ctr_pending && ctr_pending == ctr >> 8 && ctr_log(ctr_pending))
        ctr_pending = 0;
    radio_init();
    aes_load_key(DEVICE_AES_KEY);

    batt_mv = s_bf_powered ? read_battery_mv() : read_battery_idle_mv();
    // with the Blackfin on this is a reading under load (also the radio's TX current): it
    // counts for the load minimum, and the packet reports the last idle reading instead, so
    // the idle thresholds and battery mV do not jump on every picture
    if (s_bf_powered) {
        if (batt_mv < s_bat_load_min)
            s_bat_load_min = batt_mv;
        if (s_bat_idle_mv)
            batt_mv = s_bat_idle_mv;
    } else {
        s_bat_idle_mv = batt_mv;
    }

    for (uint8_t i = 0; i < 8; i++) {
        tx_buf[i] = (uint8_t)DEVICE_ID[i];
    }

    if (is_motion) flags |= 0x01;           // Bit 0: Motion
    if (s_tamper)  flags |= 0x04;           // Bit 2: Tamper (SW3 released)
    if (is_por)    flags |= 0x08;           // Bit 3: Power-on reset
    if (battery_low(batt_mv)) flags |= 0x10; // Bit 4: Low battery (idle or under load)
    flags |= 0x20;                          // Bit 5: persistent counter format
    flags |= s_rst_cause << 6;              // Bits 7:6: reset cause (SLEEP.RST)

    plain[0]  = (uint8_t)(ctr >> 8);
    plain[1]  = (uint8_t)ctr;
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
    plain[12] = marker;
    plain[13] = arg;
    plain[14] = (uint8_t)(ctr >> 16);
    plain[15] = (uint8_t)(ctr >> 24);

    aes_encrypt_block(plain, &tx_buf[8]);
    s_ctr = ctr + 1;

    g_tx_count++;
    if (!radio_send_dma()) {
        g_tx_fail++;
    }
    return ctr;

    // Keep 27 MHz crystal running for high-speed SPI with Blackfin DSP
    // clock_rcosc();
}

/* ============================================================================
 * Main
 * ============================================================================ */



// Picture packet of 56 bytes (gateway packet type 0x82, cryptoProcessBigPacket): device ID,
// then 3 AES blocks chained as CBC with a zero IV (C0 = E(P0), C1 = E(P1 ^ C0), C2 = E(P2 ^ C1)),
// so a block cannot be swapped in from another packet without breaking the tag at the end.
// Plain: tag low 16 bits, 0x82, chunk number, flags (0x80 first, 0x40 last, bits 0..5 length),
// 40 data bytes, tag high 16 bits (both big endian). The tag is the counter of the picture's
// start packet (s_pic_tag): the gateway accepts a chunk only if that counter is close to the
// last one it accepted, so a recorded picture cannot be replayed later.
// The gateway receives 58-byte frames and tells this from the 24-byte packets by the CRC.
static __xdata uint8_t s_big_plain[48];
#ifndef PIC_TAG_SKEW
#define PIC_TAG_SKEW 0           // bench test only (-DPIC_TAG_SKEW=300): chunks carry an older tag,
#endif                           // as a recorded picture would; the gateway must drop them

static void transmit_big_chunk(uint8_t type, uint16_t chunk_idx, uint8_t chunk_len,
                               bool is_first, bool is_last, const uint8_t *data) {
    uint8_t i, b, flags = chunk_len & 0x3F;
    uint32_t tag = s_pic_tag - PIC_TAG_SKEW;

    aes_load_key(DEVICE_AES_KEY);
    for (i = 0; i < 8; i++)
        tx_buf[i] = (uint8_t)DEVICE_ID[i];
    if (is_first) flags |= 0x80;
    if (is_last)  flags |= 0x40;
    s_big_plain[0] = (uint8_t)(tag >> 8);
    s_big_plain[1] = (uint8_t)tag;
    s_big_plain[2] = type;          // 0x82 picture chunk, 0x83 resent chunk
    s_big_plain[3] = (uint8_t)(chunk_idx >> 8);
    s_big_plain[4] = (uint8_t)(chunk_idx & 0xFF);
    s_big_plain[5] = flags;
    for (i = 0; i < 40; i++)
        s_big_plain[6 + i] = (i < chunk_len) ? data[i] : 0x00;
    s_big_plain[46] = (uint8_t)(tag >> 24);
    s_big_plain[47] = (uint8_t)(tag >> 16);
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
#define BF_POLL_S 30             // poll the gateway (alarm state, picture requests) this often while
                                 // armed, in a motion event or with the Blackfin powered
#define BF_POLL_DISARMED_S 120   // otherwise (disarmed and idle) this often; a PIR event still polls
                                 // at once, so motion always gets a fresh alarm state
#define BF_POLL_NOW 0xF000       // poll counter value that makes the loop poll right away
#define BF_RX_WINDOW 40          // listen window after a poll, in delay_ms(1) steps (~2.75 ms each: ~110 ms)
#define BF_BOOT_MIN_MS 50        // after /RESET release P1_2 stays 1 this long (~140 ms) so the boot
                                 // selector sees PH0 low and starts snap_link, not the factory firmware
#define BF_BOOT_MAX_TRIES 200    // then ping (0xE5 -> 0xA5) every ~30 ms until snap_link answers,
                                 // at most ~6 s (it used to be a fixed wait of ~3 s)
#define BF_ARMED_IDLE_S 21600    // armed without motion: one picture this often (0 = never); 6 h: a
                                 // picture costs ~0.9 mAh, every 30 min was ~43 mAh/day (Otii 2026-11-17)
#define BF_SLEEP_IDLE 1          // 1: with the Blackfin off, wait in PM2 (0.5 s timer / PIR edge) instead of
                                 // busy-waiting: idle below 1 mA (PSU resolution) instead of ~3 mA. A first
                                 // try (2026-11-16) never woke from the sleep timer and needed a power
                                 // cycle; a later build ran fine (likely code placement), and the watchdog
                                 // (WDT_FEED) now resets the chip if it ever hangs like that again.
#define BF_AUTO_FLASH 1          // pictures while armed (motion and the idle one): snap_link takes a
                                 // grayscale flash picture instead when the room is dark (0xD5 for 0xD3,
                                 // 0xD6 for 0xD2). The flash board is infrared, so nobody sees it
#define BF_CMD_RESEND 0xF0       // downlink command: resend the chunks in the bitmap (bf_resend)
#define BF_CMD_HOLD 0xE8         // downlink command: power the Blackfin and keep it on BF_HOLD_S
#define BF_HOLD_S 600            // (no picture); for JTAG work on the Blackfin
#define BF_ACK_MAX_S 30          // after a picture: wait at most this long for the gateway's acknowledgement
                                 // (a poll answer without a resend request) before the next alarm
                                 // picture or switching the Blackfin off; meanwhile poll every
#define BF_ACK_POLL_TICKS (2 * BF_TICKS_PER_S)   // ~2 s, so a lost poll or answer costs 2 s, not the picture
#define BF_KEEP_ON_S 15        // Blackfin stays powered this long after a picture (~0.14 A), so its
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

#define BF_IMG_TIMING 0          // test: after a picture, RF B7 and B4 with the mean time per chunk of
                                 // the RF send and the SPI read (units of 0.25 ms, Timer 1).
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
// places the chunks by number. RF on a failure: B8 + last status, or B3 + length high byte.
// Returns true if the
// picture was sent. (Before 2026-11-16: 8 bytes per 24-byte packet, ~9 ms each.)
static uint16_t s_pic_len;      // length of the last picture, still in snap_link's send buffer

static bool bf_send_picture(uint8_t cmd, bool motion) {
    uint8_t st = 0, tries, i, n;
    uint16_t len, off, idx;
    s_pic_len = 0;                    // the command replaces the picture in snap_link's buffer
    s_pic_tag = transmit(motion, false, 0xB2, cmd);   // picture start: its counter tags the chunks
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
        transmit(false, false, 0xB8, st);   // no picture after ~50 s: last status
        return false;
    }
    len = bf_link_xfer(0x00);
    len |= (uint16_t)bf_link_xfer(0x00) << 8;
    if (len < 4 || len > 65000u) {
        transmit(false, false, 0xB3, (uint8_t)(len >> 8));   // implausible length (high byte)
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
        transmit_big_chunk(0x82, idx, n, idx == 0, off + n >= len, s_img_blk);
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
    transmit(false, false, 0xB7, (uint8_t)(t_tx / z / 53));
    transmit(false, false, 0xB4, (uint8_t)(t_spi / z / 53));
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
 * Every BF_POLL_S (disarmed and idle: BF_POLL_DISARMED_S) the camera sends a poll (telemetry
 * packet, plain[12] = 0xBA, [13] = state: bit 0 armed, bit 1 Blackfin powered, bit 2 a command
 * arrived and the gateway has not seen this yet, bits 7:3 battery under load, see
 * bat_load_code) and listens BF_RX_WINDOW for the gateway's answer (868Receiver/src/downlink.h):
 * 8 bytes device ID + AES(device key, 'D' 'L' counter[4] armed cmd first[2] map[4] 'V' 'S').
 * The answer must echo the poll's 32-bit counter, which never repeats (persistent counter),
 * so a recorded answer never fits a later poll.
 * ============================================================================ */
static __xdata uint8_t rx_buf[PACKET_LEN + 2];
static __xdata uint8_t s_dl_plain[16];
static __xdata uint32_t s_poll_ctr;  // counter of the last poll, echoed by the answer
static __xdata uint8_t s_dl_cmd;     // picture requested by the gateway, 0 = none
static bool s_armed = BF_ARMED_AT_BOOT;
static bool s_cmd_ack;               // report in polls that a command arrived, until answered

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

// Poll the gateway and take its answer. Returns true for a valid answer.
static __xdata uint16_t s_rs_first;  // resend request from the last poll answer: chunk s_rs_first + i
static __xdata uint32_t s_rs_map;    // for every bit i set (0 = none)

static bool cam_poll(bool motion, bool bf_on) {
    uint8_t i, state = 0;

    if (s_armed) state |= 0x01;
    if (bf_on) state |= 0x02;
    if (s_cmd_ack) state |= 0x04;
    state |= bat_load_code() << 3;   // bits 7:3: battery voltage under load
    s_poll_ctr = transmit(motion, false, 0xBA, state);
    if (!radio_receive_dma(BF_RX_WINDOW))
        return false;
    for (i = 0; i < 8; i++)
        if (rx_buf[i] != (uint8_t)DEVICE_ID[i])
            return false;
    aes_load_key(DEVICE_AES_KEY);
    aes_decrypt_block(&rx_buf[8], s_dl_plain);
    if (s_dl_plain[0] != 'D' || s_dl_plain[1] != 'L' || s_dl_plain[14] != 'V' || s_dl_plain[15] != 'S')
        return false;
    for (i = 0; i < 4; i++)
        if (s_dl_plain[2 + i] != (uint8_t)(s_poll_ctr >> (24 - 8 * i)))
            return false;
    // the gateway has seen this poll (and with it any acknowledgement)
    s_cmd_ack = false;
    if (s_dl_plain[6] <= 1)
        s_armed = s_dl_plain[6] != 0;
    if ((s_dl_plain[7] >= 0xD1 && s_dl_plain[7] <= 0xD6) ||   // 0xD4 flash, 0xD5/0xD6 flash in the dark
        s_dl_plain[7] == BF_CMD_HOLD) {
        s_dl_cmd = s_dl_plain[7];
        s_cmd_ack = true;
    } else if (s_dl_plain[7] == BF_CMD_RESEND) {
        // resend request: first chunk in [8..9], bitmap of the 32 chunks from it in [10..13]
        s_rs_first = ((uint16_t)s_dl_plain[8] << 8) | s_dl_plain[9];
        s_rs_map = (uint32_t)s_dl_plain[10] | ((uint32_t)s_dl_plain[11] << 8) |
                   ((uint32_t)s_dl_plain[12] << 16) | ((uint32_t)s_dl_plain[13] << 24);
    }
    return true;
}

// Resend chunks the gateway missed (downlink BF_CMD_RESEND: chunk first + i for every bit i in
// map): snap_link seeks to a chunk (0xE1 hi lo) in the picture it still holds and reads on from
// there, so a seek is only needed where a run of requested chunks starts. They go out as type
// 0x83 so the gateway patches them into that picture instead of starting a new one.
static void bf_resend(uint16_t first, uint32_t map) {
    uint16_t idx, off, chunks, next = 0xFFFF;
    uint8_t i, k, n;
    if (!s_pic_len)
        return;
    chunks = (s_pic_len + BF_IMG_CHUNK - 1) / BF_IMG_CHUNK;
    for (k = 0; k < 32 && map; k++, map >>= 1) {
        if (!(map & 1))
            continue;
        idx = first + k;
        if (idx >= chunks || idx < first)
            break;
        off = idx * BF_IMG_CHUNK;
        if (idx != next) {
            bf_link_xfer(0xE1);
            bf_link_xfer((uint8_t)(idx >> 8));
            bf_link_xfer((uint8_t)idx);
        }
        n = (s_pic_len - off) > BF_IMG_CHUNK ? BF_IMG_CHUNK : (uint8_t)(s_pic_len - off);
        for (i = 0; i < n; i++)
            s_img_blk[i] = bf_link_xfer(0x00);
        transmit_big_chunk(0x83, idx, n, idx == 0, off + n >= s_pic_len, s_img_blk);
        WDT_FEED();
        next = idx + 1;
    }
}

// Power the Blackfin as bf_jtag_hold() does, without its long JTAG pauses: P1_2 = 1 at
// reset release makes the boot selector start snap_link (0x8000). Then the link pins.
static uint8_t s_boot_tries;    // pings until snap_link answered (BF_IMG_TIMING reports it)

static void bf_power_on(void) {
    clock_xosc();    // cam_sleep leaves the CPU on the HS RCOSC; the boot timing needs the crystal
    s_bf_powered = true;
    P0DIR |= 0x85;   // P0_0, P0_2 (1.2V), P0_7 (aux) = outputs
    P0_0 = 0;
    P0_7 = 0;        // high while off (bf_power_off); low first so the sequence below is unchanged
    P2_0 = 0;        // likewise high while off
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
// reverse order of bf_power_on(), Q7 off (P1_0), bus tri-state. P0_1 and P0_3 are then
// driven low: as pull-up inputs they kept Q2 on the PIR board conducting, so the red LD1
// lit and the board drew ~20 mA instead of ~3 mA (memory PIR_INDICATOR_LED). P0_6 and P0_7
// are driven HIGH: the PIR board pulls both up to VBAT (5.8 and 3.7 kOhm), so holding them
// low sank ~0.9 mA all the time (Otii 2026-11-16: idle floor 1.16 -> 0.30 mA; LD1 stays off).
// P2_0 is driven HIGH as well: it is pulled up too (not "unconnected" as the pinout table says),
// and low it sank the remaining ~0.30 mA (floor 0.30 mA -> 3 uA).
static void bf_power_off(void) {
    s_bf_powered = false;
    s_pic_len = 0;   // the picture in snap_link's buffer is gone
    P0_0 = 0;
    P0_7 = 0;
    P0_2 = 0;
    P1_0 = 0;
    P1_1 = 0;
    P1_2 = 0;        // link enable off (~30 uA less in PM2 than high; bf_power_on sets it again)
    P1DIR &= ~0x38;
    P1INP |= 0x38;
    P0 &= ~0x0A;     // P0_1, P0_3 low
    P0 |= 0xC0;      // P0_6, P0_7 high (pulled up to VBAT on the PIR board)
    P0DIR |= 0xCA;
    P2_0 = 1;        // pulled up as well; bf_power_on drives it low again
    P2DIR |= 0x01;
}

#if BF_SLEEP_IDLE
// With the Blackfin off, sleep in PM2 until the next 0.5 s sleep-timer tick or a PIR edge on
// P1_7, instead of busy-waiting on the crystal (board ~22 mA awake, ~3 mA in PM2 as measured
// with the earlier low-power build). Returns the loop ticks that passed: a whole timer period
// after a tick, 1 after a PIR edge (exact time unknown; the loop reads P1_7 right away).
// The CPU stays on the HS RCOSC after the wake-up: starting the crystal at every tick cost
// ~4 ms at 5.4 mA (~24 uA, measured on the door sensor 2026-11-19), and most ticks only read
// the tamper switch and count. Whatever needs the crystal switches to it first: transmit()
// (so cam_poll too), bf_power_on() and battery_guard(); bf_on is only set by bf_power_on(),
// so the delay_ms() path with the Blackfin on runs on the crystal.
static uint16_t cam_sleep(void) {
    g_tick = 0;
    g_pir_edge = 0;
    if (!(CLKCON & 0x40))   // on the crystal: switch (clock_rcosc would start it otherwise)
        clock_rcosc();
    enter_pm2();
    WDT_FEED();
    return g_tick ? (uint16_t)(BF_TICKS_PER_S / 2) : 1;   // one 0.5 s tick
}
#endif

static void cam_alarm_loop(void) {
    uint16_t poll = BF_POLL_NOW;   // first poll right away
    uint16_t hold = 0, keep = 0, gap = 0, ack_left = 0;
    uint32_t idle = 0;
    uint8_t ev_pic = 0, cmd;
    bool active = false, bf_on = false, was_armed = s_armed;
    // A picture stays "open" until a poll answer without a resend request (the gateway has it
    // complete, or has given up), at most BF_ACK_MAX_S: until then the camera polls every
    // BF_ACK_POLL_TICKS, takes no new alarm picture and keeps the Blackfin (the picture's only
    // copy) powered. In an alarm sequence the next picture used to start right after the first
    // resend round and replaced the picture the gateway was still completing (2026-11-16).
    bool pic_open = false;
    uint8_t tamper_cnt = 0;
    uint16_t tamper_gap = 0;                    // ticks until the next 0xB5 may go out
    uint16_t tamper_backoff = TAMPER_MIN_GAP;   // the gap after the next change (doubles)
    uint16_t tamper_calm = 0;                   // ticks without a change (resets the backoff)
    uint16_t tamper_pic_wait = 0;               // ticks until the next tamper picture may be taken
    bool tamper_pic = false;        // an alarm picture is due for a tamper while armed

    bf_power_off();
#if BF_SLEEP_IDLE
    sleep_timer_init();
#endif
    WDCTL = 0x08;                   // watchdog on, ~1 s (32768 periods of 32 kHz); cannot be stopped
    for (;;) {
        // tamper: a change counts after two equal samples in a row (0.5 s apart with the
        // Blackfin off). The gap before the next 0xB5 starts at TAMPER_MIN_GAP and doubles with
        // every change up to TAMPER_MAX_GAP, back to the start after TAMPER_CALM without one, so
        // a loose mount or a marginal contact cannot flood the radio or drain the battery; every
        // packet carries the current state anyway (flags bit 2)
        if (tamper_read() != s_tamper) {
            if (tamper_cnt < 2)
                tamper_cnt++;
        } else {
            tamper_cnt = 0;
        }
        if (tamper_cnt >= 2 && !tamper_gap) {
            s_tamper = !s_tamper;
            tamper_cnt = 0;
            tamper_gap = tamper_backoff;
            tamper_backoff = tamper_backoff < TAMPER_MAX_GAP / 2 ? tamper_backoff * 2 : TAMPER_MAX_GAP;
            tamper_calm = 0;
            transmit(active, false, 0xB5, s_tamper);
            // armed and taken off the wall: one alarm picture of whoever does it (at most one per
            // TAMPER_PIC_GAP). Poll first, as for motion, so a camera that was disarmed (polls
            // every ~2 minutes) gets a fresh alarm state
            if (s_tamper) {
                poll = 0;
                cam_poll(active, bf_on);
                if (s_armed && !tamper_pic_wait)
                    tamper_pic = true;
            }
        }
        if (bf_on) {
            uint16_t mv = read_battery_mv();
            if (mv < s_bat_load_min)
                s_bat_load_min = mv;
        }
        if (P1_7 == 0) {
            if (!active) {
                active = true;
                ev_pic = 0;
                // poll first (it already carries the motion flag) for a fresh alarm state,
                // so the gateway is not busy with the B9 packet while the camera listens
                poll = 0;
                cam_poll(true, bf_on);
                transmit(true, false, 0xB9, 1);
            }
            hold = BF_PIR_HOLD_S * BF_TICKS_PER_S;
        }
        if (poll >= (pic_open ? BF_ACK_POLL_TICKS :
                     (s_armed || active || bf_on) ? BF_POLL_S * BF_TICKS_PER_S :
                     BF_POLL_DISARMED_S * BF_TICKS_PER_S)) {
            poll = 0;
            if (cam_poll(active, bf_on) && !s_rs_map)
                pic_open = false;   // answered without a resend request: the picture is done
        }
        if (s_rs_map) {
            if (bf_on) {
                bf_resend(s_rs_first, s_rs_map);
                keep = BF_KEEP_ON_S * BF_TICKS_PER_S;
                poll = BF_POLL_NOW;   // poll again right away for the next request
            }
            s_rs_map = 0;
        }
        if (s_armed && !was_armed)
            idle = 0;               // the armed idle picture counts from arming
        was_armed = s_armed;

        cmd = 0;
        if (!s_armed)
            tamper_pic = false;
        if (s_dl_cmd) {
            cmd = s_dl_cmd;
            s_dl_cmd = 0;
        } else if (tamper_pic && !pic_open) {
            tamper_pic_wait = TAMPER_PIC_GAP;
            // as the first picture of a motion event
#if BF_AUTO_FLASH && BF_IMG_CMD == 0xD3
            cmd = 0xD5;
#else
            cmd = BF_IMG_CMD;
#endif
        } else if (s_armed && active && !pic_open && !gap) {
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
            // decided by the preprocessor, as for the tamper picture: a run-time test of the
            // constant BF_IMG_CMD makes SDCC warn (110, condition always true or false)
#if BF_AUTO_FLASH && BF_IMG_CMD == 0xD3
            cmd = 0xD5;
#else
            cmd = BF_IMG_CMD;
#endif
        }

        if (cmd == BF_CMD_HOLD) {
            // service (MQTT "HOLD"): Blackfin on without a picture, e.g. for flashing it over
            // JTAG (~90 s), which BF_KEEP_ON_S alone does not cover
            if (!bf_on) {
                bf_power_on();
                bf_on = true;
                s_bat_load_min = read_battery_mv();
            }
            keep = BF_HOLD_S * BF_TICKS_PER_S;
        } else if (cmd) {
            tamper_pic = false;     // any picture now covers a pending tamper picture
            if (!bf_on) {
                bf_power_on();
#if BF_IMG_TIMING
                transmit(false, false, 0xB6, s_boot_tries);
#endif
                bf_on = true;
                s_bat_load_min = read_battery_mv();   // the Blackfin has booted: ~0.1 A
            }
            // keep it powered only after a good picture: a hung Blackfin gets power-cycled
            if (bf_send_picture(cmd, active)) {
                keep = BF_KEEP_ON_S * BF_TICKS_PER_S;
                pic_open = true;
                ack_left = BF_ACK_MAX_S * BF_TICKS_PER_S;
            } else {
                keep = 0;
            }
            idle = 0;
            // poll right after every picture: it carries the resend request or the
            // acknowledgement (a picture takes seconds over RF, so a disarm waits less too)
            poll = BF_POLL_NOW;
            hold = hold > BF_IMG_PIC_TICKS ? hold - BF_IMG_PIC_TICKS : 0;
            gap = BF_IMG_PAUSE_TICKS;   // at least ~5 s to the next alarm picture
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
            gap = gap > n ? gap - n : 0;
            tamper_gap = tamper_gap > n ? tamper_gap - n : 0;
            tamper_pic_wait = tamper_pic_wait > n ? tamper_pic_wait - n : 0;
            if (tamper_calm < TAMPER_CALM) {
                tamper_calm += n;
                if (tamper_calm >= TAMPER_CALM)
                    tamper_backoff = TAMPER_MIN_GAP;
            }
            if (pic_open) {
                ack_left = ack_left > n ? ack_left - n : 0;
                if (!ack_left)
                    pic_open = false;   // no acknowledgement within BF_ACK_MAX_S: go on
            }
            if (P1_7 != 0)
                hold = hold > n ? hold - n : 0;
        }

        if (active && !hold && P1_7 != 0) {
            active = false;
            transmit(false, false, 0xB9, 0);
        }
        if (bf_on && !s_dl_cmd && !keep && !pic_open && (!active || !s_armed)) {
            bf_power_off();
            bf_on = false;
            s_bat_load_mv = s_bat_load_min;   // from now on in the battery % and the low flag
        }
    }
}

#if BF_SLEEP_IDLE
/* Battery guard at boot: every boot writes a counter entry to flash (ctr_boot). A dying battery
 * that resets the chip on a current peak could reboot every few seconds and wear the two log
 * pages out (and write them at a marginal supply). So below BOOT_MIN_MV the camera neither logs
 * nor sends: it sleeps in PM2 and measures again about every BOOT_RECHECK_S. */
#define BOOT_MIN_MV      2300             // the low-battery flag is set below BAT_LOW_IDLE_MV
#define BOOT_RECHECK_S   60

static void battery_guard(void) {
    uint8_t i;
    for (;;) {
        clock_xosc();                // cam_sleep leaves the HS RCOSC on; ADC and ctr_boot use the crystal
        if (read_battery_idle_mv() >= BOOT_MIN_MV)
            break;
        for (i = 0; i < 2 * BOOT_RECHECK_S; i++)
            cam_sleep();             // 0.5 s sleep-timer ticks (or a PIR edge)
    }
}
#endif

void main(void) {
    s_rst_cause = (SLEEP >> 3) & 0x03;   // 0 power-on, 1 external, 2 watchdog

    setup_gpio();
    clock_xosc();
#if BF_SLEEP_IDLE
    bf_power_off();                  // idle pin state while the guard sleeps
    sleep_timer_init();
    // after a watchdog reset first sleep ~30 s: a fault that resets the chip again and again
    // would otherwise write a counter entry and send a packet every second (log wear, radio)
    if (s_rst_cause == 2) {
        uint8_t i;
        for (i = 0; i < 60; i++)
            cam_sleep();             // 0.5 s sleep-timer ticks (or a PIR edge)
    }
    battery_guard();
#endif
    ctr_boot();

    radio_init();
    delay_ms(50);

    // Announce packet: the first of this boot (counter low byte 0), with the power-on flag
    // only after a power-on reset, and the tamper state
    s_tamper = tamper_read();
    transmit(false, s_rst_cause == 0, 0, 0);
    delay_ms(100);

    cam_alarm_loop();
}
