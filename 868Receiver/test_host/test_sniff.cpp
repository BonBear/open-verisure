/**
 * @file test_sniff.cpp
 * @brief Host unit tests for sniff_format.h (evaluation-only sniffer: line format, bounds, rate limit).
 */

#include "test.h"
#include "sniff_format.h"
#include <string.h>

TEST(sniff_format_normal_line) {
    const uint8_t d[4] = {0x00, 0xAB, 0x7F, 0xFF};
    char out[SNIFF_LINE_MAX];
    size_t n = sniffFormatLine(out, sizeof(out), 1, 0, d, 4, sizeof(d), true, false, -80, 45, -1.6f);
    CHECK_STR(out, "[Sniff#1] ch=0 len=4 crc56=ok crc24=bad rssi=-80 lqi=45 fe=-1.6 00 AB 7F FF");
    CHECK_EQ(n, strlen(out));
}

TEST(sniff_format_len_zero) {
    const uint8_t d[4] = {1, 2, 3, 4};
    char out[SNIFF_LINE_MAX];
    sniffFormatLine(out, sizeof(out), 2, 1, d, 0, sizeof(d), false, false, -100, 0, 0.0f);
    CHECK_STR(out, "[Sniff#2] ch=1 len=0 crc56=bad crc24=bad rssi=-100 lqi=0 fe=+0.0");
}

TEST(sniff_format_len_clamped_to_buffer) {
    // Guard bytes after the buffer must never be read or printed.
    uint8_t mem[8] = {0x11, 0x22, 0x33, 0x44, 0xEE, 0xEE, 0xEE, 0xEE};
    char out[SNIFF_LINE_MAX];
    sniffFormatLine(out, sizeof(out), 1, 0, mem, 255, 4, false, false, -70, 1, 0.0f);
    CHECK(strstr(out, "len=4 ") != nullptr);
    CHECK(strstr(out, "11 22 33 44") != nullptr);
    CHECK(strstr(out, "EE") == nullptr);
}

TEST(sniff_format_full_frame_fits_line_buffer) {
    uint8_t d[60];
    memset(d, 0xA5, sizeof(d));
    char out[SNIFF_LINE_MAX];
    size_t n = sniffFormatLine(out, sizeof(out), 2, 1, d, 58, sizeof(d), true, true, -138, 127, -203.2f);
    // Worst-case header plus all 58 bytes: a too-small SNIFF_LINE_MAX truncates and fails here.
    const char* header = "[Sniff#2] ch=1 len=58 crc56=ok crc24=ok rssi=-138 lqi=127 fe=-203.2";
    CHECK_EQ(n, strlen(header) + 58 * 3);
    CHECK_EQ(n, strlen(out));
    CHECK(strncmp(out, header, strlen(header)) == 0);
    CHECK(strstr(out, "len=58 ") != nullptr);
    CHECK_EQ(out[n - 1], '5');
    CHECK_EQ(out[n - 3], ' ');
}

TEST(sniff_format_small_output_buffer_truncates_safely) {
    uint8_t d[10];
    memset(d, 0xCD, sizeof(d));
    for (size_t sz = 1; sz < 120; sz++) {
        char guarded[130];
        memset(guarded, 'X', sizeof(guarded));
        size_t n = sniffFormatLine(guarded, sz, 1, 0, d, 10, sizeof(d), true, true, -70, 9, 1.0f);
        CHECK(n < sz);
        CHECK_EQ(strlen(guarded), n);
        CHECK_EQ(guarded[sz], 'X');   // nothing written past outSize
    }
    CHECK_EQ(sniffFormatLine(nullptr, 10, 1, 0, d, 1, 1, true, true, 0, 0, 0.0f), 0);
    char one[1] = {'Z'};
    CHECK_EQ(sniffFormatLine(one, 0, 1, 0, d, 1, 1, true, true, 0, 0, 0.0f), 0);
    CHECK_EQ(one[0], 'Z');
}

TEST(sniff_format_null_data) {
    char out[SNIFF_LINE_MAX];
    sniffFormatLine(out, sizeof(out), 1, 0, nullptr, 10, 10, true, true, -70, 1, 0.0f);
    CHECK(strstr(out, "len=0 ") != nullptr);
}

TEST(sniff_limiter_caps_and_reports_once) {
    SniffLimiter l;
    uint32_t rep = 99;
    int printed = 0;
    for (int i = 0; i < 50; i++) {
        if (sniffAllow(l, 1000 + (uint32_t)i, &rep)) printed++;
    }
    CHECK_EQ(printed, SNIFF_MAX_LINES_PER_SEC);
    // first call of the next window reports the 30 refused lines, and is itself allowed
    CHECK(sniffAllow(l, 2000, &rep));
    CHECK_EQ(rep, 30U);
    CHECK(sniffAllow(l, 2001, &rep));
    CHECK_EQ(rep, 0U);
}

TEST(sniff_limiter_millis_wrap) {
    SniffLimiter l;
    uint32_t rep;
    CHECK(sniffAllow(l, 0xFFFFFF00U, &rep));
    // 1 + 20 calls in the window: the 21st is refused, and so is the next one
    for (int i = 0; i < SNIFF_MAX_LINES_PER_SEC; i++) sniffAllow(l, 0xFFFFFF10U, &rep);
    CHECK(!sniffAllow(l, 0xFFFFFF20U, &rep));
    CHECK(!sniffAllow(l, 0x000002E0U, &rep));  // 992 ms after the window start, across the wrap
    CHECK(sniffAllow(l, 0x000003F0U, &rep));   // 1264 ms after the window start: new window
    CHECK_EQ(rep, 3U);
}
