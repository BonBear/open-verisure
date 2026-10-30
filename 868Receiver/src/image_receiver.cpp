/**
 * @file image_receiver.cpp
 * @brief RF Camera Image Chunk Reassembly & MQTT Publishing Implementation
 */

#include "image_receiver.h"
#include "mqtt_manager.h"
#include <string.h>

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
