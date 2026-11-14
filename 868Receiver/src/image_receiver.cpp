/**
 * @file image_receiver.cpp
 * @brief RF Camera Image Chunk Reassembly & MQTT Publishing Implementation
 */

#include "image_receiver.h"
#include "mqtt_manager.h"
#include <string.h>
#include <esp_heap_caps.h>

/* ------------------------------ JPEG chunks (type 0x80) ------------------------------
 * Chunk n carries bytes 8n .. 8n+len-1 of the JPEG (the MD CAM sends 8-byte chunks, the
 * last one shorter). Chunks are placed by number, so a lost packet only leaves an 8-byte
 * hole (zeros); the sender puts a restart marker after every block row, so a hole only
 * damages that row. A picture is finished by its last chunk, or by the first chunk of the
 * next picture, and is then published on MQTT and served on /api/camera.jpg. */
#define JPEG_CHUNK 8
#define JPEG_MAX_CHUNKS (MAX_IMAGE_SIZE / JPEG_CHUNK)

static uint8_t* s_imgBuf = nullptr;          // picture being received
static uint8_t* s_jpg = nullptr;             // last finished picture
static uint8_t s_have[JPEG_MAX_CHUNKS / 8];  // bit per chunk received
static size_t s_imgEnd = 0;                  // bytes up to the highest chunk seen
static uint16_t s_haveCount = 0;
static int32_t s_lastIdx = -1;
static size_t s_jpgLen = 0;
static uint16_t s_jpgMissing = 0;
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

// Finish the picture in s_imgBuf (len bytes; missing chunks stay zero) and publish it.
static void jpegFinish(size_t len) {
    if (len < 4 || !s_jpg) {
        jpegReset();
        return;
    }
    uint16_t chunks = (len + JPEG_CHUNK - 1) / JPEG_CHUNK;
    uint16_t missing = chunks > s_haveCount ? chunks - s_haveCount : 0;
    memcpy(s_jpg, s_imgBuf, len);
    s_jpgLen = len;
    s_jpgMissing = missing;
    bool soi = s_jpg[0] == 0xFF && s_jpg[1] == 0xD8;
    Serial.printf("[ImageReceiver] JPEG from %s: %u bytes, %u/%u chunks missing%s\n", s_activeDeviceId,
                  (unsigned int)len, missing, chunks, soi ? "" : " (no SOI)");
    if (!mqttPublishCameraImage(s_activeDeviceId, s_jpg, len))
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
        jpegFinish(s_imgEnd);           // previous picture lost its last chunk
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
    if (!s_rawBmp || s_rawNewSincePublish == 0) return;
    unsigned long now = millis();
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
