/*
 * Switching the system clock between the 27 MHz crystal (awake, radio, flash, ADC) and the
 * 13.5 MHz HS RCOSC (required before PM2), shared by the door sensor and the camera firmware.
 * Included by both main.c files; not compiled on its own.
 */

/** @brief Run on the 27 MHz crystal (needed for the radio). */
static void clock_xosc(void) {
    uint16_t timeout;
    SLEEP &= ~0x04;                               // power up both HS oscillators
    timeout = 50000;
    while (!(SLEEP & 0x40) && --timeout);         // XOSC_STB
    CLKCON = (CLKCON & 0x80);                     // OSC=0 (XOSC), TICKSPD=0, CLKSPD=0
    timeout = 50000;
    while ((CLKCON & 0x40) && --timeout);
    SLEEP |= 0x04;                                // power down the unused HS RCOSC
}

/** @brief Run on the 13.5 MHz HS RCOSC and power the crystal down (required before PM2). */
static void clock_rcosc(void) {
    uint16_t timeout;
    RFST = 0x04;                                  // radio SIDLE
    SLEEP &= ~0x04;
    timeout = 50000;
    while (!(SLEEP & 0x20) && --timeout);         // HFRC_STB
    CLKCON = (CLKCON & 0x80) | 0x40 | 0x08 | 0x01; // OSC=1, TICKSPD=001, CLKSPD=001
    timeout = 50000;
    while (!(CLKCON & 0x40) && --timeout);
    SLEEP |= 0x04;                                // power down the 27 MHz crystal
}
