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
 *    - Devices with flag 0x20 send a 32-bit counter that never goes back (its upper part is kept
 *      in the device's flash); it must increase, also after a device reset, and its floor is
 *      kept in NVS across gateway restarts.
 *    - Old format (16-bit seq restarting at 0, older firmware): reject duplicate or older sequence numbers
 *      unless flagged as a power-on reset.
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
    bool isLowBatt;         /**< Low battery flag, set by the device (its own thresholds) */
    uint8_t batteryPct;     /**< Estimated battery level (0 - 100%) */
    uint16_t batteryMv;     /**< Battery supply voltage in millivolts (e.g. 3367 mV) */
    uint16_t batteryLoadMv; /**< Camera: lowest VBAT while the Blackfin was last powered (poll
                                 state bits 7:3, see downlink.h); 0 = unknown */
    int rssi;               /**< Received Signal Strength Indicator in dBm */
    uint8_t lqi;            /**< Link Quality Indicator (0 - 255) */
    uint8_t rawP0;          /**< Diagnostic: Port 0 raw input states from CC1110 */
    uint8_t rawP1;          /**< Diagnostic: Port 1 raw input states from CC1110 */
    uint8_t rawP2;          /**< Diagnostic: Port 2 raw input states from CC1110 */
    uint8_t rawRst;         /**< Diagnostic: Microcontroller reset reason from SLEEP register */
    uint32_t counter;       /**< 32-bit persistent counter (flag 0x20), else the 16-bit seq */
    bool hasCounter;        /**< The packet carried the persistent counter (flag 0x20) */
    float tempC;            /**< Decoded temperature in deg C (from 0xB6 packet or cached) */
    bool hasTemp;           /**< True when tempC holds a valid temperature reading */
    bool valid;             /**< True if packet passed authentication, decryption and replay check */
    uint8_t fastWakeState;  /**< Siren fast-wake state: 0 = unknown, 1 = OFF, 2 = ON */
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
 * @brief Encrypt a 16-byte plaintext block using AES-128-ECB (downlink to a camera).
 * @return True on success.
 */
bool cryptoEncryptPayload(const uint8_t deviceKey[16], const uint8_t inPlain[16], uint8_t outCipher[16]);

/**
 * @brief Key check value: first 3 bytes of AES(key, 0^16) as 6 hex chars. Lets two sides
 *        compare keys without showing them (print this, never the key).
 * @param out 7 bytes; "------" if key is null.
 */
void cryptoKeyCheckValue(const uint8_t key[16], char out[7]);

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

/**
 * @brief Camera image packet of 56 bytes (packet type 0x82): 8 bytes device ID + 3 AES blocks
 *        chained as CBC with a zero IV (C0 = E(P0), C1 = E(P1 ^ C0), C2 = E(P2 ^ C1)). Plain:
 *        [0..1] tag low 16 bits, [2] 0x82, [3..4] chunk number, [5] flags (0x80 first, 0x40 last,
 *        bits 0..5 length 1..40), [6..45] data, [46..47] tag high 16 bits. The tag is the counter
 *        of the picture's start packet (0xB2) and must be close to the device's last accepted
 *        counter, else the chunk is dropped as a replay. Valid chunks go to the image receiver.
 *        Type 0x83 is the same for a chunk resent on the gateway's request. Once a picture
 *        finishes (and has no active resend), cryptoRetirePicTag() retires its tag so replayed
 *        0x82/0x83 chunks with that tag are rejected even while still in the counter window.
 * @param channel Radio channel the packet arrived on (0 = main 868.95 MHz, 1 = PCH 869.525 MHz).
 * @return True if the packet decoded as a picture chunk (taken or dropped as stale).
 */
bool cryptoProcessBigPacket(const uint8_t* rxBuf56, const uint8_t masterKey[16], uint8_t channel = 0);

/**
 * @brief Retire a completed picture tag so replayed 0x82/0x83 chunks for that picture are
 *        rejected while the tag remains within the counter window. Safe no-op if a newer tag
 *        is already active.
 */
void cryptoRetirePicTag(const char* devId, uint32_t tag);

/**
 * @brief Mark the active picture tag as resend-only (0x83 accepted, 0x82 rejected) once its
 *        first pass has finished with holes and resend is running. Cleared on retire or a newer tag.
 */
void cryptoSetPicTagResendOnly(const char* devId);

/**
 * @brief Forget a device's replay state (RAM and NVS), e.g. after reflashing erased its flash,
 *        which restarts its persistent counter. Afterwards only a boot packet of blocks 1..maxBlock
 *        (default 8, an unseeded flash) is taken; the state survives a gateway restart (NVS).
 *        Seeding the flash above the old counter is the better way.
 * @return True if the device had replay state.
 */
bool cryptoReplayReset(const char* deviceId, uint32_t maxBlock = 0);

/**
 * @brief Last accepted persistent counter of a device (RAM or NVS), for the build script: a
 *        reflashed sensor starts above it (VerisureDoorSensor/build_and_flash.py, CTR_SEED).
 * @return False if the device has never sent the counter format.
 */
bool cryptoReplayFloor(const char* deviceId, uint32_t* ctr);

/** @brief Write counters that came within 2 s of the last NVS write; call from loop(). */
void cryptoReplayFlush();
