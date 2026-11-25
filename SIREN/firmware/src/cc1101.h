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

/**
 * @brief Derive deterministic WOR 8-bit address and 16-bit confirmation tag from an 8-char device ID.
 * @param device_id 8-character Device ID string.
 * @param out_addr8 Receives non-zero/non-0xFF hardware address (`0x01..0xFE`).
 * @param out_tag16 Receives 16-bit confirmation tag.
 */
static __inline__ void cc1101_wor_derive_id(const char *device_id,
                                            uint8_t *out_addr8,
                                            uint16_t *out_tag16) {
    uint32_t h = 2166136261UL;
    uint8_t i;
    uint8_t addr8;

    if (device_id == (const char *)0) {
        return;
    }
    for (i = 0U; i < 8U; i++) {
        h ^= (uint8_t)device_id[i];
        h *= 16777619UL;
    }
    addr8 = (uint8_t)((h ^ (h >> 8)) & 0xFFU);
    if (addr8 == 0U || addr8 == 0xFFU) {
        addr8 ^= 0x5AU;
    }
    if (out_addr8 != (uint8_t *)0) {
        *out_addr8 = addr8;
    }
    if (out_tag16 != (uint16_t *)0) {
        *out_tag16 = (uint16_t)(h >> 16);
    }
}

/**
 * @brief Configure the SIREN's WOR hardware address and 16-bit tag from @p device_id.
 */
void cc1101_wor_set_id(const char *device_id);

/**
 * @brief Program CC1101 for 869.525 MHz Wake-on-Radio and arm P2.7 falling-edge IRQ.
 */
void cc1101_enter_wor(void);

/**
 * @brief Disarm P2.7 IRQ, wake CC1101, and restore normal 868.950 MHz registers.
 */
void cc1101_exit_wor_to_main(void);

/**
 * @brief Read and validate a 6-byte WOR wake frame from RX FIFO.
 * @param rx_in_ms Receives clamped delay (`0..200` ms) until gateway RX listen window.
 * @return 0 on valid wake frame, -1 on mismatch, CRC failure, or FIFO overflow.
 */
int cc1101_wor_read(uint8_t *rx_in_ms);

#endif

