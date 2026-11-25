/**
 * @file test_duty_cycle.cpp
 * @brief Host unit tests for duty_cycle.{h,cpp} (Phase E duty-cycle accounting & fallback tracking).
 */

#include "test.h"
#include "duty_cycle.h"
#include <string.h>

TEST(duty_cycle_gw_tx_508_vs_509_and_peak) {
    DutyCycleStatus st;
    uint32_t t = 10000;

    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.gwTx1hUs, 0ULL);
    CHECK_EQ(st.gwTx1hMs, 0U);
    CHECK_EQ(st.gwTxOverBudgetCount, 0U);
    CHECK(!st.gwTxOverBudget);
    CHECK(!st.anyB50OverBudget);

    // 508 * 7080 us = 3,596,640 us <= 3,600,000 us (99.9%)
    for (int i = 0; i < 508; i++) {
        dutyCycleRecordGwTx(t);
    }
    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.gwTxPackets, 508U);
    CHECK_EQ(st.gwTx1hUs, 3596640ULL);
    CHECK_EQ(st.gwTx1hMs, 3597U);
    CHECK_EQ(st.gwTxBudgetPctTenths, 999U);
    CHECK_EQ(st.gwTxPeak1hMs, 3597U);
    CHECK_EQ(st.gwTxOverBudgetCount, 0U);
    CHECK(!st.gwTxOverBudget);
    CHECK(!st.anyB50OverBudget);

    // 509th packet crosses 3,600,000 us -> rising edge
    dutyCycleRecordGwTx(t);
    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.gwTx1hUs, 3603720ULL);
    CHECK_EQ(st.gwTx1hMs, 3604U);
    CHECK_EQ(st.gwTxBudgetPctTenths, 1001U);
    CHECK_EQ(st.gwTxPeak1hMs, 3604U);
    CHECK_EQ(st.gwTxOverBudgetCount, 1U);
    CHECK(st.gwTxOverBudget);
    CHECK(st.anyB50OverBudget);

    // 510th packet updates peak, does NOT re-trigger rising-edge excursion count
    dutyCycleRecordGwTx(t);
    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.gwTxPackets, 510U);
    CHECK_EQ(st.gwTx1hUs, 3610800ULL);
    CHECK_EQ(st.gwTxPeak1hMs, 3611U);
    CHECK_EQ(st.gwTxOverBudgetCount, 1U);
    CHECK(st.gwTxOverBudget);
}

TEST(duty_cycle_seven_bucket_rotation_and_second_excursion) {
    DutyCycleStatus st;
    uint32_t t = 50000;

    // 509 packets at t = 50000 (bucket 0) -> 1st excursion
    for (int i = 0; i < 509; i++) {
        dutyCycleRecordGwTx(t);
    }
    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.gwTxOverBudgetCount, 1U);
    CHECK(st.gwTxOverBudget);

    // At +60 min (6 * 10 min): bucket 0 is still inside the 7-bucket sliding window
    t += 6UL * DUTY_BUCKET_MS;
    dutyCycleTick(t);
    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.gwTx1hUs, 3603720ULL);
    CHECK(st.gwTxOverBudget);
    CHECK_EQ(st.gwTxOverBudgetCount, 1U);

    // At +69m59s: still retained
    dutyCycleGetStatus(t + DUTY_BUCKET_MS - 1000UL, &st);
    CHECK_EQ(st.gwTx1hUs, 3603720ULL);
    CHECK(st.gwTxOverBudget);

    // At +70 min (7 * 10 min): bucket 0 rotates out and is zeroed
    t += DUTY_BUCKET_MS;
    dutyCycleTick(t);
    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.gwTx1hUs, 0ULL);
    CHECK_EQ(st.gwTx1hMs, 0U);
    CHECK(!st.gwTxOverBudget);
    CHECK(!st.anyB50OverBudget);
    CHECK_EQ(st.gwTxPeak1hMs, 3604U);
    CHECK_EQ(st.gwTxOverBudgetCount, 1U);
    CHECK_EQ(st.gwTxTotalMs, 3604U);

    // 2nd excursion after recovery increments overBudgetCount to 2
    for (int i = 0; i < 509; i++) {
        dutyCycleRecordGwTx(t);
    }
    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.gwTxOverBudgetCount, 2U);
    CHECK(st.gwTxOverBudget);
}

TEST(duty_cycle_wraparound_and_large_time_jump) {
    DutyCycleStatus st;
    uint32_t t = 0xFFFF0000UL;

    for (int i = 0; i < 509; i++) {
        dutyCycleRecordGwTx(t);
    }
    dutyCycleGetStatus(t, &st);
    CHECK(st.gwTxOverBudget);
    CHECK_EQ(st.curBucketAgeMs, 0U);

    // Step across 32-bit wraparound by 65 minutes (within 7 buckets -> still retained)
    t = (uint32_t)(t + 6UL * DUTY_BUCKET_MS + 300000UL);
    dutyCycleTick(t);
    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.gwTx1hUs, 3603720ULL);
    CHECK_EQ(st.curBucketAgeMs, 300000U);
    CHECK(st.gwTxOverBudget);

    // Advance by 5 more minutes (total 70 min since start) across wraparound -> bucket 0 cleared
    t = (uint32_t)(t + 300000UL);
    dutyCycleTick(t);
    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.gwTx1hUs, 0ULL);
    CHECK(!st.gwTxOverBudget);

    // Record 1 packet, then jump >= 70 min (10 hours) -> all buckets zeroed
    dutyCycleRecordGwTx(t);
    t = (uint32_t)(t + 10UL * 6UL * DUTY_BUCKET_MS);
    dutyCycleTick(t);
    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.gwTx1hUs, 0ULL);
    CHECK_EQ(st.gwTxPackets, 510U);
}

TEST(duty_cycle_camera_b50_b54_fallback_and_limits) {
    DutyCycleStatus st;
    uint32_t t = 100000;

    // 24B camera packet counts toward B50
    dutyCycleRecordCamPkt24("CAM00001", t);
    dutyCycleGetStatus(t, &st);
    REQUIRE(st.cameraCount == 1U);
    CHECK_STR(st.cameras[0].deviceId, "CAM00001");
    CHECK_EQ(st.cameras[0].pkt24Count, 1U);
    CHECK_EQ(st.cameras[0].b50_1hUs, 7080ULL);

    // Fallback picture (channel 0): 3 chunks + 1 resend chunk = 1 fallback picture, 4 fallback chunks
    dutyCycleRecordCamChunk("CAM00001", 0, false, true, t + 10);
    dutyCycleRecordCamChunk("CAM00001", 0, false, false, t + 20);
    dutyCycleRecordCamChunk("CAM00001", 0, false, false, t + 30);
    dutyCycleRecordCamChunk("CAM00001", 0, true, false, t + 40);

    dutyCycleGetStatus(t + 40, &st);
    CHECK_EQ(st.cameras[0].fallbackPictures, 1U);
    CHECK_EQ(st.cameras[0].fallbackChunks, 4U);
    CHECK_EQ(st.cameras[0].fallbackAirtimeMs, 55U);   // 4 * 13740 = 54960 us -> 55 ms
    CHECK_EQ(st.totalFallbackPictures, 1U);
    CHECK_EQ(st.totalFallbackChunks, 4U);
    CHECK_EQ(st.totalFallbackAirtimeMs, 55U);

    // A resend-only chunk on channel 0 without a new non-resend chunk does not increment fallbackPictures
    dutyCycleRecordCamChunk("CAM00001", 0, true, true, t + 50);
    dutyCycleGetStatus(t + 50, &st);
    CHECK_EQ(st.cameras[0].fallbackPictures, 1U);
    CHECK_EQ(st.cameras[0].fallbackChunks, 5U);

    // Second fallback picture (isNewPicture = true) increments fallbackPictures to 2
    dutyCycleRecordCamChunk("CAM00001", 0, false, true, t + 60);
    dutyCycleGetStatus(t + 60, &st);
    CHECK_EQ(st.cameras[0].fallbackPictures, 2U);
    CHECK_EQ(st.cameras[0].fallbackChunks, 6U);

    // PCH picture (channel 1) does NOT touch fallback or B50; pushes B54 to exact boundary
    // 26200 * 13740 = 359,988,000 us <= 360,000,000 us; 26201 * 13740 = 360,001,740 us > 360,000,000 us
    dutyCycleRecordCamChunk("CAM00001", 1, false, true, t + 70);
    for (int i = 1; i < 26200; i++) {
        dutyCycleRecordCamChunk("CAM00001", 1, false, false, t + 70);
    }
    dutyCycleGetStatus(t + 70, &st);
    CHECK_EQ(st.cameras[0].pchChunks, 26200U);
    CHECK_EQ(st.cameras[0].b54_1hUs, 359988000ULL);
    CHECK(!st.cameras[0].b54OverBudget);
    CHECK_EQ(st.cameras[0].b54OverBudgetCount, 0U);
    CHECK_EQ(st.cameras[0].fallbackPictures, 2U);

    dutyCycleRecordCamChunk("CAM00001", 1, false, false, t + 70);
    dutyCycleGetStatus(t + 70, &st);
    CHECK_EQ(st.cameras[0].b54_1hUs, 360001740ULL);
    CHECK(st.cameras[0].b54OverBudget);
    CHECK_EQ(st.cameras[0].b54OverBudgetCount, 1U);

    // Push camera B50 over 3,600,000 us: currently 7080 + 6 * 13740 = 89,520 us;
    // + 255 chunks (3,503,700 us) = 3,593,220 us (under); + 1 chunk = 3,606,960 us (over)
    for (int i = 0; i < 255; i++) {
        dutyCycleRecordCamChunk("CAM00001", 0, false, false, t + 80);
    }
    dutyCycleGetStatus(t + 80, &st);
    CHECK_EQ(st.cameras[0].b50_1hUs, 3593220ULL);
    CHECK(!st.cameras[0].b50OverBudget);
    CHECK(!st.anyB50OverBudget);

    dutyCycleRecordCamChunk("CAM00001", 0, false, false, t + 80);
    dutyCycleGetStatus(t + 80, &st);
    CHECK_EQ(st.cameras[0].b50_1hUs, 3606960ULL);
    CHECK(st.cameras[0].b50OverBudget);
    CHECK_EQ(st.cameras[0].b50OverBudgetCount, 1U);
    CHECK_EQ(st.totalCamB50OverBudgetCount, 1U);
    CHECK(st.anyB50OverBudget);
}

TEST(duty_cycle_lru_eviction_monotonic_totals_and_invalid_ids) {
    DutyCycleStatus st;
    uint32_t t = 1000;

    // Null / empty IDs are ignored safely
    dutyCycleRecordCamPkt24(nullptr, t);
    dutyCycleRecordCamPkt24("", t);
    dutyCycleRecordCamChunk(nullptr, 0, false, true, t);
    dutyCycleRecordCamChunk("", 0, false, true, t);
    dutyCycleGetStatus(t, nullptr);
    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.cameraCount, 0U);
    CHECK_EQ(st.totalFallbackPictures, 0U);
    CHECK_EQ(st.totalFallbackChunks, 0U);

    // Fill 4 slots; push CAM00002 over B50 budget so monotonic over-budget count is tested across eviction
    dutyCycleRecordCamChunk("CAM00001", 0, false, true, t + 10);
    for (int i = 0; i < 263; i++) {
        dutyCycleRecordCamChunk("CAM00002", 0, false, i == 0, t + 20);
    }
    dutyCycleRecordCamChunk("CAM00003", 0, false, true, t + 30);
    dutyCycleRecordCamChunk("CAM00004", 0, false, true, t + 40);

    // Refresh CAM00001 so CAM00002 becomes LRU
    dutyCycleRecordCamPkt24("CAM00001", t + 50);

    dutyCycleGetStatus(t + 50, &st);
    CHECK_EQ(st.cameraCount, 4U);
    CHECK_EQ(st.totalFallbackPictures, 4U);
    CHECK_EQ(st.totalFallbackChunks, 266U);
    CHECK_EQ(st.totalCamB50OverBudgetCount, 1U);
    CHECK(st.anyB50OverBudget);

    // 5th camera evicts CAM00002; monotonic totals remain preserved
    dutyCycleRecordCamChunk("CAM00005", 0, false, true, t + 60);
    dutyCycleGetStatus(t + 60, &st);
    CHECK_EQ(st.cameraCount, 4U);
    CHECK_EQ(st.totalFallbackPictures, 5U);
    CHECK_EQ(st.totalFallbackChunks, 267U);
    CHECK_EQ(st.totalCamB50OverBudgetCount, 1U);
    CHECK(!st.anyB50OverBudget);

    bool found2 = false, found5 = false;
    for (uint8_t i = 0; i < st.cameraCount; i++) {
        if (strcmp(st.cameras[i].deviceId, "CAM00002") == 0) found2 = true;
        if (strcmp(st.cameras[i].deviceId, "CAM00005") == 0) found5 = true;
    }
    CHECK(!found2);
    CHECK(found5);
}

TEST(duty_cycle_gw_wake_b54_and_allow_gate) {
    DutyCycleStatus st;
    uint32_t t = 20000;

    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.gwB54WindowUs, 0ULL);
    CHECK_EQ(st.gwB54_1hMs, 0U);
    CHECK_EQ(st.gwB54PctTenths, 0U);
    CHECK_EQ(st.gwB54Packets, 0U);
    CHECK(dutyCycleAllowGwWake(t));

    // One TX slice = 17 * 3335 us = 56,695 us (~57 ms) on Band 54; Band 50 unaffected
    for (int i = 0; i < 17; i++) {
        dutyCycleRecordGwWakeTx(t);
    }
    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.gwB54Packets, 17U);
    CHECK_EQ(st.gwB54WindowUs, 56695ULL);
    CHECK_EQ(st.gwB54_1hMs, 57U);
    CHECK_EQ(st.gwTx1hUs, 0ULL);
    CHECK(dutyCycleAllowGwWake(t));

    // Push up to 107,466 * 3335 = 358,399,110 us: + 1,600,000 = 359,999,110 < 360,000,000 (allowed)
    for (uint32_t i = 17; i < 107466UL; i++) {
        dutyCycleRecordGwWakeTx(t);
    }
    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.gwB54WindowUs, 358399110ULL);
    CHECK(dutyCycleAllowGwWake(t));

    // 107,467th packet -> 358,402,445 + 1,600,000 = 360,002,445 >= 360,000,000 (blocked)
    dutyCycleRecordGwWakeTx(t);
    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.gwB54WindowUs, 358402445ULL);
    CHECK_EQ(st.gwB54PctTenths, 995U);
    CHECK(!dutyCycleAllowGwWake(t));

    // 70 minutes later: sliding window rotates out and allows wake bursts again
    t += 7UL * DUTY_BUCKET_MS;
    dutyCycleTick(t);
    dutyCycleGetStatus(t, &st);
    CHECK_EQ(st.gwB54WindowUs, 0ULL);
    CHECK_EQ(st.gwB54_1hMs, 0U);
    CHECK(dutyCycleAllowGwWake(t));
}
