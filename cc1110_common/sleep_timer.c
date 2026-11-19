/*
 * Sleep timer Event0, the periodic wake-up from PM2, shared by the door sensor and the camera.
 * Included by both main.c files; not compiled on its own. The includer defines ST_WOR_RES and
 * ST_WOREVT (0.5 s with the 36 kHz sleep clock) and the st_isr that handles the event.
 */

/** @brief Start the periodic sleep-timer Event0 (runs on the 36 kHz calibrated RC oscillator). */
static void sleep_timer_init(void) {
    uint8_t t0;
    uint16_t timeout;

    WORCTRL = 0x04 | ST_WOR_RES; // WOR_RESET + resolution
    // wait for 2 sleep-timer edges after the reset (SWRS033), bounded
    t0 = WORTIME0;
    timeout = 60000;
    while (WORTIME0 == t0 && --timeout);
    t0 = WORTIME0;
    timeout = 60000;
    while (WORTIME0 == t0 && --timeout);

    WOREVT1 = (uint8_t)(ST_WOREVT >> 8);
    WOREVT0 = (uint8_t)ST_WOREVT;
    WORIRQ = 0x10;  // EVENT0_MASK = 1, clear flag
    STIF = 0;
    STIE = 1;
}
