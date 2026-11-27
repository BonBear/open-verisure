/*
 * 868.950 MHz OpenVerisure PHY on the EFR32FG14 sub-GHz radio through RAIL.
 * The PHY (src/autogen/rail_config.c) is generated from radio/openverisure_868.radioconf.
 */
#ifndef RADIO_H
#define RADIO_H

#include <stdbool.h>
#include <stdint.h>

/* HFXO must already be the HF clock. Returns false when RAIL could not be set up. */
bool radio_init(void);

/* Sends len bytes (fixed-length PHY: must be PKT_AIR_LEN) and waits until done. */
bool radio_send(const uint8_t *data, uint16_t len);

typedef enum {
  RADIO_SLEEP_ERROR,     /* RAIL_Sleep failed: do not call radio_wake() */
  RADIO_SLEEP_SHALLOW,   /* RAIL asleep, but EM2 not allowed: EM1 only */
  RADIO_SLEEP_DEEP,      /* RAIL asleep, EM2 allowed */
} radio_sleep_t;

/* Before sleeping: idles RAIL and puts it to sleep. Unless the result is RADIO_SLEEP_ERROR, call
 * radio_wake() afterwards with the time slept (RAIL timer sync is disabled). */
radio_sleep_t radio_sleep(void);
void radio_wake(uint32_t slept_us);

/* RAIL microsecond time base (valid after radio_init). */
uint32_t radio_time_us(void);

#ifdef RADIO_TEST_CW
bool radio_test_cw(uint16_t ch);
#endif

#endif /* RADIO_H */
