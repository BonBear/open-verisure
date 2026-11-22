/*
 * Internal VDD/3 battery voltage ADC measurement for the CC1110, shared by the door sensor
 * and the camera firmware. Included by both main.c files; not compiled on its own.
 * Needs <cc1110.h>, <stdint.h>, and delay_ms() from delay.c.
 */

/**
 * @brief Measure the battery supply voltage (VDD) using the internal ADC.
 * @return Battery voltage in millivolts (e.g. 3050 mV).
 * @note Uses the internal 1.25V bandgap voltage reference and VDD/3 channel.
 *       Runs 2 conversions and discards the first to avoid stale EOC readings.
 */
uint16_t read_battery_mv(void) {
    uint8_t n;
    uint16_t timeout;
    for (n = 0; n < 2; n++) {
        ADCCON3 = 0x3F; // Internal 1.25V ref, 12-bit decimation, VDD/3
        timeout = 5000;
        while (!(ADCCON1 & 0x80) && --timeout);
        if (n == 0)
            (void)ADCL, (void)ADCH;   // reading ADCH clears EOC for the next conversion
    }

    // Read 12-bit result (left-justified across ADCH:ADCL): VDD = 3 * (adc / 2047) * 1250 mV
    uint16_t adc = ((uint16_t)ADCL >> 4) | ((uint16_t)ADCH << 4);
    return (uint16_t)(((uint32_t)adc * 3750UL) / 2047UL);
}

/**
 * @brief Battery voltage after a wake-up from PM2.
 * @note After PM2 the VDD/3 reading is ~110 mV low for several ms while the reference settles
 *       (2026-11-17, Otii at 3.0 V: 2861 mV ~3 ms after wake, 2969 mV after ~11 ms more);
 *       a second conversion right away does not fix it. Waits ~11 ms (delay_ms runs ~2.75x slow).
 */
uint16_t read_battery_idle_mv(void) {
    delay_ms(4);
    return read_battery_mv();
}
