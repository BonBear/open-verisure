/**
 * @file image_receiver.h
 * @brief RF Camera Image Chunk Reassembly for Verisure Sub-1GHz Sensor
 */

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_IMAGE_SIZE 65536    /* 640x480 colour JPEG ~20 KB */

void imageReceiverInit(void);

/**
 * @brief Process an incoming authenticated image chunk from RF.
 * @param deviceId 8-char sensor ID.
 * @param chunkIdx 16-bit chunk index (0..N); chunk n holds bytes 40n.. of the JPEG.
 * @param chunkLen Number of bytes in this chunk (1..40).
 * @param isFirst True if this is the first chunk (SOI).
 * @param isLast True if this is the final chunk (EOI).
 * @param data Pointer to chunkLen bytes of JPEG data.
 * @param channel Radio channel (0 = main 868.95 MHz, 1 = PCH 869.525 MHz).
 */
void imageReceiverProcessChunk(const char* deviceId, uint16_t chunkIdx, uint8_t chunkLen,
                               bool isFirst, bool isLast, const uint8_t* data, uint8_t channel = 0);

/** Cameras with their own picture state (reassembly, delta base, resend, pending publish); keep
 *  equal to DOWNLINK_MAX_CAMERAS in downlink.h (static_assert in image_receiver.cpp). A camera gets a slot with its first chunk; when
 *  all are taken the idle slot quiet the longest is reused, and if none is idle the chunk is dropped. */
#define IMG_MAX_CAMERAS 4

/** Newest 320x240 picture of a camera: the colour picture (with deltas spliced in) or, if it came
 *  later, a grayscale flash picture (nullptr if none yet); missing = chunks that never arrived (zeros).
 *  deviceId: the camera's 8-char ID; nullptr = the camera whose picture changed last. */
const uint8_t* imageReceiverJpeg(size_t* len, uint16_t* missing, const char* deviceId = nullptr);

/** Last finished JPEG wider than 320 pixels (640x480) of a camera, nullptr if none yet
 *  (deviceId nullptr = the camera whose large picture came last). */
const uint8_t* imageReceiverJpegLarge(size_t* len, uint16_t* missing, const char* deviceId = nullptr);

/** A chunk resent on the gateway's request (packet type 0x83): patched into the last picture;
 *  once complete it replaces the held first pass and is published after the next poll answer. */
void imageReceiverProcessResend(const char* deviceId, uint16_t chunkIdx, uint8_t chunkLen, bool isLast,
                                const uint8_t* data, uint8_t channel = 0);

/** A poll from this camera arrived: finish a picture that is still open (its end was lost). */
void imageReceiverPollSeen(const char* deviceId);

/** A finished picture is published on MQTT (a blocking call) only after its camera's poll answer,
 *  so the publish does not delay that answer; if no poll follows, imageReceiverLoop() publishes it
 *  once IMG_PUBLISH_DEFER_MS have passed (only while loop() runs, e.g. not during an OTA upload).
 *  A picture with holes is held while its resend runs and published when the resend completes
 *  (the complete picture replaces it) or ends without completing. */
#define IMG_PUBLISH_DEFER_MS 300

/** Call at the end of answering a poll from this camera (answer sent, skipped or failed):
 *  publishes the picture waiting for it, if any, unless it is held while its resend runs. */
void imageReceiverAfterPollAnswer(const char* deviceId);

/** Publish every waiting picture now, oldest first, whatever device (used by tests). */
void imageReceiverPublishPending(void);

/** Test: drop every n-th 56-byte picture chunk (0 = off), to exercise the resend. */
void imageReceiverSetDrop(uint32_t n);

/** Call from loop(): finishes a picture whose last chunk was lost (after a pause). */
void imageReceiverLoop(void);

/** Record whether PCH was advertised in the last poll answer sent to this camera. */
void imageReceiverNotePollAnswer(const char* deviceId, bool pchAdvertised);

/** Note a 0xB2 picture start packet from this camera (explicit pchAdvertised flag). */
void imageReceiverNotePicStart(const char* deviceId, bool pchAdvertised);

/** Note a 0xB2 picture start packet using the last imageReceiverNotePollAnswer() state. */
void imageReceiverNotePicStart(const char* deviceId);

/** True while PCH advertisement is suppressed for this camera due to consecutive misses. */
bool imageReceiverPchSuppressed(const char* deviceId);

/** Clear PCH miss counter, backoff and suppression for one camera (or all if deviceId == nullptr). */
void imageReceiverPchReset(const char* deviceId = nullptr);

/** Print per-camera PCH/main chunk counts and suppression state to Serial (CLI 'pch status'). */
void imageReceiverPchPrintStatus(void);

struct PchCamStats {
    char deviceId[10];
    uint32_t pchChunks;
    uint32_t mainChunks;
    uint8_t misses;
    bool suppressed;
    uint32_t suppressRemSec;
};

/** Copy per-camera PCH stats into out (up to maxOut entries); returns number written. */
int imageReceiverPchGetStats(PchCamStats* out, int maxOut);

