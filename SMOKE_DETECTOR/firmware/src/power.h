/*
 * EM2 sleep between packets. The RTCC runs from the 32.768 kHz LFXO (A320L on PB14/PB15;
 * LFRCO fallback when the crystal does not start) and wakes the core; RAIL is put to sleep and
 * woken around it, so the radio timebase stays consistent.
 */
#ifndef POWER_H
#define POWER_H

#include <stdbool.h>
#include <stdint.h>

/* Starts the LF clock and the RTCC (call after radio_init: the LFXO wait uses the RAIL time
 * base). Returns true when the LFXO runs, false on LFRCO. */
bool power_init(void);

/* Sleeps for about ms milliseconds: EM2, or EM1 when RAIL does not allow EM2 (counted in
 * g_sleep_refused). The radio must be idle. */
void power_sleep_ms(uint32_t ms);

#endif /* POWER_H */
