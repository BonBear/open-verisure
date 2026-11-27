/*
 * Battery voltage: ADC0 measures AVDD (the 3.0 V CR-2/3AZ cell) against the
 * internal 5 V full-scale reference. Call while the radio is idle (no TX load on the cell).
 */
#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>

/* AVDD in millivolts (average of a few conversions); 0 when the ADC did not finish. Needs
 * radio_init() first (the timeout uses the RAIL time base). */
uint16_t battery_read_mv(void);

/* Same curve as the siren (same Li-MnO2 cell): 3000 mV = 100 %, 2200 mV = 0 %. */
uint8_t battery_pct(uint16_t mv);

#endif /* BATTERY_H */
