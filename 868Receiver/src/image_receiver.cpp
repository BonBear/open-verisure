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

/* ------------------------------ JPEG chunks (type 0x80) ------------------------------
 * Chunk n carries bytes 8n .. 8n+len-1 of the JPEG (the MD CAM sends 8-byte chunks, the
 * last one shorter). Chunks are placed by number, so a lost packet only leaves an 8-byte
 * hole (zeros); the sender puts a restart marker after every MCU (or MCU row), so a hole
 * only damages that part. A blob is finished by its last chunk, or by the first chunk of
 * the next blob. Full 320x240 pictures (COM id) become the base that later delta blobs
 * (only the changed MCUs, see jpeg_splice.h) are spliced into; the current picture is
 * published on MQTT and served on /api/camera.jpg. Pictures wider than 320 (640x480) are
 * kept apart for /api/camera_large.jpg. */
#define JPEG_CHUNK 8
#define JPEG_MAX_CHUNKS (MAX_IMAGE_SIZE / JPEG_CHUNK)
#define JPEG_QUIET_MS 5000   // the camera sends chunks back to back: a pause this long ends a blob

static uint8_t* s_imgBuf = nullptr;          // picture being received
static uint8_t* s_jpg = nullptr;             // last finished picture
static uint8_t s_have[JPEG_MAX_CHUNKS / 8];  // bit per chunk received
static size_t s_imgEnd = 0;                  // bytes up to the highest chunk seen
static uint16_t s_haveCount = 0;
static int32_t s_lastIdx = -1;
static size_t s_jpgLen = 0;
static uint16_t s_jpgMissing = 0;
static uint8_t* s_jpgLarge = nullptr;        // last finished picture wider than 320
static size_t s_jpgLargeLen = 0;
static uint16_t s_jpgLargeMissing = 0;
static uint8_t* s_tmp = nullptr;             // splice output, swapped with s_jpg
static int s_baseId = -1;                    // COM id of the full picture deltas apply to
static uint16_t s_deltaCount = 0;            // deltas applied since the last full picture
static char s_activeDeviceId[10] = {0};
static unsigned long s_lastChunkTime = 0;

static uint8_t* allocBig(size_t n) {
    uint8_t* p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = (uint8_t*)malloc(n);
    return p;
}

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

// Finish the blob in s_imgBuf (len bytes; missing chunks stay zero): a full JPEG (kept as
// the base for deltas if it is 320 wide, or as the large picture) or a delta blob that is
// spliced into the base (jpeg_splice.cpp). The resulting picture is published on MQTT.
// endSeen false: the last chunk never came (the blob ended by a pause or the next blob), so
// the missing count cannot include the lost end; a JPEG gets its EOI back so it decodes.
static void jpegFinish(size_t len, bool endSeen = true) {
    if (len < 4 || !s_jpg || !s_tmp) {
        jpegReset();
        return;
    }
    uint16_t chunks = (len + JPEG_CHUNK - 1) / JPEG_CHUNK;
    uint16_t missing = chunks > s_haveCount ? chunks - s_haveCount : 0;
    const char* endNote = endSeen ? "" : " (end lost)";
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
            if (updated && !mqttPublishCameraImage(s_activeDeviceId, s_jpg, n))
                Serial.printf("[ImageReceiver] Failed to publish image to MQTT broker.\n");
        }
        jpegReset();
        return;
    }
    bool soi = s_imgBuf[0] == 0xFF && s_imgBuf[1] == 0xD8;
    unsigned int w = jpegWidth(s_imgBuf, len);
    if (w > 320) {                           // large (640x480) picture: kept apart, no base
        if (s_jpgLarge) {
            memcpy(s_jpgLarge, s_imgBuf, len);
            s_jpgLargeLen = len;
            s_jpgLargeMissing = missing;
        }
    } else if (s_baseId >= 0 && jpegCommentId(s_imgBuf, len) < 0) {
        // no COM id (grayscale, e.g. a flash picture 0xD4): published below, but the colour
        // picture stays the base for the camera's next delta
    } else {
        memcpy(s_jpg, s_imgBuf, len);
        s_jpgLen = len;
        s_jpgMissing = missing;
        s_baseId = jpegCommentId(s_jpg, len);
        s_deltaCount = 0;
    }
    Serial.printf("[ImageReceiver] JPEG from %s: %u bytes, width %u, id %d, %u/%u chunks missing%s%s\n", s_activeDeviceId,
                  (unsigned int)len, w, jpegCommentId(s_imgBuf, len), missing, chunks, soi ? "" : " (no SOI)", endNote);
    if (!mqttPublishCameraImage(s_activeDeviceId, s_imgBuf, len))
        Serial.printf("[ImageReceiver] Failed to publish image to MQTT broker.\n");
    jpegReset();
}

void imageReceiverInit(void) {
    s_activeDeviceId[0] = '\0';
    s_lastChunkTime = 0;
    jpegReset();
}

void imageReceiverProcessChunk(const char* deviceId, uint16_t chunkIdx, uint8_t chunkLen,
                               bool isFirst, bool isLast, const uint8_t* data) {
    if (!deviceId || !data || chunkLen == 0 || chunkLen > JPEG_CHUNK || chunkIdx >= JPEG_MAX_CHUNKS) return;
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
        jpegFinish(s_imgEnd, false);    // previous picture lost its last chunk
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
    if (len) *len = s_jpgLen;
    if (missing) *missing = s_jpgMissing;
    return s_jpgLen ? s_jpg : nullptr;
}

const uint8_t* imageReceiverJpegLarge(size_t* len, uint16_t* missing) {
    if (len) *len = s_jpgLargeLen;
    if (missing) *missing = s_jpgLargeMissing;
    return s_jpgLargeLen ? s_jpgLarge : nullptr;
}

/* ------------------------------ raw grayscale tiles ------------------------------ */

#define RAW_BMP_HDR (14 + 40 + 256 * 4)
#define RAW_BMP_SIZE (RAW_BMP_HDR + RAW_IMG_W * RAW_IMG_H)

static uint8_t* s_rawBmp = nullptr;          // BMP file: header + palette + pixels
static uint8_t s_rawHave[RAW_TILES / 8];     // bit per tile received
static uint16_t s_rawHaveCount = 0;
static uint16_t s_rawNewSincePublish = 0;
static unsigned long s_rawLastTile = 0;
static unsigned long s_rawLastPublish = 0;
static char s_rawDeviceId[10] = {0};

static void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static bool rawInit(void) {
    if (s_rawBmp) return true;
    s_rawBmp = (uint8_t*)heap_caps_malloc(RAW_BMP_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rawBmp) s_rawBmp = (uint8_t*)malloc(RAW_BMP_SIZE);
    if (!s_rawBmp) return false;
    memset(s_rawBmp, 0, RAW_BMP_SIZE);
    uint8_t* h = s_rawBmp;
    h[0] = 'B'; h[1] = 'M';
    put32(h + 2, RAW_BMP_SIZE);
    put32(h + 10, RAW_BMP_HDR);
    put32(h + 14, 40);
    put32(h + 18, RAW_IMG_W);
    put32(h + 22, RAW_IMG_H);                // bottom-up rows
    h[26] = 1;                               // planes
    h[28] = 8;                               // bits per pixel
    put32(h + 34, RAW_IMG_W * RAW_IMG_H);
    put32(h + 46, 256);
    for (int i = 0; i < 256; i++) {          // grayscale palette
        uint8_t* e = h + 54 + i * 4;
        e[0] = e[1] = e[2] = (uint8_t)i;
    }
    memset(s_rawHave, 0, sizeof(s_rawHave));
    s_rawHaveCount = 0;
    return true;
}

void imageReceiverProcessTile(const char* deviceId, uint16_t tile, const uint8_t* pix8) {
    if (!deviceId || !pix8 || tile >= RAW_TILES || !rawInit()) return;
    strncpy(s_rawDeviceId, deviceId, 9);
    s_rawDeviceId[9] = '\0';

    uint32_t p = (uint32_t)tile * RAW_TILE_PIX;
    uint32_t row = p / RAW_IMG_W, x = p % RAW_IMG_W;
    memcpy(s_rawBmp + RAW_BMP_HDR + (RAW_IMG_H - 1 - row) * RAW_IMG_W + x, pix8, RAW_TILE_PIX);

    uint8_t bit = 1 << (tile & 7);
    if (!(s_rawHave[tile >> 3] & bit)) {
        s_rawHave[tile >> 3] |= bit;
        s_rawHaveCount++;
        if (s_rawHaveCount == RAW_TILES)
            Serial.printf("[ImageReceiver] Raw picture complete (%u tiles) from %s\n", RAW_TILES, deviceId);
    }
    s_rawNewSincePublish++;
    s_rawLastTile = millis();
}

void imageReceiverLoop(void) {
    unsigned long now = millis();
    // a JPEG/delta whose last chunk was lost: finish it after a pause instead of dropping it
    // when the next picture comes more than 15 s later (stale)
    if (s_lastIdx >= 0 && now - s_lastChunkTime > JPEG_QUIET_MS) jpegFinish(s_imgEnd, false);
    if (!s_rawBmp || s_rawNewSincePublish == 0) return;
    bool complete = s_rawHaveCount == RAW_TILES;
    bool quiet = now - s_rawLastTile > 5000;                 // sender paused or finished
    bool periodic = now - s_rawLastPublish > 120000;         // progress while streaming
    if (!(quiet || periodic || (complete && now - s_rawLastPublish > 10000))) return;
    s_rawLastPublish = now;
    s_rawNewSincePublish = 0;
    Serial.printf("[ImageReceiver] Publishing raw picture (%u/%u tiles) from %s\n",
                  s_rawHaveCount, RAW_TILES, s_rawDeviceId);
    mqttPublishCameraImage(s_rawDeviceId, s_rawBmp, RAW_BMP_SIZE);
}

const uint8_t* imageReceiverBmp(size_t* len) {
    if (len) *len = s_rawBmp ? RAW_BMP_SIZE : 0;
    return s_rawBmp;
}

uint16_t imageReceiverTilesHave(void) { return s_rawHaveCount; }
