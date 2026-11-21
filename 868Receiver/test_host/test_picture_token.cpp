/**
 * @file test_picture_token.cpp
 * @brief Host unit tests for picture_token.{h,cpp} (Phase D Picture Token state machine).
 */

#include "test.h"
#include "picture_token.h"
#include <string.h>

TEST(pic_token_grant_and_release) {
    PicTokenStatus st;
    uint32_t t = 1000;
    pictureTokenGetStatus(t, &st);
    CHECK_EQ(st.state, PIC_TOK_IDLE);
    CHECK_STR(st.holderId, "");

    CHECK(pictureTokenRequest("CAM00001", t));
    pictureTokenGetStatus(t + 500, &st);
    CHECK_EQ(st.state, PIC_TOK_HELD);
    CHECK_STR(st.holderId, "CAM00001");
    CHECK(!st.holderLegacy);
    CHECK_EQ(st.holdMs, 500);
    CHECK_EQ(st.leaseRemMs, PIC_TOKEN_GRANT_LEASE_MS - 500);
    CHECK_EQ(st.grants, 1);

    CHECK(pictureTokenRelease("CAM00001", t + 600));
    pictureTokenGetStatus(t + 600, &st);
    CHECK_EQ(st.state, PIC_TOK_IDLE);
    CHECK_EQ(st.releases, 1);
}

TEST(pic_token_four_cam_fifo_and_reserved_promotion) {
    PicTokenStatus st;
    uint32_t t = 1000;
    REQUIRE(pictureTokenRequest("CAMA0001", t));
    CHECK(!pictureTokenRequest("CAMB0002", t + 10));
    CHECK(!pictureTokenRequest("CAMC0003", t + 20));
    CHECK(!pictureTokenRequest("CAMD0004", t + 30));
    CHECK(!pictureTokenRequest("CAME0005", t + 40));

    pictureTokenGetStatus(t + 50, &st);
    CHECK_EQ(st.state, PIC_TOK_HELD);
    CHECK_EQ(st.waitCount, 4);
    CHECK_STR(st.waitIds[0], "CAMB0002");
    CHECK_STR(st.waitIds[1], "CAMC0003");
    CHECK_STR(st.waitIds[2], "CAMD0004");
    CHECK_STR(st.waitIds[3], "CAME0005");
    CHECK_EQ(st.holds, 4);

    // Non-oldest waiter requesting again preserves FIFO order
    CHECK(!pictureTokenRequest("CAMD0004", t + 100));
    pictureTokenGetStatus(t + 100, &st);
    CHECK_EQ(st.waitCount, 4);
    CHECK_STR(st.waitIds[0], "CAMB0002");
    CHECK_STR(st.waitIds[2], "CAMD0004");

    // Release promotes CAMB0002 to RESERVED; CAMC0003 is still blocked while RESERVED
    CHECK(pictureTokenRelease("CAMA0001", t + 200));
    pictureTokenGetStatus(t + 200, &st);
    CHECK_EQ(st.state, PIC_TOK_RESERVED);
    CHECK_STR(st.holderId, "CAMB0002");
    CHECK_EQ(st.leaseRemMs, PIC_TOKEN_RESERVE_MS);
    CHECK_EQ(st.waitCount, 3);
    CHECK(!pictureTokenRequest("CAMC0003", t + 300));

    // CAMB0002 claims its reservation
    CHECK(pictureTokenRequest("CAMB0002", t + 400));
    pictureTokenGetStatus(t + 400, &st);
    CHECK_EQ(st.state, PIC_TOK_HELD);
    CHECK_STR(st.holderId, "CAMB0002");
    CHECK_EQ(st.grants, 2);
}

TEST(pic_token_idempotent_regrant_and_lost_release_reset) {
    PicTokenStatus st;
    uint32_t t = 1000;
    REQUIRE(pictureTokenRequest("CAM00001", t));

    // Lost GRANT retry (!activitySeen): refreshes leaseStartMs, preserves grantedAtMs
    t += 8000;
    CHECK(pictureTokenRequest("CAM00001", t));
    pictureTokenGetStatus(t, &st);
    CHECK_EQ(st.state, PIC_TOK_HELD);
    CHECK_EQ(st.holdMs, 8000);
    CHECK_EQ(st.leaseRemMs, PIC_TOKEN_GRANT_LEASE_MS);
    CHECK_EQ(st.grants, 2);

    // Once activity is seen, a new 0xBB means previous 0xB1 was lost -> resets grantedAtMs
    pictureTokenOnPicStart("CAM00001", t + 100);
    t += 5000;
    CHECK(pictureTokenRequest("CAM00001", t));
    pictureTokenGetStatus(t + 10, &st);
    CHECK_EQ(st.holdMs, 10);
    CHECK_EQ(st.leaseRemMs, PIC_TOKEN_GRANT_LEASE_MS - 10);
    CHECK_EQ(st.grants, 3);
}

TEST(pic_token_reserve_expiry_and_waiter_pruning) {
    PicTokenStatus st;
    uint32_t t = 1000;
    REQUIRE(pictureTokenRequest("CAMA0001", t));
    CHECK(!pictureTokenRequest("CAMB0002", t + 100));
    CHECK(!pictureTokenRequest("CAMC0003", t + 200));

    // Refresh CAMC0003 at t + 4000 so it stays valid while CAMB0002 expires
    CHECK(pictureTokenRelease("CAMA0001", t + 500));   // CAMB0002 RESERVED until t + 5000
    CHECK(!pictureTokenRequest("CAMC0003", t + 4000));

    char exp[9] = {};
    CHECK(!pictureTokenTick(t + 4999, exp, sizeof(exp)));
    CHECK(pictureTokenTick(t + 5000, exp, sizeof(exp)));
    CHECK_STR(exp, "CAMB0002");

    // CAMC0003 was refreshed at t + 4000 (age 1000 < WAIT_TTL_MS 6500), so it becomes RESERVED
    pictureTokenGetStatus(t + 5000, &st);
    CHECK_EQ(st.state, PIC_TOK_RESERVED);
    CHECK_STR(st.holderId, "CAMC0003");
    CHECK_EQ(st.expiries, 1);

    // Queue a stale waiter and verify it is pruned before promotion
    pictureTokenInit();
    REQUIRE(pictureTokenRequest("CAMA0001", 1000));
    CHECK(!pictureTokenRequest("CAMB0002", 1000));      // expires at 7500 (WAIT_TTL_MS 6500)
    CHECK(!pictureTokenRequest("CAMC0003", 6000));      // fresh at 7500 (age 1500)
    CHECK(!pictureTokenTick(7500, exp, sizeof(exp)));   // CAMB0002 pruned, CAMA0001 still held (grant 10s)
    pictureTokenGetStatus(7500, &st);
    CHECK_EQ(st.waitCount, 1);
    CHECK_STR(st.waitIds[0], "CAMC0003");
    CHECK(pictureTokenRelease("CAMA0001", 7600));
    pictureTokenGetStatus(7600, &st);
    CHECK_EQ(st.state, PIC_TOK_RESERVED);
    CHECK_STR(st.holderId, "CAMC0003");

    // If CAMC0003 does not claim before RESERVE_MS with empty queue, state returns to IDLE
    CHECK(pictureTokenTick(7600 + PIC_TOKEN_RESERVE_MS, exp, sizeof(exp)));
    CHECK_STR(exp, "CAMC0003");
    pictureTokenGetStatus(7600 + PIC_TOKEN_RESERVE_MS, &st);
    CHECK_EQ(st.state, PIC_TOK_IDLE);
}

TEST(pic_token_lease_renewal_expiry_and_max_hold) {
    PicTokenStatus st;
    uint32_t t = 1000;
    REQUIRE(pictureTokenRequest("CAMA0001", t));

    // Renew with 0xB2 and chunks every 8 s up to 90 s max hold
    for (int i = 0; i < 11; i++) {
        t += 8000;
        if (i == 0) pictureTokenOnPicStart("CAMA0001", t);
        else pictureTokenOnChunk("CAMA0001", (i % 2) != 0, t);
        pictureTokenGetStatus(t, &st);
        CHECK_EQ(st.state, PIC_TOK_HELD);
    }
    // Total elapsed = 88000 ms (< 90000 ms); 2000 ms later max hold triggers despite fresh chunk
    t += 1999;
    CHECK(!pictureTokenTick(t));
    char exp[9] = {};
    CHECK(pictureTokenTick(t + 1, exp, sizeof(exp)));
    CHECK_STR(exp, "CAMA0001");
    pictureTokenGetStatus(t + 1, &st);
    CHECK_EQ(st.state, PIC_TOK_IDLE);
    CHECK_EQ(st.expiries, 1);

    // Activity lease expiry (12 s without chunks)
    pictureTokenInit();
    REQUIRE(pictureTokenRequest("CAMA0001", 1000));
    pictureTokenOnPicStart("CAMA0001", 2000);
    CHECK(!pictureTokenTick(2000 + PIC_TOKEN_LEASE_MS - 1));
    CHECK(pictureTokenTick(2000 + PIC_TOKEN_LEASE_MS, exp, sizeof(exp)));
    CHECK_STR(exp, "CAMA0001");
}

TEST(pic_token_ack_grace_shrink_and_resend_renewal) {
    PicTokenStatus st;
    uint32_t t = 1000;
    REQUIRE(pictureTokenRequest("CAMA0001", t));

    // Poll answered before any activitySeen does NOT shrink lease
    pictureTokenOnPollAnswered("CAMA0001", false, t + 1000);
    pictureTokenGetStatus(t + 1000, &st);
    CHECK_EQ(st.leaseRemMs, PIC_TOKEN_GRANT_LEASE_MS - 1000);

    // Activity seen -> poll answered with hasResend=true renews to LEASE_MS
    pictureTokenOnChunk("CAMA0001", false, t + 2000);
    pictureTokenOnPollAnswered("CAMA0001", true, t + 5000);
    pictureTokenGetStatus(t + 5000, &st);
    CHECK_EQ(st.leaseRemMs, PIC_TOKEN_LEASE_MS);

    // Poll answered with hasResend=false shrinks remaining lease to ACK_GRACE_MS (3000 ms)
    pictureTokenOnPollAnswered("CAMA0001", false, t + 6000);
    pictureTokenGetStatus(t + 6000, &st);
    CHECK_EQ(st.leaseRemMs, PIC_TOKEN_ACK_GRACE_MS);

    // If remaining < ACK_GRACE_MS, another poll answered must NOT extend it
    pictureTokenOnPollAnswered("CAMA0001", false, t + 8000);
    pictureTokenGetStatus(t + 8000, &st);
    CHECK_EQ(st.leaseRemMs, 1000);
    CHECK(pictureTokenTick(t + 9000));
}

TEST(pic_token_wrong_and_late_release) {
    PicTokenStatus st;
    REQUIRE(pictureTokenRequest("CAMA0001", 1000));
    CHECK(!pictureTokenRequest("CAMB0002", 1100));

    // Unrelated camera release -> false, holder unaffected
    CHECK(!pictureTokenRelease("CAMZ9999", 1200));
    // Waiting camera release -> false, removed from wait queue
    CHECK(!pictureTokenRelease("CAMB0002", 1300));
    pictureTokenGetStatus(1300, &st);
    CHECK_EQ(st.state, PIC_TOK_HELD);
    CHECK_STR(st.holderId, "CAMA0001");
    CHECK_EQ(st.waitCount, 0);

    // Late 0xB1 after lease already expired -> false
    CHECK(pictureTokenTick(1000 + PIC_TOKEN_GRANT_LEASE_MS));
    CHECK(!pictureTokenRelease("CAMA0001", 1000 + PIC_TOKEN_GRANT_LEASE_MS + 10));
}

TEST(pic_token_legacy_claim_displacement_and_immediate_release) {
    PicTokenStatus st;
    // 1. Claim from IDLE and immediate release on pictureDone(resendActive=false)
    pictureTokenOnPicStart("LEGCAM01", 1000);
    pictureTokenGetStatus(1000, &st);
    CHECK_EQ(st.state, PIC_TOK_HELD);
    CHECK_STR(st.holderId, "LEGCAM01");
    CHECK(st.holderLegacy);
    CHECK_EQ(st.legacyClaims, 1);

    // While legacy holds, another camera cannot steal via 0xB2/chunk
    pictureTokenOnChunk("LEGCAM02", false, 1100);
    pictureTokenGetStatus(1100, &st);
    CHECK_STR(st.holderId, "LEGCAM01");

    pictureTokenOnPictureDone("LEGCAM01", true, 1200);
    pictureTokenGetStatus(1200, &st);
    CHECK_EQ(st.state, PIC_TOK_HELD);               // kept while resendActive
    pictureTokenOnPictureDone("LEGCAM01", false, 1300);
    pictureTokenGetStatus(1300, &st);
    CHECK_EQ(st.state, PIC_TOK_IDLE);               // released immediately
    CHECK_EQ(st.releases, 1);

    // 2. Legacy displaces RESERVED waiter to front (index 0) and releases on pollAnswered
    REQUIRE(pictureTokenRequest("TOKCAMA1", 2000));
    CHECK(!pictureTokenRequest("TOKWAIT1", 2100));
    CHECK(!pictureTokenRequest("TOKWAIT2", 2200));
    CHECK(pictureTokenRelease("TOKCAMA1", 2300));  // TOKWAIT1 is now RESERVED, TOKWAIT2 at queue[0]

    pictureTokenOnChunk("LEGCAM01", false, 2400);
    pictureTokenGetStatus(2400, &st);
    CHECK_EQ(st.state, PIC_TOK_HELD);
    CHECK_STR(st.holderId, "LEGCAM01");
    CHECK(st.holderLegacy);
    CHECK_EQ(st.waitCount, 2);
    CHECK_STR(st.waitIds[0], "TOKWAIT1");           // displaced to front
    CHECK_STR(st.waitIds[1], "TOKWAIT2");

    pictureTokenOnPollAnswered("LEGCAM01", false, 2500);
    pictureTokenGetStatus(2500, &st);
    CHECK_EQ(st.state, PIC_TOK_RESERVED);
    CHECK_STR(st.holderId, "TOKWAIT1");
    CHECK_EQ(st.waitCount, 1);
    CHECK_STR(st.waitIds[0], "TOKWAIT2");
}

TEST(pic_token_queue_bounds_and_invalid_ids) {
    PicTokenStatus st;
    CHECK(!pictureTokenRequest(nullptr, 1000));
    CHECK(!pictureTokenRequest("", 1000));
    CHECK(!pictureTokenRelease(nullptr, 1000));
    CHECK(!pictureTokenRelease("", 1000));
    pictureTokenOnPicStart(nullptr, 1000);
    pictureTokenOnChunk("", false, 1000);
    pictureTokenOnPictureDone(nullptr, false, 1000);
    pictureTokenOnPollAnswered("", false, 1000);

    // Fill holder + 4 waiters + 1 RESERVED displacement with full queue -> never overflows waitQueue[4]
    REQUIRE(pictureTokenRequest("CAM00000", 1000));
    CHECK(!pictureTokenRequest("CAM00001", 1010));
    CHECK(!pictureTokenRequest("CAM00002", 1020));
    CHECK(!pictureTokenRequest("CAM00003", 1030));
    CHECK(!pictureTokenRequest("CAM00004", 1040));
    CHECK(!pictureTokenRequest("CAM00005", 1050));  // dropped (queue full)
    CHECK(pictureTokenRelease("CAM00000", 1060));   // CAM00001 RESERVED, 3 in queue
    CHECK(!pictureTokenRequest("CAM00005", 1070));  // now 4 in queue
    pictureTokenOnPicStart("LEGCAM99", 1080);       // displaces CAM00001 to index 0, caps queue at 4
    pictureTokenGetStatus(1080, &st);
    CHECK_EQ(st.waitCount, PIC_TOKEN_MAX_CAMERAS);
    CHECK_STR(st.waitIds[0], "CAM00001");
    CHECK_STR(st.waitIds[1], "CAM00002");
    CHECK_STR(st.waitIds[2], "CAM00003");
    CHECK_STR(st.waitIds[3], "CAM00004");
}

TEST(pic_token_uint32_wraparound) {
    PicTokenStatus st;
    const uint32_t t0 = 0xFFFFFF00UL;   // 256 ms before wrap
    REQUIRE(pictureTokenRequest("CAMA0001", t0));
    CHECK(!pictureTokenRequest("CAMB0002", t0 + 100));

    // Cross 0xFFFFFFFF -> 0x00000000
    uint32_t t1 = t0 + 5000;            // wrapped: 0x00001288
    pictureTokenGetStatus(t1, &st);
    CHECK_EQ(st.state, PIC_TOK_HELD);
    CHECK_EQ(st.holdMs, 5000);
    CHECK_EQ(st.leaseRemMs, PIC_TOKEN_GRANT_LEASE_MS - 5000);

    // Activity + ACK grace across wrap
    pictureTokenOnChunk("CAMA0001", false, t1);
    pictureTokenOnPollAnswered("CAMA0001", false, t1 + 500);
    pictureTokenGetStatus(t1 + 500, &st);
    CHECK_EQ(st.leaseRemMs, PIC_TOKEN_ACK_GRACE_MS);

    // Grant expires -> CAMB0002 was pruned (age 5000 + 3000 - 100 > 6500), so goes IDLE
    char exp[9] = {};
    CHECK(pictureTokenTick(t1 + 500 + PIC_TOKEN_ACK_GRACE_MS, exp, sizeof(exp)));
    CHECK_STR(exp, "CAMA0001");
    pictureTokenGetStatus(t1 + 500 + PIC_TOKEN_ACK_GRACE_MS, &st);
    CHECK_EQ(st.state, PIC_TOK_IDLE);
}
