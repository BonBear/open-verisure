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
 *    - SoC: Texas Instruments CC1110F32 (8051 CPU, 32 KB Flash, 4 KB XRAM); runs on the
 *      27 MHz crystal while awake, on the HS RC oscillator around PM2 sleep
 *    - Power: PM2 sleep (~26 uA idle) with a 0.5 s sleep-timer tick; port interrupts on
 *      reed (P1_7), shock (P1_2) and tamper (P0_1) wake it. Watchdog ~1 s, heartbeat
 *      every HEARTBEAT_INTERVAL_SEC (3600 s). Below BOOT_MIN_MV (2.3 V) at boot nothing is
 *      logged or sent; the battery is measured again about every minute.
 *    - Radio: Built-in Sub-1GHz CC1100/CC1101 transceiver core
 *    - RF Frequency: 868.950 MHz (2-FSK, 38.418 kBaud, 21.4 kHz frequency deviation)
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
 *    - P2_1 : Debug Data (DD, QFN pin 15) / Flashing Header J2 Pin 4
 *    - P2_2 : Debug Clock (DC, QFN pin 16) / Flashing Header J2 Pin 5
 *    - RESET: Pin 31 (RESET_N) connected to J2 Pin 6 and the reset button SW1
 *
 * 3. Security & Telemetry Protocol:
 *    - Packet Length: 24 bytes fixed + 2 bytes hardware CC1110 CRC
 *    - Header (Bytes 0..7): 8-character plaintext Device ID (e.g. "2JHL2MVN")
 *    - Ciphertext (Bytes 8..23): 16-byte AES-128-ECB encrypted block containing:
 *      * Bytes 0..1 : Packet counter bits 15:8 and 7:0 (32-bit persistent counter,
 *                     anti-replay; see "Persistent packet counter")
 *      * Byte 2     : Flags (0x01 Reed open, 0x02 Shock, 0x04 Tamper, 0x08 first packet
 *                     after power-on, 0x10 Low battery < 2400 mV, 0x20 counter high half
 *                     in bytes 14..15, bits 7:6 reset cause SLEEP.RST: 0 power-on,
 *                     1 RESET_N, 2 watchdog, 3 clock loss)
 *      * Byte 3     : Battery percentage (0..100%)
 *      * Bytes 4..5 : Battery voltage in millivolts (e.g. 3367 mV)
 *      * Bytes 6..9 : Device ID echo (anti-spoofing verification)
 *      * Bytes 10..11: Magic authentication bytes 'V', 'S' (0x56, 0x53)
 *      * Bytes 12..13: Hardware diagnostic ports P0 and P1
 *      * Bytes 14..15: Packet counter bits 23:16 and 31:24
 *    - The upper 24 counter bits are logged in flash pages 30-31 (0x7800-0x7FFF), so code
 *      is linked with --code-size 0x7800. Full format: documentation/cryptography_and_protocol.md
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
 * @def HEARTBEAT_INTERVAL_SEC
 * @brief Telemetry heartbeat interval in seconds when no events occur (1 hour = 3600 seconds).
 */
#define HEARTBEAT_INTERVAL_SEC 3600

/* Radio registers are written through the SDCC cc1110.h names (FSCAL0 = 0xDF1F, PA_TABLE0 =
 * 0xDF2E; 0xDF26 is reserved), never through CC1101 SPI offsets + 0xDF00, which differ. */

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

/* ============================================================================
 * Delays & Watchdog
 * ============================================================================ */

#include "../cc1110_common/delay.c"

/* ============================================================================
 * Hardware Cryptographic Engine (AES-128 Coprocessor)
 * ============================================================================ */

#include "../cc1110_common/aes.c"

/* ============================================================================
 * Battery Voltage Monitoring (Internal ADC)
 * ============================================================================ */

#include "../cc1110_common/battery_adc.c"

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
 * System Clock Initialization (27.000 MHz External Crystal)
 * ============================================================================ */

/**
 * @brief Initialize the high-speed system oscillator to the 27 MHz XOSC.
 * @details Powers up the external crystal oscillator and cleanly switches
 * the CPU clock from the internal 13.5 MHz RC oscillator to the 27 MHz crystal.
 */
void clock_init(void) {
    uint16_t timeout;

    // Turn on both crystal oscillator and RC oscillator (OSC_PD = 0)
    SLEEP &= ~0x04;

    // Wait until the 27 MHz XOSC is stable (SLEEP bit 6 = XOSC_STB)
    timeout = 50000;
    while (!(SLEEP & 0x40) && --timeout);

    // Select XOSC as system clock (CLKCON bit 6: OSC = 0) and set TICKSPD/CLKSPD to 27 MHz
    CLKCON &= ~0x47;

    // Confirm that the clock switch has taken effect
    timeout = 50000;
    while ((CLKCON & 0x40) && --timeout);

    // Power down the internal 13 MHz RC oscillator to save energy
    if (SLEEP & 0x40) {
        SLEEP |= 0x04;
    }
}

#include "../cc1110_common/clock.c"

/* ============================================================================
 * PM2 sleep: 0.5 s sleep-timer tick, port interrupts on reed, shock and tamper
 * ============================================================================ */

/** @brief Sleep Timer tick 0.5 s. The 32 kHz RC oscillator is calibrated to f_xosc / 750, which
 *  is 36.0 kHz with this board's 27 MHz crystal (not 32 kHz): with WOR_RES=2 and EVENT0=16 a tick
 *  was 0.455 s and the camera's 30 s poll came every 27.3 s (2026-11-18). WOR_RES=1 (32 periods
 *  of 27.8 us) x 563 = 0.5004 s. */
#define ST_WOR_RES        0x01
#define ST_WOREVT         563
#define TICKS_PER_S       2
#define HEARTBEAT_TICKS   ((uint16_t)HEARTBEAT_INTERVAL_SEC * TICKS_PER_S)
#define SHOCK_HOLD_TICKS  (1 * TICKS_PER_S)   // a shock is reported for ~1 s, then cleared
#define REED_MIN_GAP      4U                  // ~2 s after the first reed change (0.5 s/tick)
#define REED_MAX_GAP      600U                // doubles up to ~5 min
#define REED_CALM         1200U               // ~10 min without a reed change resets backoff

volatile __data uint8_t g_tick = 0;   // set by the sleep timer every 0.5 s
volatile __data uint8_t g_p0ifg = 0;  // port 0 / port 1 edges seen by the ISRs
volatile __data uint8_t g_p1ifg = 0;

void st_isr(void) __interrupt(ST_VECTOR) {
    SLEEP &= ~0x03;   // cancel a pending PM entry
    WORIRQ &= ~0x01;  // clear EVENT0_FLAG (keep EVENT0_MASK)
    STIF = 0;
    g_tick = 1;
}

/** @brief Port 0: tamper (P0_1). PxIFG must be cleared before PxIF. */
void p0_isr(void) __interrupt(P0INT_VECTOR) {
    SLEEP &= ~0x03;
    g_p0ifg |= P0IFG;
    P0IFG = 0x00;
    P0IF = 0;
}

/** @brief Port 1: reed (P1_7) and shock (P1_2). */
void p1_isr(void) __interrupt(P1INT_VECTOR) {
    SLEEP &= ~0x03;
    g_p1ifg |= P1IFG;
    P1IFG = 0x00;
    P1IF = 0;
}

#include "../cc1110_common/sleep_timer.c"

/**
 * @brief Enter PM2 until the next sleep-timer tick or an edge on reed, shock or tamper.
 * @pre CPU on the HS RCOSC (clock_rcosc()).
 * Reed (P1_7) and tamper (P0_1) close to GND, so with the CC1110's pull-up (~20 kOhm) a closed
 * switch costs ~150 uA each, all the time (door closed and lid on: ~0.3 mA, 20x the rest of the
 * sensor). A closed switch therefore sleeps without its pull-up (the switch holds the pin low)
 * and without an interrupt; its opening is seen at the next 0.5 s tick, when main() turns the
 * pull-up back on and reads it. An open switch keeps its pull-up and wakes the CPU at once on
 * the falling edge when it closes. Port 1 has one edge setting for all its pins (P1ICON):
 * falling; a shock pulse has both edges, so it wakes either way.
 */
static void enter_pm2(uint8_t shock_level, uint8_t last_reed, uint8_t last_tamper, bool reed_irq_en) {
    uint8_t reed, tamper, d;

    // a change since the last report (e.g. while the packet was being sent): report it now
    // instead of at the next tick (unless reed is in backoff, so rattling sleeps in PM2)
    if ((reed_irq_en && P1_7 != last_reed) || P0_1 != last_tamper)
        return;

    // switch to HS RCOSC and wait at least one 32 kHz period before disabling interrupts
    CLKCON |= 0x40;
    while (!(CLKCON & 0x40));
    for (d = 200; d != 0; d--) {    // counting down: SDCC loop reversal warns (158) on an up-count >= 128
        __asm nop __endasm;
    }

    EA = 0;
    reed = P1_7;
    tamper = P0_1;
    if ((reed_irq_en && reed != last_reed) || tamper != last_tamper) {
        EA = 1;
        return;
    }
    PICTL |= 0x03;                       // P1ICON, P0ICON: falling edge
    if (reed) {
        P1IEN = reed_irq_en ? 0x84 : 0x04; // reed open: wake when it closes (unless in backoff); shock
    } else {
        P1INP |= 0x80;                   // reed closed: no pull-up, polled at the tick
        P1IEN = 0x04;                    // shock only
    }
    if (tamper) {
        PICTL |= 0x08;                   // P0IENL: P0_0..P0_3 (tamper is P0_1)
    } else {
        P0INP |= 0x02;                   // lid on: no pull-up, polled at the tick
        PICTL &= ~0x08;
    }
    P1IFG = 0x00;
    P0IFG = 0x00;
    P1IF = 0;
    P0IF = 0;
    IEN2 |= 0x10;            // P1IE
    P0IE = 1;

    // race check: a level changed while arming, or a tick, edge, or HW IRQ is already pending
    if ((reed_irq_en && P1_7 != reed) || P0_1 != tamper || P1_2 != shock_level ||
        g_tick || g_p0ifg || g_p1ifg ||
        (IRCON & 0x80) || (IRCON & 0x20) || (IRCON2 & 0x08)) {
        EA = 1;
        return;
    }
    SLEEP = (SLEEP & ~0x03) | 0x02; // MODE = PM2
    __asm
        nop
        nop
        nop
        setb _EA
        orl _PCON, #0x01
        nop
    __endasm;
}

/* ============================================================================
 * Persistent packet counter (replay protection across resets)
 * ============================================================================
 * Each packet carries a 32-bit counter: its low 16 bits in the seq field, its high 16 bits in
 * plain[14..15] (flag 0x20). The upper 24 bits, the "block", are logged in the last two flash
 * pages before they are used: a new block at every boot and after every 256 packets. So the
 * counter never goes back, not even after a reset, and the gateway can drop every packet that is
 * not newer than the last one (before this, a reset restarted seq at 0 and the gateway had to
 * accept any packet with the power-on flag, so recorded ones could be replayed).
 * The log: 4-byte entries, appended; 0xFFFFFFFF = empty. An entry is [b0, check, b1, b2] with the
 * 24-bit block b2:b1:b0 and check = the number of 0 bits in b0, b1 and b2 (a Berger code, since
 * 2026-11-19; it was b0 ^ b1 ^ b2 ^ 0xA5, which a write cut in its second word could pass). A cut
 * write or erase and a worn cell only leave bits at 1, which lowers the count of the block bytes
 * and can only raise the stored check, so such an entry never passes and is skipped. Blocks
 * never reach 0xFF0000 (b2 = 0xFF is invalid): above CTR_BLOCK_MAX nothing is logged and nothing
 * sent, the counter never wraps. Every entry is read back; a failed one is skipped and the next
 * slot used, and a slot that is not blank is never written. When the page of the newest entry
 * is full, the other page is erased and used, so a power loss during the erase never loses the
 * newest entry; without the crystal nothing is logged (no erase, so no page switch), and the
 * other page is only erased once this one holds the newest entry (a page without a valid entry
 * yet is erased again if its erase did not finish). A packet is sent only in a
 * block up to the newest logged block + 1 (ctr_use): the next boot starts at newest + 2, so no
 * counter value can come again, even when blocks cannot be logged (the device then holds its
 * packets). Reflashing erases the log; build_and_flash.py then builds the firmware with
 * CTR_SEED = the block above the gateway's last accepted counter (GET /api/replay), so the
 * counter continues. Without the gateway it starts at block 1 and the gateway needs
 * 'replay reset <ID>' on its serial console.
 */
#define CTR_PAGE0        30                      // pages 30 and 31: 0x7800-0x7FFF
#ifndef CTR_ENTRIES
#define CTR_ENTRIES      256                     // 1 KB page / 4 bytes
#endif
#define CTR_BLOCK_MAX    0xFEFFFFUL              // b2 = 0xFF marks a cut write
#ifndef CTR_SEED
#define CTR_SEED         1UL                     // first block when the log is empty (see below)
#endif
#define CTR_PAGE_ADDR(p) ((uint16_t)(p) << 10)
#define X_FWDATA_H       0xDF                    // FWDATA in XDATA space (DMA destination)
#define X_FWDATA_L       0xAF
#define DMA_TRIG_FLASH   18

static __xdata uint8_t ctr_buf[4];
static uint8_t ctr_page;     // page the next entry goes to
static uint16_t ctr_next;    // next free entry in ctr_page (CTR_ENTRIES = full)

#include "../cc1110_common/ctr_log.c"

/** @brief Find the newest valid block and log the next one, which this boot uses (its packets
 *  are held while it is not logged: ctr_use).
 *  @return The first counter value of this boot. */
static uint32_t ctr_boot(void) {
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
    // An empty log means the chip was just flashed: start at CTR_SEED, which build_and_flash.py
    // sets above the gateway's last accepted counter, so no 'replay reset' is needed.
    // A found log skips one block (best + 2): the block after the newest entry may have been
    // used without being logged (ctr_limit). best <= CTR_BLOCK_MAX, so this cannot overflow.
    best = found ? best + 2 : (CTR_SEED >= 1 && CTR_SEED <= CTR_BLOCK_MAX ? CTR_SEED : 1);
    ctr_limit = best - 1;            // newest + 1, or below the seed: nothing used yet
    ctr_safe = true;                 // ctr_page holds the newest entry (or there is none)
    ctr_retry = 0;
    ctr_log(best);
    return best << 8;
}

/* ============================================================================
 * Battery guard at boot
 * ============================================================================
 * Every boot writes a counter entry to flash (ctr_boot). A dying battery that resets the chip on
 * the TX current dip could reboot every few seconds and wear the two log pages out (and write
 * them at a marginal supply). So below BOOT_MIN_MV the sensor neither logs nor sends: it sleeps
 * in PM2 and measures again every BOOT_RECHECK_S, and starts normally once the supply is back.
 */
#define BOOT_MIN_MV        2300                      // the low-battery flag is set below 2400 mV
#define BOOT_RECHECK_S     60
#define BOOT_RECHECK_TICKS (BOOT_RECHECK_S * TICKS_PER_S)
#define WDT_BOOT_PAUSE_S   30                        // sleep this long after a watchdog reset

/** @brief Sleep in PM2 for a number of 0.5 s sleep-timer ticks. @pre Crystal running. */
static void sleep_ticks(uint16_t ticks) {
    while (ticks) {
        uint8_t t;
        clock_rcosc();
        enter_pm2(P1_2, P1_7, P0_1, false);
        WDT_FEED();
        P1INP &= ~0x80;              // pull-ups back on (enter_pm2 drops them for closed switches)
        P0INP &= ~0x02;
        clock_xosc();
        EA = 0;
        t = g_tick;
        g_tick = 0;
        g_p0ifg = 0;
        g_p1ifg = 0;
        EA = 1;
        if (t) ticks--;
    }
}

/** @brief Wait until the supply is at least BOOT_MIN_MV. @pre Crystal running, sleep timer set. */
static void battery_guard(void) {
    while (read_battery_idle_mv() < BOOT_MIN_MV) {   // also after each PM2 recheck
        P0_2 = 0;
        P0_4 = 0;
        sleep_ticks(BOOT_RECHECK_TICKS);
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
 * - P2_1: DD (Debug Data) / Pin 4 on J2 header (Output diagnostic)
 * - P2_2: DC (Debug Clock) / Pin 5 on J2 header (Output diagnostic)
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

#include "../cc1110_common/radio_cca.c"
#include "../cc1110_common/radio_init.c"

/* ============================================================================
 * Packet Transmission Engines (DMA vs CPU-Polled)
 * ============================================================================ */

#if !USE_DMA
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
#endif

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

    // 3. Clear flags, perform CCA (when enabled), arm DMA0, and strobe STX
    radio_cca_start_tx(tx_buf);

    // 4. Wait until packet and CRC are fully transmitted (RFIF bit 4 = IRQ_DONE)
    bool underflow = false;
    timeout = 50000;
    while (!(RFIF & 0x10) && --timeout) {
        if ((MARCSTATE & 0x1F) == 0x16) {
            underflow = true; // TXFIFO_UNDERFLOW
            break;
        }
    }

    bool dma_done = (DMAIRQ & 0x01) != 0 || (RFIF & 0x10) != 0;
    bool ok = (!underflow && timeout > 0 && dma_done);

    if (!ok) {
        DMAARM = 0x81; // abort channel 0 so a failed TX never deadlocks flash_write4()
        __asm nop __endasm;
    }

    // 5. Clear interrupt flags and return to IDLE
    RFST = 0x04;
    RFTXRXIF = 0;
    RFIF = 0;

    return ok;
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
    // 01 = External Reset (RESET_N pin 31: reset button SW1 under the battery clip, or J2-6;
    //      the tamper switch is P0_1, not RESET_N)
    // 10 = Watchdog Timer Reset
    // 11 = Clock Loss Reset
    uint8_t boot_sleep = SLEEP;
    uint8_t rst_cause = (boot_sleep >> 3) & 0x03;
    bool is_button_reset = (rst_cause == 0x01);
    bool is_por_reset    = (rst_cause == 0x00);
    // watchdog on first (~1 s; cannot be stopped): the start-up (clock, battery guard, counter
    // log, radio) feeds it in delay_ms, after every sleep and in the flash routines, so a hang
    // there (e.g. an unbounded flash wait) resets the chip instead of leaving it awake for good.
    // Seen once on the camera (same start-up), 2026-11-19: after a power-on it stayed at 6.4 mA without a packet.
    // delay_ms runs ~2.75x slower while it feeds, so the start-up delays now take longer.
    WDCTL = 0x08;

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

    // 3. Initialize the 27 MHz system clock
    clock_init();

    // 3b. Nothing is logged or sent below BOOT_MIN_MV (see battery_guard). After a watchdog
    // reset, first sleep WDT_BOOT_PAUSE_S: a fault that resets the chip again and again would
    // otherwise write a counter entry and send a packet every second (log wear, radio load).
    sleep_timer_init();
    if (rst_cause == 0x02)
        sleep_ticks(WDT_BOOT_PAUSE_S * TICKS_PER_S);
    battery_guard();

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

    // persistent counter (see ctr_boot): logs this boot's block in flash, crystal must run
    uint32_t ctr = ctr_boot();
    uint8_t boot_pkts = 0;           // packets sent since this boot
    uint8_t p1_2_prev = P1_2;
    uint8_t last_reed = P1_7;
    uint8_t last_tamper = P0_1;
    uint8_t shock_hold = 0;          // ticks the shock is still reported
    uint16_t hb_ticks = 0;           // ticks since the last packet
    // XDATA storage keeps main()'s DSEG frame within the 95-byte --model-small budget
    static __xdata uint16_t s_reed_gap;      // ticks until the next reed-triggered packet may go out
    static __xdata uint16_t s_reed_backoff;
    static __xdata uint16_t s_reed_calm;
    s_reed_gap = 0;
    s_reed_backoff = REED_MIN_GAP;
    s_reed_calm = 0;
    bool prev_safe = (last_reed == 0 && last_tamper == 0);
    bool first_packet = true;

    // Idle in PM2 between events (it used to poll the inputs every ms in a busy loop, ~6.9 mA).
    // A wake-up that sends nothing (most 0.5 s ticks) stays on the HS RCOSC: starting the crystal
    // took ~4 ms at 5.4 mA per tick, ~24 of the 26 uA idle (2026-11-19). Before a packet the
    // CPU switches to the crystal (clock_xosc) and stays on it for the whole packet, ADC, flash
    // and radio, as before. (Keeping the CPU on the RCOSC and starting the crystal only for the
    // transmission left the radio silent: no TX current, nothing at the gateway.)
    sleep_timer_init();             // the watchdog runs since the start of main()

    while (1) {
        bool send_immediate = false;
        bool shock_event = false;
        bool is_heartbeat = false;

        if (first_packet) {
            first_packet = false;
            send_immediate = true;   // the first packet after boot/reset goes out at once
        } else {
            uint8_t p0f, p1f, tick;
            EA = 0;
            p0f = g_p0ifg; p1f = g_p1ifg; tick = g_tick;
            g_p0ifg = 0; g_p1ifg = 0; g_tick = 0;
            EA = 1;
            (void)p0f;

            uint8_t curr_reed = P1_7;
            uint8_t curr_tamper = P0_1;
            uint8_t curr_p1_2 = P1_2;
            if (tick) {
                if (hb_ticks < 0xFFFF) hb_ticks++;
                if (s_reed_gap > 0) s_reed_gap--;
                if (s_reed_calm < REED_CALM) {
                    s_reed_calm++;
                    if (s_reed_calm >= REED_CALM)
                        s_reed_backoff = REED_MIN_GAP;
                }
            }
            if (curr_reed != last_reed) {
                s_reed_calm = 0;
                if (!s_reed_gap) {
                    last_reed = curr_reed;
                    send_immediate = true;
                    s_reed_gap = s_reed_backoff;
                    s_reed_backoff = (s_reed_backoff < REED_MAX_GAP / 2U)
                        ? (uint16_t)(s_reed_backoff * 2U) : REED_MAX_GAP;
                }
            }
            if (curr_tamper != last_tamper) {
                last_tamper = curr_tamper;
                send_immediate = true;
            }
            // shock: an edge on P1_2 seen by the ISR, or its level changed since the last look
            if ((p1f & 0x04) || curr_p1_2 != p1_2_prev) {
                p1_2_prev = curr_p1_2;
                shock_event = true;
                // only the start of a shock is sent; further edges just extend it, so steady
                // vibration does not keep the CPU awake and flood the channel (~9 packets/s)
                if (!shock_hold)
                    send_immediate = true;
                shock_hold = SHOCK_HOLD_TICKS + 1;
            }
            if (tick) {
                if (shock_hold && !shock_event && --shock_hold == 0)
                    send_immediate = true;   // the shock is over: send the clear state
                if (!send_immediate && hb_ticks >= HEARTBEAT_TICKS)
                    is_heartbeat = true;
            }
        }

        if (send_immediate || is_heartbeat) {
            clock_xosc();   // crystal for the ADC, flash timing and radio

            // Sample current hardware sensor states for telemetry packet
            uint8_t reed_val = P1_7;
            bool is_open = (reed_val != 0);
            bool is_shock = (shock_event || shock_hold > 0);
            bool is_tamper = (P0_1 != 0);

            bool is_currently_safe = (!is_open && !is_shock && !is_tamper);
            bool returned_to_safe = (send_immediate && is_currently_safe && !prev_safe);
            bool entered_alert = (send_immediate && !is_currently_safe);

            last_reed = reed_val;
            prev_safe = is_currently_safe;
            hb_ticks = 0;

            // Measure battery voltage and calculate percentage
            uint16_t batt_mv = read_battery_idle_mv();   // right after PM2: settle first
            uint8_t batt_pct = calculate_battery_pct(batt_mv);

            // Packet Construction (24 Bytes): 1. plaintext header, the 8-character device ID
            for (uint8_t i = 0; i < 8; i++) {
                tx_buf[i] = (uint8_t)DEVICE_ID[i];
            }

            // 2. Plaintext Payload (16 Bytes) before AES-128 encryption
            static __xdata uint8_t plain[16];
            // a new block of 256 is logged in flash before it is used; a block that cannot be
            // logged (and is not the one after the newest entry) is not sent (see ctr_use)
            bool ctr_ok = ctr_use(ctr >> 8);
            plain[0] = (uint8_t)(ctr >> 8);
            plain[1] = (uint8_t)ctr;

            uint8_t flags = 0;
            if (is_open) flags |= 0x01;                       // Bit 0: Reed switch open
            if (is_shock) flags |= 0x02;                      // Bit 1: Vibration / shock
            if (is_tamper) flags |= 0x04;                     // Bit 2: Tamper / Sabotage (Cover off)
            // Bit 3: first packet after a power-on reset (shown in Home Assistant; the
            // persistent counter means the gateway needs no reset exception any more)
            if (is_por_reset && boot_pkts == 0) flags |= 0x08;
            if (batt_mv < 2400) flags |= 0x10;                // Bit 4: Low battery warning (<2.4V)
            flags |= 0x20;                                    // Bit 5: persistent counter in [14..15]
            flags |= rst_cause << 6;                          // Bits 7:6: reset cause (SLEEP.RST)

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
            plain[14] = (uint8_t)(ctr >> 16); // Counter, high half
            plain[15] = (uint8_t)(ctr >> 24);

            // 3. Encrypt the 16 payload bytes directly into tx_buf[8..23]. The AES coprocessor
            //    loses its key in PM2 (packets after the first sleep failed authentication at
            //    the gateway), so load it before every packet.
            aes_load_key(DEVICE_AES_KEY);
            aes_encrypt_block(plain, &tx_buf[8]);
            if (ctr_ok) {
                ctr++;
                if (boot_pkts < 0xFF) boot_pkts++;
            }

            // 4. Green LED on a heartbeat or back to safe, red when entering an alert; both off
            //    while idle
            if (is_heartbeat || returned_to_safe) {
                P0_4 = 1; // Green LED ON
            } else if (entered_alert) {
                P0_2 = 1; // Red LED ON
            }

            P2_1 = !P2_1;  // Toggle DD pin for oscilloscope / logic analyzer hookup

            radio_init();  // TEST2..0 and PA_TABLE1..7 are not kept in PM2 (as in the camera)
            bool ok = ctr_ok && radio_send();   // held packet: nothing sent
            P2_2 = ok ? 1 : 0; // DC pin indicates transmission success

            // The transmission itself puts a falling edge on the shock input P1_2 (seen in
            // P1IFG right after radio_send), which reported a shock and sent the next packet:
            // after the first heartbeat the sensor sent a packet every 0.31 s for good. Drop the
            // edges and the level change caused by the transmission.
            EA = 0;
            P1IFG &= ~0x04;
            g_p1ifg &= ~0x04;
            p1_2_prev = P1_2;
            EA = 1;

            delay_ms(40);
            P0_4 = 0;      // Turn off Green LED
            P0_2 = 0;      // Turn off Red LED

            // For immediate trigger events, apply a short 60 ms refractory period to prevent RF channel flooding
            if (send_immediate) {
                delay_ms(60);
            }
            clock_rcosc();
        }

        enter_pm2(p1_2_prev, last_reed, last_tamper, s_reed_gap == 0);   // CPU on the HS RCOSC here
        WDT_FEED();
        // pull-ups back on for reed and tamper before they are read (enter_pm2 drops them
        // for a closed switch). Without the crystal start there is no 4 ms gap any more, so
        // wait a little for the pin to rise if the switch has opened: a stale low would drop
        // the pull-up again and the opening would never be seen.
        P1INP &= ~0x80;
        P0INP &= ~0x02;
        {
            uint8_t d;
            for (d = 0; d < 100; d++) {
                __asm nop __endasm;
            }
        }
    }
}
