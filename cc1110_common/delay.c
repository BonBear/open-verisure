/*
 * Busy-wait millisecond delay and watchdog feed for the CC1110, shared by the door sensor
 * and the camera firmware. Included by VerisureDoorSensor/main.c and MD_CAM/firmware/main.c
 * (each builds one translation unit, so this file is not compiled on its own).
 * Needs <cc1110.h> and <stdint.h>.
 */

/* Watchdog (WDCTL, 32 kHz clock, also runs in PM2): resets the chip if it is not fed for
 * ~1 s. Started first thing in main(); fed in delay_ms, after every sleep, in the flash
 * routines and per picture chunk. Feeding before it is started has no effect. */
#define WDT_FEED() do { WDCTL = 0xA8; WDCTL = 0x58; } while (0)

/**
 * @brief Busy-wait software millisecond delay calibrated for 27 MHz.
 * @param ms Number of milliseconds to delay.
 * @note With the loop overhead and watchdog check (2180 iterations) it runs ~2.75x slower than
 *       1 ms/step at 27 MHz (preserving MD_CAM's calibrated BF_TICKS_PER_S = 36 timing) and ~2x
 *       longer again on the 13.5 MHz HS RCOSC. Use only where a longer wait is harmless.
 */
void delay_ms(uint16_t ms) {
    while (ms--) {
        if (WDCTL & 0x08)           // per ms: delay_ms(1100) takes ~3 s (runs ~2.75x slow)
            WDT_FEED();
        for (uint16_t i = 0; i < 2180U; i++) {
            __asm nop __endasm;
        }
    }
}
