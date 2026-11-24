/**
 * @file aes128.h
 * @brief Minimal AES-128-ECB encryption and decryption (pure C, zero static RAM).
 */

#ifndef SIREN_AES128_H
#define SIREN_AES128_H

#include <stdint.h>

/**
 * @brief Encrypt one 16-byte block with AES-128-ECB.
 * @param key 16-byte AES-128 key.
 * @param in  16-byte plaintext input (may alias @p out).
 * @param out 16-byte ciphertext output.
 */
void aes128_ecb_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/**
 * @brief Decrypt one 16-byte block with AES-128-ECB.
 * @param key 16-byte AES-128 key.
 * @param in  16-byte ciphertext input (may alias @p out).
 * @param out 16-byte plaintext output.
 */
void aes128_ecb_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

#endif

