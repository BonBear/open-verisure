/**
 * @file image_receiver.cpp
 * @brief RF Camera Image Chunk Reassembly & MQTT Publishing Implementation
 */

#include "image_receiver.h"
#include "mqtt_manager.h"
#include <string.h>
#include <esp_heap_caps.h>
#include "jpeg_splice.h"
#include "downlink.h"

/* --------------------------- JPEG chunks (type 0x82) ---------------------------
 * Chunk n carries bytes 40n .. 40n+len-1 of the JPEG (40-byte chunks in 56-byte packets), the
 * last one shorter. Chunks are placed by number, so a lost packet only leaves an 8-byte
 * hole (zeros); the sender puts a restart marker after every MCU (or MCU row), so a hole
 * only damages that part. A blob is finished by its last chunk, or by the first chunk of
 * the next blob. Full 320x240 pictures (COM id) become the base that later delta blobs
 * (only the changed MCUs, see jpeg_splice.h) are spliced into; the current picture is
 * published on MQTT and served on /api/camera.jpg. Pictures wider than 320 (640x480) are
 * kept apart for /api/camera_large.jpg. */
#define JPEG_CHUNK 40        // data bytes per picture packet (type 0x82/0x83)
#define JPEG_MAX_CHUNKS ((MAX_IMAGE_SIZE + JPEG_CHUNK - 1) / JPEG_CHUNK)
#define JPEG_QUIET_MS 5000   // the camera sends chunks back to back: a pause this long ends a blob

static uint8_t* s_imgBuf = nullptr;          // picture being received
static uint8_t* s_jpg = nullptr;             // last finished picture
static uint8_t s_have[(JPEG_MAX_CHUNKS + 7) / 8];  // bit per chunk received
static size_t s_imgEnd = 0;                  // bytes up to the highest chunk seen
static uint16_t s_haveCount = 0;
static int32_t s_lastIdx = -1;
static size_t s_jpgLen = 0;
static uint16_t s_jpgMissing = 0;
static uint8_t* s_jpgLarge = nullptr;        // last finished picture wider than 320
static size_t s_jpgLargeLen = 0;
static uint16_t s_jpgLargeMissing = 0;
// grayscale picture without COM id (a flash picture in the dark): kept apart from the colour
// base so the next delta still applies, and served by /api/camera.jpg while it is the newest
#define GRAY_MAX 16384
static uint8_t* s_jpgGray = nullptr;
static size_t s_jpgGrayLen = 0;
static uint16_t s_jpgGrayMissing = 0;
static bool s_grayNewest = false;

// Resend of missing chunks (56-byte packets only): a full picture with holes (or a lost end)
// is held for publishing (see publishHeld) and copied to s_rsImg; with its next poll answer the gateway asks
// the camera for the missing chunks among the 32 from the first missing one (a bitmap). The
// camera sends them as type 0x83, they are patched into s_rsImg, and the next ones are asked for, until the picture is
// complete: then it goes through jpegFinish() again (stored like a picture without holes; it
// replaces the held first pass and is published after the next poll answer). If the resend ends
// without completing, the first pass is published instead.
// Limits: RS_MAX_ROUNDS requests, RS_TIMEOUT_MS without an answer, RS_TOTAL_MS.
#define RS_MAX_ROUNDS 40
#define RS_TIMEOUT_MS 20000
#define RS_TOTAL_MS 60000
static bool s_rsActive = false;
static char s_rsDevice[10];
static uint8_t* s_rsImg = nullptr;   // the picture being completed
static unsigned long s_rsStart;
static size_t s_rsLen;               // data length known so far (without an added EOI)
static bool s_rsEndSeen;
static uint16_t s_rsChunks;          // chunks of the data known so far
static uint8_t s_rsHave[(JPEG_MAX_CHUNKS + 7) / 8];
static uint8_t s_rsRounds;
static uint16_t s_rsReqEnd;          // last chunk of the current request
static unsigned long s_rsTime;

// test (serial command "drop <n>"): drop every n-th 56-byte picture chunk, 0 = off
static uint32_t s_dropEvery = 0, s_dropCount = 0;
static bool s_noResend = false;      // finishing because the next picture started: no resend
void imageReceiverSetDrop(uint32_t n) { s_dropEvery = n; s_dropCount = 0; }
static uint8_t* s_tmp = nullptr;             // splice output, swapped with s_jpg
static int s_baseId = -1;                    // COM id of the full picture deltas apply to
static uint16_t s_deltaCount = 0;            // deltas applied since the last full picture
static char s_activeDeviceId[10] = {0};
static unsigned long s_lastChunkTime = 0;

// Deferred publish: publishing a picture (up to ~35 KB over MQTT) blocks for 100+ ms, and the
// camera polls right after its last chunk and listens for the answer for ~0.1 s. So jpegFinish()
// only stores the picture and marks it here; it is published after that camera's poll answer
// (imageReceiverAfterPollAnswer), or from imageReceiverLoop() IMG_PUBLISH_DEFER_MS later if no poll
// comes. One slot for now (the gateway has one picture state); a per-camera array later.
// buf points to a buffer that stays unchanged until the publish (s_jpg, s_jpgLarge, s_jpgGray or
// s_pubImg, never s_imgBuf which jpegReset() clears): every jpegFinish() first flushes the slot.
struct PendingPublish {
    bool active;
    char deviceId[10];
    const uint8_t* buf;
    size_t len;
    unsigned long since;
};
static PendingPublish s_pub = {false, {0}, nullptr, 0, 0};
static uint8_t* s_pubImg = nullptr;          // copy of a picture that is not stored anywhere else
static bool s_resendDone = false;            // jpegFinish() completes a picture by resend

static uint8_t* allocBig(size_t n) {
    uint8_t* p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = (uint8_t*)malloc(n);
    return p;
}

static void publishPending(void) {
    if (!s_pub.active) return;
    s_pub.active = false;
    if (!mqttPublishCameraImage(s_pub.deviceId, s_pub.buf, s_pub.len))
        Serial.printf("[ImageReceiver] Failed to publish image to MQTT broker.\n");
}

// A picture with holes is held while its resend runs: the camera resends and polls again ~60-80 ms
// after the answer that asked for the chunks, and a blocking publish in between would make that
// poll miss its answer deadline. When the resend completes, the complete picture replaces it (see
// jpegFinish); when it ends without completing (rsCancel), the next imageReceiverLoop() or poll
// hook publishes the first pass.
static bool publishHeld(void) {
    return s_pub.active && s_rsActive && strcmp(s_rsDevice, s_pub.deviceId) == 0;
}

// Mark the picture for publishing. stable: where it is stored (s_jpg, s_jpgLarge, s_jpgGray), or
// nullptr to copy len bytes of s_imgBuf to s_pubImg (a picture without frame header, or too
// big for the gray buffer). Without a buffer for the copy it is published at once.
static void queuePublish(const uint8_t* stable, size_t len) {
    if (!stable) {
        if (!s_pubImg) s_pubImg = allocBig(MAX_IMAGE_SIZE);
        if (!s_pubImg) {
            if (!mqttPublishCameraImage(s_activeDeviceId, s_imgBuf, len))
                Serial.printf("[ImageReceiver] Failed to publish image to MQTT broker.\n");
            return;
        }
        memcpy(s_pubImg, s_imgBuf, len);
        stable = s_pubImg;
    }
    strncpy(s_pub.deviceId, s_activeDeviceId, 9);
    s_pub.deviceId[9] = '\0';
    s_pub.buf = stable;
    s_pub.len = len;
    s_pub.since = millis();
    s_pub.active = true;
}

static void jpegFinish(size_t len, bool endSeen = true);

static void jpegReset(void) {
    if (s_imgBuf) memset(s_imgBuf, 0, MAX_IMAGE_SIZE);
    memset(s_have, 0, sizeof(s_have));
    s_imgEnd = 0;
    s_haveCount = 0;
    s_lastIdx = -1;
}

// Picture width from the SOF0 marker (0 if not found).
static unsigned int jpegWidth(const uint8_t* j, size_t len) {
    for (size_t i = 2; i + 9 < len && i < 1024; i++)
        if (j[i] == 0xFF && j[i + 1] == 0xC0)
            return ((unsigned int)j[i + 7] << 8) | j[i + 8];
    return 0;
}

// A delta that cannot be applied (the gateway restarted, or missed the full picture it
// belongs to) is useless, and the camera keeps sending deltas against its own reference
// while the Blackfin stays powered. Ask it for a full picture (0xD1) with its next poll,
// at most once per BASE_REQUEST_MS so a camera that cannot deliver it is not flooded.
#define BASE_REQUEST_MS 60000UL
static void requestNewBase(void) {
    static unsigned long s_lastRequest = 0;
    static bool s_requested = false;
    if (s_requested && millis() - s_lastRequest < BASE_REQUEST_MS) return;
    s_requested = true;
    s_lastRequest = millis();
    bool ok = downlinkRequestPicture(s_activeDeviceId, 0xD1);
    Serial.printf("[ImageReceiver] No base for this delta: full picture 0xD1 for %s %s\n", s_activeDeviceId,
                  ok ? "queued" : "NOT queued");
}

static bool rsHave(uint16_t i) { return (s_rsHave[i >> 3] >> (i & 7)) & 1; }

// End the resend (completed or given up): also drop a request still queued in the downlink, so
// it does not go out later, ahead of a queued command, and ask for chunks of a picture the
// gateway no longer completes.
static void rsCancel(void) {
    if (s_rsActive) downlinkCancelResend(s_rsDevice);
    s_rsActive = false;
}

// Ask for the missing chunks among the 32 from the first missing one (a bitmap, so chunks the
// gateway already has are not sent again; with the end not seen yet the chunks after the
// known data count as missing), or finish.
static void rsRequestNext(void) {
    uint16_t first = 0xFFFF, last = 0, i;
    uint32_t map = 0;
    for (i = 0; i < s_rsChunks; i++) {
        if (!rsHave(i)) {
            first = i;
            break;
        }
    }
    if (first != 0xFFFF || !s_rsEndSeen) {
        if (first == 0xFFFF) first = s_rsChunks;   // the end is missing: ask for what follows
        for (i = 0; i < 32 && first + i < JPEG_MAX_CHUNKS; i++) {
            uint16_t c = first + i;
            if (c < s_rsChunks ? !rsHave(c) : !s_rsEndSeen) {
                map |= 1UL << i;
                last = c;
            }
        }
    }
    if (first == 0xFFFF) {               // complete: store and publish it as a normal picture
        Serial.printf("[ImageReceiver] Picture from %s complete after resend: %u bytes, %u round(s), %lu ms\n",
                      s_rsDevice, (unsigned int)s_rsLen, s_rsRounds, millis() - s_rsStart);
        rsCancel();
        if (s_lastIdx >= 0) return;      // a new picture is coming in (cannot happen: it cancels)
        memcpy(s_imgBuf, s_rsImg, s_rsLen);
        s_haveCount = s_rsChunks;
        strncpy(s_activeDeviceId, s_rsDevice, 9);
        s_noResend = true;
        s_resendDone = true;
        jpegFinish(s_rsLen, true);
        s_resendDone = false;
        s_noResend = false;
        return;
    }
    if (!map) {                          // nothing addressable (picture at JPEG_MAX_CHUNKS)
        rsCancel();
        return;
    }
    if (++s_rsRounds > RS_MAX_ROUNDS || millis() - s_rsStart > RS_TOTAL_MS) {
        Serial.printf("[ImageReceiver] Resend for %s stopped after %u rounds\n", s_rsDevice, s_rsRounds - 1);
        rsCancel();
        // released right after a resend batch: restart the defer time so the camera's next poll
        // (~30-40 ms later) is answered before this blocking publish
        if (s_pub.active) s_pub.since = millis();
        return;
    }
    s_rsReqEnd = last;
    s_rsTime = millis();
    bool ok = downlinkRequestResend(s_rsDevice, first, map);
    Serial.printf("[ImageReceiver] Resend of %d chunks in %u..%u from %s %s\n", __builtin_popcount(map), first, last,
                  s_rsDevice, ok ? "queued" : "NOT queued");
}

void imageReceiverProcessResend(const char* deviceId, uint16_t chunkIdx, uint8_t chunkLen, bool isLast,
                                const uint8_t* data) {
    if (!s_rsActive || !deviceId || !data || strcmp(deviceId, s_rsDevice) != 0 || chunkLen == 0 ||
        chunkLen > JPEG_CHUNK || chunkIdx >= JPEG_MAX_CHUNKS) return;
    size_t off = (size_t)chunkIdx * JPEG_CHUNK;
    if (off + chunkLen > MAX_IMAGE_SIZE) return;
    memcpy(s_rsImg + off, data, chunkLen);
    s_rsHave[chunkIdx >> 3] |= (uint8_t)(1 << (chunkIdx & 7));
    if (isLast) {
        s_rsLen = off + chunkLen;
        s_rsEndSeen = true;
        s_rsChunks = chunkIdx + 1;
    } else if (off + chunkLen > s_rsLen) {
        s_rsLen = off + chunkLen;
        if (chunkIdx + 1 > s_rsChunks) s_rsChunks = chunkIdx + 1;
    }
    s_rsTime = millis();
    if (chunkIdx >= s_rsReqEnd || isLast) rsRequestNext();
}

// Finish the blob in s_imgBuf (len bytes; missing chunks stay zero): a full JPEG (kept as
// the base for deltas if it is 320 wide, or as the large picture) or a delta blob that is
// spliced into the base (jpeg_splice.cpp). The resulting picture is marked for publishing on
// MQTT (queuePublish: published after the camera's poll answer, see PendingPublish).
// endSeen false: the last chunk never came (the blob ended by a pause or the next blob), so
// the missing count cannot include the lost end; a JPEG gets its EOI back so it decodes.
static void jpegFinish(size_t len, bool endSeen) {
    if (len < 4 || !s_jpg || !s_tmp) {
        jpegReset();
        return;
    }
    // A picture still waiting to be published must go out before s_jpg & co are overwritten, or it
    // would be dropped. Exception: the same picture completed by resend replaces its first pass
    // (holes), so only the complete one is published (one blocking publish instead of two).
    if (s_pub.active) {
        if (s_resendDone && strcmp(s_pub.deviceId, s_activeDeviceId) == 0)
            s_pub.active = false;
        else
            publishPending();
    }
    uint16_t chunks = (len + JPEG_CHUNK - 1) / JPEG_CHUNK;
    uint16_t missing = chunks > s_haveCount ? chunks - s_haveCount : 0;
    const char* endNote = endSeen ? "" : " (end lost)";
    size_t dataLen = len;
    if (!endSeen && !jpegIsDelta(s_imgBuf, len) && len + 2 <= MAX_IMAGE_SIZE &&
        !(s_imgBuf[len - 2] == 0xFF && s_imgBuf[len - 1] == 0xD9)) {
        s_imgBuf[len++] = 0xFF;
        s_imgBuf[len++] = 0xD9;
    }
    if (jpegIsDelta(s_imgBuf, len)) {
        int id = jpegDeltaId(s_imgBuf, len);
        unsigned int updated = 0;
        size_t n = 0;
        if (s_jpgLen && id == s_baseId)
            n = jpegApplyDelta(s_jpg, s_jpgLen, s_imgBuf, len, s_tmp, MAX_IMAGE_SIZE, &updated);
        Serial.printf("[ImageReceiver] Delta from %s: %u bytes for picture %d, %u blocks%s, %u/%u chunks missing%s\n",
                      s_activeDeviceId, (unsigned int)len, id, updated,
                      n ? "" : (id == s_baseId ? " (could not apply)" : " (other base, ignored)"), missing, chunks, endNote);
        if (!n)
            requestNewBase();
        if (n) {
            uint8_t* t = s_jpg;          // the spliced picture becomes the current one
            s_jpg = s_tmp;
            s_tmp = t;
            s_jpgLen = n;
            s_deltaCount++;
            if (updated) s_grayNewest = false;
            if (updated) queuePublish(s_jpg, n);
        }
        jpegReset();
        return;
    }
    bool soi = s_imgBuf[0] == 0xFF && s_imgBuf[1] == 0xD8;
    unsigned int w = jpegWidth(s_imgBuf, len);
    const uint8_t* stable = nullptr;                // where the picture is kept for the publish
    if (w == 0) {
        // no frame header (lost with a chunk): published, but neither stored as the base
        // nor as the large picture until a resend completes it
    } else if (w > 320) {                           // large (640x480) picture: kept apart, no base
        if (s_jpgLarge) {
            memcpy(s_jpgLarge, s_imgBuf, len);
            s_jpgLargeLen = len;
            s_jpgLargeMissing = missing;
            stable = s_jpgLarge;
        }
    } else if (s_baseId >= 0 && jpegCommentId(s_imgBuf, len) < 0) {
        // no COM id (grayscale, e.g. a flash picture 0xD4): published below, but the colour
        // picture stays the base for the camera's next delta
        if (!s_jpgGray) s_jpgGray = allocBig(GRAY_MAX);
        if (s_jpgGray && len <= GRAY_MAX) {
            memcpy(s_jpgGray, s_imgBuf, len);
            s_jpgGrayLen = len;
            s_jpgGrayMissing = missing;
            s_grayNewest = true;
            stable = s_jpgGray;
        }
    } else {
        s_grayNewest = false;
        memcpy(s_jpg, s_imgBuf, len);
        s_jpgLen = len;
        s_jpgMissing = missing;
        s_baseId = jpegCommentId(s_jpg, len);
        s_deltaCount = 0;
        stable = s_jpg;
    }
    Serial.printf("[ImageReceiver] JPEG from %s: %u bytes, width %u, id %d, %u/%u chunks missing%s%s\n", s_activeDeviceId,
                  (unsigned int)len, w, jpegCommentId(s_imgBuf, len), missing, chunks, soi ? "" : " (no SOI)", endNote);
    queuePublish(stable, len);
    if (!s_noResend && (missing || !endSeen)) {
        if (!s_rsImg) s_rsImg = allocBig(MAX_IMAGE_SIZE);
    }
    if (s_rsImg && !s_noResend && (missing || !endSeen)) {
        s_rsActive = true;
        strncpy(s_rsDevice, s_activeDeviceId, 9);
        s_rsDevice[9] = '\0';
        memcpy(s_rsImg, s_imgBuf, dataLen);
        s_rsStart = millis();
        s_rsLen = dataLen;
        s_rsEndSeen = endSeen;
        s_rsChunks = (dataLen + JPEG_CHUNK - 1) / JPEG_CHUNK;
        memcpy(s_rsHave, s_have, sizeof(s_rsHave));
        s_rsRounds = 0;
        rsRequestNext();
    }
    jpegReset();
}

void imageReceiverPollSeen(const char* deviceId) {
    // the camera polls right after a picture: a picture still open lost its last chunk; finish
    // it now so a resend request can go out with this poll's answer (the Blackfin is still on)
    if (deviceId && s_lastIdx >= 0 && strcmp(deviceId, s_activeDeviceId) == 0) jpegFinish(s_imgEnd, false);
    // a resend round whose last chunk was lost never asks for the next one (that happens when
    // the last requested chunk arrives): ask again for what is still missing with this poll's
    // answer. The camera polls every ~2 s until an answer without a resend request.
    if (deviceId && s_rsActive && strcmp(deviceId, s_rsDevice) == 0 && !downlinkResendPending(deviceId))
        rsRequestNext();
}

void imageReceiverPublishPending(void) { publishPending(); }

void imageReceiverAfterPollAnswer(const char* deviceId) {
    if (deviceId && s_pub.active && strcmp(deviceId, s_pub.deviceId) == 0 && !publishHeld()) publishPending();
}

void imageReceiverInit(void) {
    s_activeDeviceId[0] = '\0';
    s_lastChunkTime = 0;
    jpegReset();
}

void imageReceiverProcessChunk(const char* deviceId, uint16_t chunkIdx, uint8_t chunkLen,
                               bool isFirst, bool isLast, const uint8_t* data) {
    if (!deviceId || !data || chunkLen == 0 || chunkLen > JPEG_CHUNK || chunkIdx >= JPEG_MAX_CHUNKS ||
        (size_t)chunkIdx * JPEG_CHUNK + chunkLen > MAX_IMAGE_SIZE) return;
    rsCancel();                           // a new picture: the stored one may change (deltas)
    if (s_dropEvery && ++s_dropCount % s_dropEvery == 0) return;   // test
    if (!s_imgBuf) {
        s_imgBuf = allocBig(MAX_IMAGE_SIZE);
        s_jpg = allocBig(MAX_IMAGE_SIZE);
        s_jpgLarge = allocBig(MAX_IMAGE_SIZE);
        s_tmp = allocBig(MAX_IMAGE_SIZE);
        if (!s_imgBuf || !s_jpg) return;
        jpegReset();
    }

    unsigned long now = millis();
    bool otherDevice = strcmp(s_activeDeviceId, deviceId) != 0;
    bool stale = s_lastIdx >= 0 && now - s_lastChunkTime > 15000;
    bool wrapped = s_lastIdx >= 0 && (int32_t)chunkIdx < s_lastIdx;   // next picture, first chunk lost
    if (otherDevice || stale) {
        jpegReset();
    } else if ((isFirst || wrapped) && s_lastIdx >= 0) {
        s_noResend = true;              // the camera's buffer already holds the next picture
        jpegFinish(s_imgEnd, false);    // previous picture lost its last chunk
        s_noResend = false;
    }
    if (otherDevice) {
        strncpy(s_activeDeviceId, deviceId, 9);
        s_activeDeviceId[9] = '\0';
    }
    s_lastChunkTime = now;

    size_t off = (size_t)chunkIdx * JPEG_CHUNK;
    memcpy(s_imgBuf + off, data, chunkLen);
    uint8_t bit = 1 << (chunkIdx & 7);
    if (!(s_have[chunkIdx >> 3] & bit)) {
        s_have[chunkIdx >> 3] |= bit;
        s_haveCount++;
    }
    if (off + chunkLen > s_imgEnd) s_imgEnd = off + chunkLen;
    s_lastIdx = chunkIdx;

    if (isLast) jpegFinish(off + chunkLen);
}

const uint8_t* imageReceiverJpeg(size_t* len, uint16_t* missing) {
    if (s_grayNewest && s_jpgGrayLen) {
        if (len) *len = s_jpgGrayLen;
        if (missing) *missing = s_jpgGrayMissing;
        return s_jpgGray;
    }
    if (len) *len = s_jpgLen;
    if (missing) *missing = s_jpgMissing;
    return s_jpgLen ? s_jpg : nullptr;
}

const uint8_t* imageReceiverJpegLarge(size_t* len, uint16_t* missing) {
    if (len) *len = s_jpgLargeLen;
    if (missing) *missing = s_jpgLargeMissing;
    return s_jpgLargeLen ? s_jpgLarge : nullptr;
}

void imageReceiverLoop(void) {
    unsigned long now = millis();
    // a JPEG/delta whose last chunk was lost: finish it after a pause instead of dropping it
    // when the next picture comes more than 15 s later (stale)
    if (s_lastIdx >= 0 && now - s_lastChunkTime > JPEG_QUIET_MS) jpegFinish(s_imgEnd, false);
    // millis() again: jpegFinish() above may just have started a resend (s_rsTime > now), and
    // now - s_rsTime would wrap and give it up at once (seen when a picture's end was lost)
    if (s_rsActive && millis() - s_rsTime > RS_TIMEOUT_MS) {
        Serial.printf("[ImageReceiver] Resend for %s given up (no answer)\n", s_rsDevice);
        rsCancel();
    }
    // a picture no poll followed (or whose poll was not seen): publish it after the defer time
    if (s_pub.active && millis() - s_pub.since >= IMG_PUBLISH_DEFER_MS && !publishHeld()) publishPending();
}

#ifdef HOST_TEST
/* Host unit tests only (868Receiver/test_host), never in the firmware: forget the pictures and the
 * reassembly and resend state as a gateway restart does (the buffers stay allocated). */
void imageReceiverHostTestRestart() {
    jpegReset();
    s_jpgLen = s_jpgLargeLen = s_jpgGrayLen = 0;
    s_jpgMissing = s_jpgLargeMissing = s_jpgGrayMissing = 0;
    s_grayNewest = false;
    s_rsActive = false;
    s_pub.active = false;
    s_resendDone = false;
    s_dropEvery = s_dropCount = 0;
    s_noResend = false;
    s_baseId = -1;
    s_deltaCount = 0;
    s_activeDeviceId[0] = '\0';
    s_lastChunkTime = 0;
}
#endif
