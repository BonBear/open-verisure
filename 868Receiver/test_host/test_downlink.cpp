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
