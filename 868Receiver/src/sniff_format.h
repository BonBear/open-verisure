/**
 * @file sniff_format.h
 * @brief Evaluation-only packet sniffer helpers (pure, header-only, no Arduino dependency so the
 *        host tests can build it). main.cpp includes it only under RADIO_EVAL; nothing here is
 *        referenced by the release build.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum SniffMode : uint8_t { SNIFF_OFF = 0, SNIFF_UNKNOWN = 1, SNIFF_ALL = 2 };

/** Longest line sniffFormatLine() can produce for a RX_PKT_LEN (58) byte frame, with margin. */
#define SNIFF_LINE_MAX 320

/** Dumped lines per 1000 ms window; further lines are counted and reported once per window. */
#define SNIFF_MAX_LINES_PER_SEC 20

/**
 * Format one dump line into out (always NUL-terminated when outSize > 0):
 *   [Sniff#<module>] ch=<channel> len=<n> crc56=<ok|bad> crc24=<ok|bad> rssi=<dBm> lqi=<n> fe=<kHz> <hex>
 * n is the number of bytes actually dumped: min(len, bufSize). The bytes are read only below n, so a
 * len larger than the buffer (or 0) is safe. If out is too small the line is cut, never overrun.
 * Returns the length written (strlen(out)).
 */
static inline size_t sniffFormatLine(char* out, size_t outSize, unsigned module, unsigned channel,
                                     const uint8_t* data, size_t len, size_t bufSize,
                                     bool crc56Ok, bool crc24Ok, int rssiDbm, unsigned lqi,
                                     float feKhz) {
    if (!out || outSize == 0) return 0;
    out[0] = '\0';
    size_t n = (len < bufSize) ? len : bufSize;
    if (!data) n = 0;
    int w = snprintf(out, outSize, "[Sniff#%u] ch=%u len=%u crc56=%s crc24=%s rssi=%d lqi=%u fe=%+.1f",
                     module, channel, (unsigned)n, crc56Ok ? "ok" : "bad", crc24Ok ? "ok" : "bad",
                     rssiDbm, lqi, (double)feKhz);
    if (w < 0) { out[0] = '\0'; return 0; }
    size_t pos = (size_t)w;
    if (pos >= outSize) return outSize - 1;   // snprintf truncated (NUL already in place)
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        if (pos + 3 >= outSize) break;        // room for ' ', 2 digits and the NUL
        out[pos++] = ' ';
        out[pos++] = hex[data[i] >> 4];
        out[pos++] = hex[data[i] & 0x0F];
    }
    out[pos] = '\0';
    return pos;
}

/** At most SNIFF_MAX_LINES_PER_SEC lines per 1000 ms window; counts the lines it refused. */
struct SniffLimiter {
    uint32_t windowStartMs = 0;
    uint16_t used = 0;
    uint32_t suppressed = 0;
    bool     started = false;
};

/**
 * Call once per candidate line. Returns true when the line may be printed. *reportSuppressed is set to
 * the number of lines refused in the window that just ended (0 = nothing to report); the caller prints
 * it once. Uses unsigned time differences, so a millis() wrap is harmless.
 */
static inline bool sniffAllow(SniffLimiter& l, uint32_t nowMs, uint32_t* reportSuppressed) {
    *reportSuppressed = 0;
    if (!l.started || (uint32_t)(nowMs - l.windowStartMs) >= 1000U) {
        *reportSuppressed = l.suppressed;
        l.started = true;
        l.windowStartMs = nowMs;
        l.used = 0;
        l.suppressed = 0;
    }
    if (l.used < SNIFF_MAX_LINES_PER_SEC) { l.used++; return true; }
    if (l.suppressed < 0xFFFFFFFFU) l.suppressed++;
    return false;
}
