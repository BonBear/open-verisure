/**
 * @file cc1101.h
 * @brief TI CC1101 Sub-1GHz transceiver driver (26.000 MHz XT1, 868.950 MHz).
 */

#ifndef SIREN_CC1101_H
#define SIREN_CC1101_H

#include <stdint.h>

/**
 * @brief Reset and configure the CC1101 for 868.950 MHz Verisure packets, then sleep (SPWD).
 * @return 0 on success, -1 if PARTNUM/VERSION probe failed.
 */
int cc1101_init(void);

/**
 * @brief Wake CC1101 from SPWD into IDLE and restore TEST2..0 and PATABLE[0].
 */
void cc1101_wake(void);

/**
 * @brief Transmit a 24-byte frame over RF.
 * @param frame 24-byte payload (8-byte Device ID + 16-byte AES block).
 * @return 0 on success, -1 on timeout or error.
 */
int cc1101_tx(const uint8_t frame[24]);

/**
 * @brief Listen up to @p timeout_ms (+15 ms if sync active at deadline) for a 24-byte frame.
 * @param frame      Output buffer for 24 received payload bytes.
 * @param timeout_ms Receive window in milliseconds.
 * @return 1 if a CRC-valid 24-byte packet was received, 0 otherwise.
 */
int cc1101_rx(uint8_t frame[24], uint16_t timeout_ms);

/**
 * @brief Return CC1101 to IDLE, flush RX/TX FIFOs, and enter SPWD sleep.
 */
void cc1101_sleep(void);

#endif

