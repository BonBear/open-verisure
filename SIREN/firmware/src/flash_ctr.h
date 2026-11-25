/**
 * @file flash_ctr.h
 * @brief Persistent 32-bit rolling sequence counter in MSP430F149 Info Flash.
 *
 * Info B (0x1000..0x107F) and Info A (0x1080..0x10FF) each hold 32 x 4-byte
 * slots storing the upper 24 bits (the "block") with a 0-bit count (Berger code)
 * for unidirectional flash-fault detection.
 */

#ifndef SIREN_FLASH_CTR_H
#define SIREN_FLASH_CTR_H

#include <stdint.h>

#define CTR_BLOCK_MAX   0xFEFFFFUL
#define CTR_MIN_BATT_MV 2700U

/**
 * @brief Scan Info Flash segments B & A, advance past any unlogged block, and
 *        initialize the persistent counter.
 * @param seed_block Initial block when Info Flash has no valid entries (clamped >= 1).
 */
void ctr_boot(uint32_t seed_block);

/**
 * @brief Allocate the next strictly increasing 32-bit packet counter.
 * @param batt_mv Current supply voltage in millivolts (must be >= 2700 mV to write flash).
 * @param out_ctr Receives the counter value to place into the packet.
 * @return 0 on success, -1 if the packet must be held (low battery, ceiling, or flash failure).
 */
int ctr_next(uint16_t batt_mv, uint32_t *out_ctr);

#ifdef HOST_TEST
extern uint8_t g_mock_info[256];
extern int g_mock_cut_word1;
extern int g_mock_skip_writes;
extern int g_mock_fail_erase;

void flash_mock_reset(void);
#endif

#endif

