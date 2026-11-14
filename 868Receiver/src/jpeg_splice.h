/**
 * @file jpeg_splice.h
 * @brief Restart-interval splicing for MD CAM pictures (no Arduino dependencies).
 *
 * The MD CAM sends a full 320x240 JPEG with a restart marker after every MCU and a COM
 * marker 'I' <id>, and afterwards delta blobs that carry only the MCUs that changed:
 *   'D' '1' id  mcus_lo mcus_hi  count_lo count_hi  then per MCU:
 *   idx_lo idx_hi  len_lo len_hi  <len bytes of entropy-coded data, one restart interval>
 * jpegApplyDelta() replaces those restart intervals in the full picture and writes a new,
 * valid JPEG, without decoding anything.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define JPEG_SPLICE_MAX_SEGS 1200

/** Id from the COM marker 'I' <id> before the scan, or -1. */
int jpegCommentId(const uint8_t* j, size_t len);

/** True if the buffer is a delta blob ('D' '1'). */
bool jpegIsDelta(const uint8_t* d, size_t len);

/** Base picture id a delta blob refers to, or -1. */
int jpegDeltaId(const uint8_t* d, size_t len);

/**
 * Apply delta blob d to full picture base; writes the new JPEG to out (outMax bytes).
 * Returns the new length, or 0 if the base cannot be split into the delta's number of
 * restart intervals or out is too small. *updated = MCUs replaced (entries that do not
 * fit the blob, e.g. after a lost radio packet, stop the parsing).
 */
size_t jpegApplyDelta(const uint8_t* base, size_t baseLen, const uint8_t* d, size_t dLen,
                      uint8_t* out, size_t outMax, unsigned int* updated);
