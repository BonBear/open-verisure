/**
 * @file picture_token.h
 * @brief Phase D single-flight RF Picture Token state machine for MD_CAM cameras.
 *
 * Pure C++17 (no Arduino, FreeRTOS, SPI, or Preferences dependencies) so it builds both
 * on the ESP32-S2 gateway and in host unit/fuzz tests. Single-threaded in loop() only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PIC_TOKEN_MAX_CAMERAS     4
#define PIC_TOKEN_GRANT_LEASE_MS  10000UL
#define PIC_TOKEN_LEASE_MS        12000UL
#define PIC_TOKEN_ACK_GRACE_MS    3000UL
#define PIC_TOKEN_RESERVE_MS      4500UL
#define PIC_TOKEN_WAIT_TTL_MS     6500UL
#define PIC_TOKEN_MAX_HOLD_MS     90000UL

#define PIC_TOKEN_REQ_MARKER      0xBB
#define PIC_TOKEN_REL_MARKER      0xB1
#define PIC_TOKEN_REL_OK          0x00
#define PIC_TOKEN_REL_ERROR       0x01
#define PIC_TOKEN_REL_ACK_TIMEOUT 0x02

enum PicTokenState : uint8_t {
    PIC_TOK_IDLE = 0,
    PIC_TOK_RESERVED = 1,
    PIC_TOK_HELD = 2
};

struct PicTokenStatus {
    PicTokenState state;
    char holderId[9];
    bool holderLegacy;
    uint32_t holdMs;
    uint32_t leaseRemMs;
    uint8_t waitCount;
    char waitIds[PIC_TOKEN_MAX_CAMERAS][9];
    uint32_t grants;
    uint32_t holds;
    uint32_t releases;
    uint32_t expiries;
    uint32_t legacyClaims;
};

/** Reset token state, wait queue, and statistics. */
void pictureTokenInit();

/**
 * @brief Advance timers: prune stale waiters and expire RESERVED or HELD leases.
 * @param nowMs Current millisecond timestamp (wrap-safe).
 * @param expiredId Optional buffer receiving the expired holder/reserved ID (NUL-terminated).
 * @param expiredIdSize Size of expiredId buffer in bytes.
 * @return true if a HELD or RESERVED lease expired on this tick.
 */
bool pictureTokenTick(uint32_t nowMs, char* expiredId = nullptr, size_t expiredIdSize = 9);

/**
 * @brief Handle a 0xBB picture token request from deviceId.
 * @return true if granted (DL_FLAG_GRANT), false if held/queued (DL_FLAG_HOLD).
 */
bool pictureTokenRequest(const char* deviceId, uint32_t nowMs);

/**
 * @brief Handle an explicit release (0xB1, 0x00 boot, 0xB8/0xB3 abort, replay reset, or TX rollback).
 *        Removes deviceId from the wait queue if waiting; if holding/reserved, promotes next waiter.
 * @return true if deviceId was the current holder or reserved camera.
 */
bool pictureTokenRelease(const char* deviceId, uint32_t nowMs);

/** Implicitly claim (legacy) or promote RESERVED -> HELD / renew on 0xB2 picture start. */
void pictureTokenOnPicStart(const char* deviceId, uint32_t nowMs);

/** Implicitly claim (legacy) or promote RESERVED -> HELD / renew on 0x82 or 0x83 chunk. */
void pictureTokenOnChunk(const char* deviceId, bool isResend, uint32_t nowMs);

/** Renew lease if resendActive, or immediately release if held by a legacy camera. */
void pictureTokenOnPictureDone(const char* deviceId, bool resendActive, uint32_t nowMs);

/**
 * @brief Update token lease after a 0xBA poll answer is queued to deviceId.
 *        If hasResend, renew lease; else if activitySeen, release legacy immediately or shrink
 *        token-aware lease to at most PIC_TOKEN_ACK_GRACE_MS.
 */
void pictureTokenOnPollAnswered(const char* deviceId, bool hasResend, uint32_t nowMs);

/** Snapshot current state, wait queue, and counters into out. */
void pictureTokenGetStatus(uint32_t nowMs, PicTokenStatus* out);
