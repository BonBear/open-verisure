/*
 * Persistent 32-bit packet counter (anti-replay, see .agents/memory/rf_crypto.md).
 *
 * The upper 24 bits ("block") are logged in the last two 2 kB flash pages; the low byte counts
 * packets in RAM. On boot the counter continues at the next block, so a reset never reuses a
 * counter value the gateway has seen. The pages are outside the firmware image, so reflashing
 * with tools/flash.py (sector erase) keeps them.
 */
#ifndef CTR_LOG_H
#define CTR_LOG_H

#include <stdbool.h>
#include <stdint.h>

/* Opens a new block and returns its first counter value (block << 8); false if flash failed. */
bool ctr_log_init(uint32_t *counter);

/* Call before sending with counter; opens the next block when the low byte wrapped to 0. */
bool ctr_log_reserve(uint32_t counter);

#endif /* CTR_LOG_H */
