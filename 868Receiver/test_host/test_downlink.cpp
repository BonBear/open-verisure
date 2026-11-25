/**
 * @file test_downlink.cpp
 * @brief Host tests of downlink.cpp: picture commands and the camera's ACK bit.
 *
 * The camera's poll carries DOWNLINK_STATE_ACK while the last answer it received carried a
 * picture command; any other answer clears it. The model below behaves like that, so the tests
 * can lose answers on purpose.
 */

#include "test.h"
#include "downlink.h"

namespace {

struct CameraModel {
    bool ack = false;
    uint8_t got = 0;      // last picture command the camera received

    /** One poll: the gateway's answer, which the camera receives unless 'lost'. */
    uint8_t poll(bool lost = false) {
        uint16_t first;
        uint32_t map;
        uint8_t cmd = downlinkCommandForPoll("CAMERA01", ack ? DOWNLINK_STATE_ACK : 0, &first, &map);
        if (!lost) {
            ack = (cmd >= 0xD1 && cmd <= 0xD6) || cmd == 0xE8;
            if (ack) got = cmd;
        }
        return cmd;
    }
};

}  // namespace

TEST(downlink_command_sent_and_cleared_by_its_ack) {
    downlinkInit();
    CameraModel c;
    REQUIRE(downlinkRequestPicture("CAMERA01", 0xD1));
    CHECK_EQ(c.poll(), 0xD1);
    CHECK_EQ(c.got, 0xD1);
    CHECK_EQ(c.poll(), 0);              // the ACK clears it: no second picture
}

TEST(downlink_command_repeated_until_acknowledged) {
    downlinkInit();
    CameraModel c;
    REQUIRE(downlinkRequestPicture("CAMERA01", 0xD2));
    CHECK_EQ(c.poll(true), 0xD2);       // answer lost
    CHECK_EQ(c.poll(true), 0xD2);       // lost again
    CHECK_EQ(c.poll(), 0xD2);           // arrives
    CHECK_EQ(c.poll(), 0);
}

// Review 2026-11-19: an ACK left over from an older command must not clear a newer command whose
// answer was lost. A new command only goes out in the answer to a poll without the ACK bit.
TEST(downlink_leftover_ack_does_not_clear_a_newer_command) {
    downlinkInit();
    CameraModel c;
    REQUIRE(downlinkRequestPicture("CAMERA01", 0xD1));
    CHECK_EQ(c.poll(), 0xD1);           // A arrives, the camera's ACK is set
    REQUIRE(downlinkRequestPicture("CAMERA01", 0xD4));   // B replaces A before A's ACK came in
    CHECK_EQ(c.poll(), 0);              // the poll still reports an ACK: an answer without a command
    CHECK_EQ(c.poll(true), 0xD4);       // ACK clear now: B goes out, but this answer is lost
    CHECK_EQ(c.poll(), 0xD4);           // no ACK for B yet: B again, and it arrives
    CHECK_EQ(c.got, 0xD4);
    CHECK_EQ(c.poll(), 0);              // B's own ACK clears it
}

TEST(downlink_leftover_ack_after_a_lost_empty_answer) {
    downlinkInit();
    CameraModel c;
    REQUIRE(downlinkRequestPicture("CAMERA01", 0xD1));
    CHECK_EQ(c.poll(), 0xD1);
    CHECK_EQ(c.poll(true), 0);          // A acknowledged; the empty answer is lost, so the ACK stays
    REQUIRE(downlinkRequestPicture("CAMERA01", 0xD5));
    CHECK_EQ(c.poll(), 0);              // the leftover ACK holds B back for one poll
    CHECK_EQ(c.poll(true), 0xD5);       // sent, lost
    CHECK_EQ(c.poll(), 0xD5);           // not cleared by a stale ACK: sent again
    CHECK_EQ(c.got, 0xD5);
}

TEST(downlink_resend_request_goes_out_once) {
    downlinkInit();
    CameraModel c;
    REQUIRE(downlinkRequestResend("CAMERA01", 100, 0x5));
    uint16_t first = 0;
    uint32_t map = 0;
    CHECK_EQ(downlinkCommandForPoll("CAMERA01", 0, &first, &map), DOWNLINK_CMD_RESEND);
    CHECK_EQ(first, 100);
    CHECK_EQ(map, 0x5);
    CHECK_EQ(downlinkCommandForPoll("CAMERA01", 0, &first, &map), 0);
}

// Bench 2026-11-20: a new picture command must not go out before a pending resend request, or the
// camera takes the answer as the acknowledgement of the open picture and it ends with holes.
TEST(downlink_resend_goes_before_an_unsent_command) {
    downlinkInit();
    CameraModel c;
    REQUIRE(downlinkRequestPicture("CAMERA01", 0xD3));
    REQUIRE(downlinkRequestResend("CAMERA01", 184, 0x3));
    uint16_t first = 0;
    uint32_t map = 0;
    CHECK_EQ(downlinkCommandForPoll("CAMERA01", 0, &first, &map), DOWNLINK_CMD_RESEND);
    CHECK_EQ(first, 184);
    CHECK_EQ(map, 0x3);
    CHECK_EQ(c.poll(), 0xD3);           // the next ACK-free poll gets the command
    CHECK_EQ(c.poll(), 0);
}

// Also a command already sent (no ACK yet: the camera did not receive it) waits for the resend.
TEST(downlink_resend_goes_before_a_sent_command) {
    downlinkInit();
    CameraModel c;
    REQUIRE(downlinkRequestPicture("CAMERA01", 0xD3));
    CHECK_EQ(c.poll(true), 0xD3);       // sent, answer lost: no ACK yet
    REQUIRE(downlinkRequestResend("CAMERA01", 24, 0x1));
    uint16_t first = 0;
    uint32_t map = 0;
    CHECK_EQ(downlinkCommandForPoll("CAMERA01", 0, &first, &map), DOWNLINK_CMD_RESEND);
    CHECK_EQ(first, 24);
    CHECK_EQ(map, 0x1);
    CHECK_EQ(c.poll(), 0xD3);           // the next ACK-free poll gets the command
    CHECK_EQ(c.poll(), 0);              // its ACK clears it
}

// The bench sequence of 2026-11-20 (D1 full picture with holes, D3 queued meanwhile).
TEST(downlink_bench_sequence_resend_rounds_before_the_queued_command) {
    downlinkInit();
    CameraModel c;
    uint16_t first = 0;
    uint32_t map = 0;
    REQUIRE(downlinkRequestPicture("CAMERA01", 0xD1));
    CHECK_EQ(c.poll(), 0xD1);           // delivered, the camera's ACK is set
    REQUIRE(downlinkRequestPicture("CAMERA01", 0xD3));   // replaces D1 in the slot
    REQUIRE(downlinkRequestResend("CAMERA01", 24, 0x3));
    CHECK_EQ(downlinkCommandForPoll("CAMERA01", DOWNLINK_STATE_ACK, &first, &map), DOWNLINK_CMD_RESEND);
    CHECK_EQ(first, 24);
    CHECK_EQ(map, 0x3);
    c.ack = false;                      // a 0xF0 answer clears the camera's ACK
    REQUIRE(downlinkRequestResend("CAMERA01", 184, 0x3));   // round 2
    CHECK_EQ(c.poll(), DOWNLINK_CMD_RESEND);
    CHECK_EQ(c.poll(), 0xD3);           // no ACK in the poll: the command goes out now
    CHECK_EQ(c.poll(), 0);              // the poll with D3's ACK clears it
}

TEST(downlink_cancel_resend_keeps_the_command_and_frees_the_slot) {
    downlinkInit();
    CameraModel c;
    REQUIRE(downlinkRequestPicture("CAMERA01", 0xD3));
    REQUIRE(downlinkRequestResend("CAMERA01", 10, 0x1));
    downlinkCancelResend("CAMERA01");
    CHECK(!downlinkResendPending("CAMERA01"));
    CHECK_EQ(c.poll(), 0xD3);           // the command is untouched
    downlinkInit();
    REQUIRE(downlinkRequestResend("CAMERA01", 10, 0x1));
    downlinkCancelResend("CAMERA01");
    CHECK_EQ(c.poll(), 0);              // nothing left to send
}

TEST(downlink_rejects_unknown_commands) {
    downlinkInit();
    CHECK(!downlinkRequestPicture("CAMERA01", 0xD0));
    CHECK(!downlinkRequestPicture("CAMERA01", 0xD7));
    CHECK(downlinkRequestPicture("CAMERA01", 0xE8));      // HOLD (service)
}

TEST(downlink_build_plain_pch_flags_and_resend) {
    uint8_t plain[16];

    // Non-resend without flags: bytes [8..13] are zero
    downlinkBuildPlain(0x12345678UL, true, 0, 0xABCD, 0x11223344UL, plain);
    CHECK_EQ(plain[0], 'D');
    CHECK_EQ(plain[1], 'L');
    CHECK_EQ(plain[2], 0x12);
    CHECK_EQ(plain[3], 0x34);
    CHECK_EQ(plain[4], 0x56);
    CHECK_EQ(plain[5], 0x78);
    CHECK_EQ(plain[6], 1);
    CHECK_EQ(plain[7], 0);
    for (int i = 8; i <= 13; i++) CHECK_EQ(plain[i], 0);
    CHECK_EQ(plain[14], 'V');
    CHECK_EQ(plain[15], 'S');

    // Non-resend with DL_FLAG_PCH: [8] = flags, [9] = channel, [10..13] = 0
    downlinkBuildPlain(0x00000102UL, false, 0xD1, 0, 0, plain, DL_FLAG_PCH, DL_PCH_CH_869525);
    CHECK_EQ(plain[6], 0);
    CHECK_EQ(plain[7], 0xD1);
    CHECK_EQ(plain[8], DL_FLAG_PCH);
    CHECK_EQ(plain[9], DL_PCH_CH_869525);
    for (int i = 10; i <= 13; i++) CHECK_EQ(plain[i], 0);

    // Non-resend with DL_FLAG_TOKEN | DL_FLAG_PCH + non-zero channel
    downlinkBuildPlain(1, true, 0, 0, 0, plain, DL_FLAG_TOKEN | DL_FLAG_PCH | DL_FLAG_GRANT, 2);
    CHECK_EQ(plain[8], DL_FLAG_TOKEN | DL_FLAG_PCH | DL_FLAG_GRANT);
    CHECK_EQ(plain[9], 2);
    for (int i = 10; i <= 13; i++) CHECK_EQ(plain[i], 0);

    // Non-resend without DL_FLAG_PCH: channel byte [9] stays 0 even if channel arg != 0
    downlinkBuildPlain(1, true, 0, 0, 0, plain, DL_FLAG_TOKEN | DL_FLAG_HOLD, 3);
    CHECK_EQ(plain[8], DL_FLAG_TOKEN | DL_FLAG_HOLD);
    CHECK_EQ(plain[9], 0);

    // Resend (0xF0): [8..9] = first (BE), [10..13] = map (LE), flags/channel must NOT overwrite them
    downlinkBuildPlain(0xAABBCCDDUL, true, DOWNLINK_CMD_RESEND, 0x1234, 0xA5B6C7D8UL, plain,
                       DL_FLAG_TOKEN | DL_FLAG_PCH | DL_FLAG_GRANT, 5);
    CHECK_EQ(plain[7], DOWNLINK_CMD_RESEND);
    CHECK_EQ(plain[8], 0x12);
    CHECK_EQ(plain[9], 0x34);
    CHECK_EQ(plain[10], 0xD8);
    CHECK_EQ(plain[11], 0xC7);
    CHECK_EQ(plain[12], 0xB6);
    CHECK_EQ(plain[13], 0xA5);
    CHECK_EQ(plain[14], 'V');
    CHECK_EQ(plain[15], 'S');

    // Siren ON (0xA1): [8] = clamped duration (0..180 s), [9] = tone (0..5), [10..13] = 0
    downlinkBuildPlain(0x01020304UL, false, DOWNLINK_CMD_SIREN_ON, 60, 0, plain);
    CHECK_EQ(plain[7], DOWNLINK_CMD_SIREN_ON);
    CHECK_EQ(plain[8], 60);
    for (int i = 9; i <= 13; i++) CHECK_EQ(plain[i], 0);
    CHECK_EQ(plain[14], 'V');
    CHECK_EQ(plain[15], 'S');

    downlinkBuildPlain(0x01020304UL, true, DOWNLINK_CMD_SIREN_ON, 250, 0, plain, DL_FLAG_PCH, 2);
    CHECK_EQ(plain[7], DOWNLINK_CMD_SIREN_ON);
    CHECK_EQ(plain[8], DOWNLINK_SIREN_MAX_SEC);
    CHECK_EQ(plain[9], 2);
    for (int i = 10; i <= 13; i++) CHECK_EQ(plain[i], 0);

    downlinkBuildPlain(0x01020304UL, false, DOWNLINK_CMD_SIREN_ON, 0, 0, plain, 0, 1);
    CHECK_EQ(plain[7], DOWNLINK_CMD_SIREN_ON);
    CHECK_EQ(plain[8], 0);
    CHECK_EQ(plain[9], 1);
    for (int i = 10; i <= 13; i++) CHECK_EQ(plain[i], 0);

    // Invalid tone > DOWNLINK_SIREN_MAX_TONE clamps to 0 (alarm)
    downlinkBuildPlain(0x01020304UL, false, DOWNLINK_CMD_SIREN_ON, 10, 0, plain, 0, 9);
    CHECK_EQ(plain[8], 10);
    CHECK_EQ(plain[9], 0);
}

TEST(downlink_siren_on_off_duration_clamping_and_ack_clearing) {
    downlinkInit();
    uint16_t first = 0;
    uint32_t map = 0;

    // Invalid IDs rejected
    CHECK(!downlinkRequestSiren(nullptr, true, 60));
    CHECK(!downlinkRequestSiren("", true, 60));

    // Duration 0 defaults to 60 s; lost answer retries; ACK clears it
    REQUIRE(downlinkRequestSiren("2S5F554X", true, 0));
    CHECK_EQ(downlinkCommandForPoll("2S5F554X", 0, &first, &map), DOWNLINK_CMD_SIREN_ON);
    CHECK_EQ(first, 60);
    CHECK_EQ(downlinkCommandForPoll("2S5F554X", 0, &first, &map), DOWNLINK_CMD_SIREN_ON);
    CHECK_EQ(first, 60);
    CHECK_EQ(downlinkCommandForPoll("2S5F554X", DOWNLINK_STATE_ACK, &first, &map), 0);

    // Duration > 180 clamps to DOWNLINK_SIREN_MAX_SEC (180)
    REQUIRE(downlinkRequestSiren("2S5F554X", true, 250));
    CHECK_EQ(downlinkCommandForPoll("2S5F554X", 0, &first, &map), DOWNLINK_CMD_SIREN_ON);
    CHECK_EQ(first, DOWNLINK_SIREN_MAX_SEC);
    CHECK_EQ(downlinkCommandForPoll("2S5F554X", DOWNLINK_STATE_ACK, &first, &map), 0);

    // Leftover ACK holds OFF for one poll, then sends OFF, then ACK clears OFF
    REQUIRE(downlinkRequestSiren("2S5F554X", true, 60));
    CHECK_EQ(downlinkCommandForPoll("2S5F554X", 0, &first, &map), DOWNLINK_CMD_SIREN_ON);
    REQUIRE(downlinkRequestSiren("2S5F554X", false));
    CHECK_EQ(downlinkCommandForPoll("2S5F554X", DOWNLINK_STATE_ACK, &first, &map), 0);
    CHECK_EQ(downlinkCommandForPoll("2S5F554X", 0, &first, &map), DOWNLINK_CMD_SIREN_OFF);
    CHECK_EQ(first, 0);
    CHECK_EQ(downlinkCommandForPoll("2S5F554X", DOWNLINK_STATE_ACK, &first, &map), 0);
}

TEST(downlink_siren_tone_queueing_and_out9_encoding) {
    downlinkInit();
    uint16_t first = 0;
    uint32_t map = 0;
    uint8_t plain[16];

    // Tone 1 (ding_dong) with duration 0 preserves dur=0 so firmware chooses 1-shot duration
    REQUIRE(downlinkRequestSiren("2S5F554X", true, 0, 1));
    CHECK_EQ(downlinkCommandForPoll("2S5F554X", 0, &first, &map), DOWNLINK_CMD_SIREN_ON);
    CHECK_EQ((uint8_t)(first & 0xFFU), 0);
    CHECK_EQ((uint8_t)(first >> 8), 1);
    downlinkBuildPlain(1, false, DOWNLINK_CMD_SIREN_ON, (uint8_t)(first & 0xFFU), map, plain, 0,
                       (uint8_t)(first >> 8));
    CHECK_EQ(plain[8], 0);
    CHECK_EQ(plain[9], 1);
    CHECK_EQ(downlinkCommandForPoll("2S5F554X", DOWNLINK_STATE_ACK, &first, &map), 0);

    // Tone 5 (tone) with explicit duration 15 s
    REQUIRE(downlinkRequestSiren("2S5F554X", true, 15, 5));
    CHECK_EQ(downlinkCommandForPoll("2S5F554X", 0, &first, &map), DOWNLINK_CMD_SIREN_ON);
    CHECK_EQ((uint8_t)(first & 0xFFU), 15);
    CHECK_EQ((uint8_t)(first >> 8), 5);
    downlinkBuildPlain(2, true, DOWNLINK_CMD_SIREN_ON, (uint8_t)(first & 0xFFU), map, plain, 0,
                       (uint8_t)(first >> 8));
    CHECK_EQ(plain[8], 15);
    CHECK_EQ(plain[9], 5);
    CHECK_EQ(downlinkCommandForPoll("2S5F554X", DOWNLINK_STATE_ACK, &first, &map), 0);

    // Unknown tone (> 5) falls back to tone 0 (alarm) with default 60 s when duration == 0
    REQUIRE(downlinkRequestSiren("2S5F554X", true, 0, 99));
    CHECK_EQ(downlinkCommandForPoll("2S5F554X", 0, &first, &map), DOWNLINK_CMD_SIREN_ON);
    CHECK_EQ((uint8_t)(first & 0xFFU), 60);
    CHECK_EQ((uint8_t)(first >> 8), 0);
    downlinkBuildPlain(3, false, DOWNLINK_CMD_SIREN_ON, (uint8_t)(first & 0xFFU), map, plain, 0,
                       (uint8_t)(first >> 8));
    CHECK_EQ(plain[8], 60);
    CHECK_EQ(plain[9], 0);
}
