/**
 * @file jpeg_splice.cpp
 * @brief Restart-interval splicing for MD CAM pictures, see jpeg_splice.h.
 */
#include "jpeg_splice.h"
#include <string.h>

static uint32_t s_segOff[JPEG_SPLICE_MAX_SEGS];
static uint16_t s_segLen[JPEG_SPLICE_MAX_SEGS];
static const uint8_t* s_repPtr[JPEG_SPLICE_MAX_SEGS];
static uint16_t s_repLen[JPEG_SPLICE_MAX_SEGS];

// Length of the header up to and including the SOS segment, 0 if not found.
static size_t headerLen(const uint8_t* j, size_t len) {
    size_t i = 2;
    if (len < 4 || j[0] != 0xFF || j[1] != 0xD8) return 0;
    while (i + 4 <= len) {
        if (j[i] != 0xFF) return 0;
        uint8_t m = j[i + 1];
        size_t l = ((size_t)j[i + 2] << 8) | j[i + 3];
        if (m == 0xD9 || l < 2) return 0;
        if (m == 0xDA) return i + 2 + l <= len ? i + 2 + l : 0;
        i += 2 + l;
    }
    return 0;
}

int jpegCommentId(const uint8_t* j, size_t len) {
    size_t i = 2;
    if (len < 4 || j[0] != 0xFF || j[1] != 0xD8) return -1;
    while (i + 4 <= len) {
        if (j[i] != 0xFF) return -1;
        uint8_t m = j[i + 1];
        size_t l = ((size_t)j[i + 2] << 8) | j[i + 3];
        if (m == 0xDA || m == 0xD9 || l < 2) return -1;
        if (m == 0xFE && l == 4 && i + 6 <= len && j[i + 4] == 'I') return j[i + 5];
        i += 2 + l;
    }
    return -1;
}

// Split the entropy-coded data into restart intervals; returns their number (0 on error).
static unsigned int splitSegments(const uint8_t* j, size_t len, size_t hdr) {
    unsigned int n = 0;
    size_t start = hdr, i = hdr;
    while (i + 1 < len) {
        if (j[i] == 0xFF && j[i + 1] >= 0xD0 && j[i + 1] <= 0xD9) {
            if (n >= JPEG_SPLICE_MAX_SEGS || i - start > 0xFFFF) return 0;
            s_segOff[n] = (uint32_t)start;
            s_segLen[n] = (uint16_t)(i - start);
            n++;
            if (j[i + 1] == 0xD9) return n;
            i += 2;
            start = i;
            continue;
        }
        i++;
    }
    if (start < len && n < JPEG_SPLICE_MAX_SEGS && len - start <= 0xFFFF) {   // no EOI (truncated)
        s_segOff[n] = (uint32_t)start;
        s_segLen[n] = (uint16_t)(len - start);
        n++;
    }
    return n;
}

bool jpegIsDelta(const uint8_t* d, size_t len) {
    return len >= 7 && d[0] == 'D' && d[1] == '1';
}

int jpegDeltaId(const uint8_t* d, size_t len) {
    return jpegIsDelta(d, len) ? d[2] : -1;
}

size_t jpegApplyDelta(const uint8_t* base, size_t baseLen, const uint8_t* d, size_t dLen,
                      uint8_t* out, size_t outMax, unsigned int* updated) {
    if (updated) *updated = 0;
    if (!jpegIsDelta(d, dLen)) return 0;
    unsigned int total = d[3] | ((unsigned int)d[4] << 8);
    unsigned int count = d[5] | ((unsigned int)d[6] << 8);
    size_t hdr = headerLen(base, baseLen);
    if (!hdr || total == 0 || total > JPEG_SPLICE_MAX_SEGS) return 0;
    if (splitSegments(base, baseLen, hdr) != total) return 0;

    for (unsigned int k = 0; k < total; k++) s_repPtr[k] = nullptr;
    size_t p = 7;
    unsigned int done = 0;
    for (unsigned int e = 0; e < count; e++) {
        if (p + 4 > dLen) break;
        unsigned int idx = d[p] | ((unsigned int)d[p + 1] << 8);
        unsigned int l = d[p + 2] | ((unsigned int)d[p + 3] << 8);
        if (idx >= total || l == 0 || p + 4 + l > dLen) break;
        s_repPtr[idx] = d + p + 4;
        s_repLen[idx] = (uint16_t)l;
        p += 4 + l;
        done++;
    }

    size_t o = 0;
    if (hdr > outMax) return 0;
    memcpy(out, base, hdr);
    o = hdr;
    for (unsigned int k = 0; k < total; k++) {
        const uint8_t* sp = s_repPtr[k] ? s_repPtr[k] : base + s_segOff[k];
        size_t sl = s_repPtr[k] ? s_repLen[k] : s_segLen[k];
        if (o + sl + 2 > outMax) return 0;
        memcpy(out + o, sp, sl);
        o += sl;
        out[o++] = 0xFF;
        out[o++] = (k + 1 < total) ? (uint8_t)(0xD0 + (k & 7)) : 0xD9;
    }
    if (updated) *updated = done;
    return o;
}
