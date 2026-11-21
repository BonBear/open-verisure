/**
 * @file duty_cycle.h
 * @brief Phase E sliding-window RF duty-cycle accounting & band-50 fallback tracking.
 *
 * Pure C++17 (no Arduino, FreeRTOS, SPI, or Preferences dependencies) so it builds both
 * on the ESP32-S2 gateway and in host unit/fuzz tests. Single-threaded in loop() only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DUTY_BUCKETS            7
#define DUTY_BUCKET_MS          600000UL
#define DUTY_MAX_CAMERAS        4

#define DUTY_AIRTIME_PKT24_US   7080UL
#define DUTY_AIRTIME_CHUNK56_US 13740UL

#define DUTY_BAND50_LIMIT_US    3600000UL
#define DUTY_BAND54_LIMIT_US    360000000UL

struct CamDutyStatus {
    char     deviceId[9];
    uint32_t b50_1hMs;
    uint32_t b50BudgetPctTenths;
    uint32_t b50Peak1hMs;
    uint32_t b50OverBudgetCount;
    bool     b50OverBudget;
    uint32_t b54_1hMs;
    uint32_t b54BudgetPctTenths;
    uint32_t b54Peak1hMs;
    uint32_t b54OverBudgetCount;
    bool     b54OverBudget;
    uint64_t b50_1hUs;
    uint64_t b54_1hUs;
    uint32_t pkt24Count;
    uint32_t pchChunks;
    uint32_t fallbackChunks;
    uint32_t fallbackPictures;
    uint32_t fallbackAirtimeMs;
};

struct DutyCycleStatus {
    uint32_t gwTx1hMs;
    uint32_t gwTxBudgetPctTenths;
    uint32_t gwTxPeak1hMs;
    uint32_t gwTxOverBudgetCount;
    uint32_t gwTxPackets;
    uint32_t gwTxTotalMs;
    uint64_t gwTx1hUs;
    bool     gwTxOverBudget;
    uint32_t totalFallbackPictures;
    uint32_t totalFallbackChunks;
    uint32_t totalFallbackAirtimeMs;
    uint32_t totalCamB50OverBudgetCount;
    bool     anyB50OverBudget;
    uint32_t curBucketAgeMs;
    uint8_t  cameraCount;
    CamDutyStatus cameras[DUTY_MAX_CAMERAS];
};

/** Reset (and lazily allocate once) all duty-cycle buckets, camera slots, and totals. */
void dutyCycleInit();

/** Advance the shared 7 x 10-min sliding-window bucket clock to nowMs. */
void dutyCycleTick(uint32_t nowMs);

/** Record one 24-byte gateway downlink transmission on band 50 (868.950 MHz). */
void dutyCycleRecordGwTx(uint32_t nowMs);

/** Record one authenticated 24-byte camera packet on band 50 (868.950 MHz). */
void dutyCycleRecordCamPkt24(const char* deviceId, uint32_t nowMs);

/** Record one authenticated 56-byte camera picture chunk (band 50 fallback or band 54 PCH). */
void dutyCycleRecordCamChunk(const char* deviceId, uint8_t channel, bool isResend,
                             bool isNewPicture, uint32_t nowMs);

/** Snapshot current 1 h sliding-window metrics, peaks, over-budget flags, and fallback totals. */
void dutyCycleGetStatus(uint32_t nowMs, DutyCycleStatus* out);
