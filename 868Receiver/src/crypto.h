/**
 * @file crypto.h
 * @brief AES-128 Cryptographic Engine, Key Derivation (KDF) & Anti-Replay Filter
 *
 * This module provides the cryptographic core for decrypting and authenticating
 * telemetry packets from Verisure Sub-1GHz sensors.
 *
 * ============================================================================
 * CRYPTOGRAPHIC SPECIFICATION:
 * ============================================================================
 * 1. Key Derivation Function (KDF):
 *    Each sensor possesses a unique 128-bit Device AES Key derived from the
 *    128-bit Gateway Master Key and the sensor's physical 8-byte Device ID:
 *      Device_Key = AES-128-ECB(Master_Key, Pad16(Device_ID))
 *
 * 2. Packet Decryption (AES-128-ECB):
 *    The 24-byte over-the-air packet comprises:
 *    - Bytes 0..7  : Plaintext 8-character ASCII Device ID (e.g. "2JHL2MVN")
 *    - Bytes 8..23 : 16-byte AES-128 encrypted ciphertext block
 *
 * 3. Message Authentication & Anti-Spoofing:
 *    - Decrypted bytes 10..11 must match magic ASCII bytes "VS" (0x56, 0x53).
 *    - Decrypted bytes 6..9 must match the first 4 characters of the plaintext Device ID.
 *
 * 4. Replay Attack Protection:
 *    - A dynamic table tracks the last seen sequence numbers per sensor node.
 *    - Reject duplicate or older sequence numbers unless flagged as hardware resets.
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Default Development Master AES Key (16 bytes / 128 bits).
 * @note In production, overridden by `secrets.h` or ESP32 NVS config.
 */
static const uint8_t DEFAULT_MASTER_KEY[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10
};

/**
 * @struct VerisureTelemetry
 * @brief Decrypted and authenticated telemetry data structure from a sensor node.
 */
struct VerisureTelemetry {
    char deviceId[9];       /**< 8-character null-terminated sensor identifier */
    uint16_t sequence;      /**< 16-bit rolling sequence counter */
    bool isOpen;            /**< Magnetic reed switch state: true = Open (Alarm), false = Closed (Secure) */
    bool isShock;           /**< Vibration/Shock sensor state: true = Shock/Tamper detected */
    bool isButton;          /**< Sabotage/Tamper switch state: true = Cover removed (Alarm) */
    bool isPowerOn;         /**< Power-On Reset indicator: true = Battery recently replaced */
    bool isLowBatt;         /**< Low battery threshold flag: true = Battery < 2.4V */
    uint8_t batteryPct;     /**< Estimated battery level (0 - 100%) */
    uint16_t batteryMv;     /**< Battery supply voltage in millivolts (e.g. 3367 mV) */
    int rssi;               /**< Received Signal Strength Indicator in dBm */
    uint8_t lqi;            /**< Link Quality Indicator (0 - 255) */
    uint8_t rawP0;          /**< Diagnostic: Port 0 raw input states from CC1110 */
    uint8_t rawP1;          /**< Diagnostic: Port 1 raw input states from CC1110 */
    uint8_t rawP2;          /**< Diagnostic: Port 2 raw input states from CC1110 */
    uint8_t rawRst;         /**< Diagnostic: Microcontroller reset reason from SLEEP register */
    bool valid;             /**< True if packet passed authentication, decryption and replay check */
};

/**
 * @brief Derive a device-specific 128-bit key from the Master Key and Device ID.
 * @param masterKey 16-byte gateway master key.
 * @param deviceId 8-character ASCII device ID string.
 * @param outDeviceKey Output buffer for the derived 16-byte device key.
 * @return True on cryptographic success.
 */
bool cryptoDeriveDeviceKey(const uint8_t masterKey[16], const char* deviceId, uint8_t outDeviceKey[16]);

/**
 * @brief Decrypt a 16-byte ciphertext block using AES-128-ECB.
 * @param deviceKey 16-byte device encryption key.
 * @param inCipher Pointer to 16-byte encrypted input data.
 * @param outPlain Pointer to 16-byte decrypted plaintext output buffer.
 * @return True on successful decryption.
 */
bool cryptoDecryptPayload(const uint8_t deviceKey[16], const uint8_t inCipher[16], uint8_t outPlain[16]);

/**
 * @brief Process, authenticate, decrypt and validate an incoming 24-byte RF packet.
 * @param rxBuf24 Pointer to 24-byte received radio buffer (8B ID + 16B Cipher).
 * @param rssi Packet RSSI in dBm.
 * @param lqi Packet Link Quality Indicator.
 * @param masterKey 16-byte Master Key to derive the device key.
 * @param outTelem Output structure populated with parsed telemetry.
 * @return True if packet is authentic, untampered, and passes replay checks.
 */
bool cryptoProcessPacket(const uint8_t* rxBuf24, int rssi, uint8_t lqi, const uint8_t masterKey[16], VerisureTelemetry* outTelem);
