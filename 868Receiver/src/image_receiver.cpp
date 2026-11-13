/**
 * @file image_receiver.cpp
 * @brief RF Camera Image Chunk Reassembly & MQTT Publishing Implementation
 */

#include "image_receiver.h"
#include "mqtt_manager.h"
#include <string.h>
#include <esp_heap_caps.h>

static uint8_t s_imgBuf[MAX_IMAGE_SIZE];
static size_t s_imgLen = 0;
static uint16_t s_expectedChunk = 0;
static char s_activeDeviceId[10] = {0};
static unsigned long s_lastChunkTime = 0;

void imageReceiverInit(void) {
    s_imgLen = 0;
    s_expectedChunk = 0;
    s_activeDeviceId[0] = '\0';
    s_lastChunkTime = 0;
}

void imageReceiverProcessChunk(const char* deviceId, uint16_t chunkIdx, uint8_t chunkLen,
                               bool isFirst, bool isLast, const uint8_t* data) {
    if (!deviceId || !data || chunkLen == 0 || chunkLen > 8) return;

    unsigned long now = millis();

    // Reset buffer on First chunk or on timeout (> 8 seconds since last chunk)
    if (isFirst || (s_imgLen > 0 && now - s_lastChunkTime > 8000) ||
        strcmp(s_activeDeviceId, deviceId) != 0) {
        s_imgLen = 0;
        s_expectedChunk = 0;
        strncpy(s_activeDeviceId, deviceId, 9);
        s_activeDeviceId[9] = '\0';
    }

    s_lastChunkTime = now;

    // Buffer overflow guard
    if (s_imgLen + chunkLen > MAX_IMAGE_SIZE) {
        Serial.printf("[ImageReceiver] Image buffer overflow (> %d bytes), resetting.\n", MAX_IMAGE_SIZE);
        s_imgLen = 0;
        s_expectedChunk = 0;
        return;
    }

    // Append chunk
    memcpy(&s_imgBuf[s_imgLen], data, chunkLen);
    s_imgLen += chunkLen;
    s_expectedChunk = chunkIdx + 1;

    // Progress log every 32 chunks (~256 bytes) or on completion
    if ((chunkIdx % 32 == 0) || isLast) {
        Serial.printf("[ImageReceiver] [%s] Chunk #%u received (+%u B, Total: %u B)%s\n",
                      deviceId, chunkIdx, chunkLen, (unsigned int)s_imgLen, isLast ? " [FINAL CHUNK]" : "");
    }

    // Final chunk received -> publish to Home Assistant!
    if (isLast) {
        // Validate JPEG SOI marker (0xFF 0xD8)
        if (s_imgLen >= 4 && s_imgBuf[0] == 0xFF && s_imgBuf[1] == 0xD8) {
            Serial.printf("[ImageReceiver] >>> VALID JPEG IMAGE RECEIVED (%u bytes) from %s! <<<\n",
                          (unsigned int)s_imgLen, deviceId);
            bool ok = mqttPublishCameraImage(deviceId, s_imgBuf, s_imgLen);
            if (ok) {
                Serial.printf("[ImageReceiver] Successfully delivered camera image to Home Assistant MQTT!\n");
            } else {
                Serial.printf("[ImageReceiver] Failed to publish image to MQTT broker.\n");
            }
        } else {
            Serial.printf("[ImageReceiver] Warning: Image completed (%u bytes) but missing JPEG SOI header (0x%02X 0x%02X).\n",
                          (unsigned int)s_imgLen, s_imgBuf[0], s_imgBuf[1]);
            // Publish anyway in case it's a valid format Home Assistant can decode
            mqttPublishCameraImage(deviceId, s_imgBuf, s_imgLen);
        }

        // Reset for next image
        s_imgLen = 0;
        s_expectedChunk = 0;
    }
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
