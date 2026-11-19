/**
 * @file ota_image.h
 * @brief Pure checks of an uploaded firmware image before any byte of it is written to flash.
 *
 * No Arduino or ESP-IDF calls, so the host unit tests in 868Receiver/test_host include this
 * header directly. Included by ota_update.cpp.
 *
 * ESP-IDF application image layout (esp_image_format.h, esp_app_format.h):
 *   0   magic 0xE9
 *   1   segment count (1..16)
 *   12  chip ID, little endian (ESP32 = 0, ESP32-S2 = 2, ...)
 *   24  first segment header (load address, length: 8 bytes)
 *   32  esp_app_desc_t, starting with the magic word 0xABCD5432
 * A bootloader image has the same 0xE9 header but no app description, so it is refused.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <ctype.h>

#define OTA_IMAGE_MAGIC        0xE9
#define OTA_IMAGE_MAX_SEGMENTS 16
#define OTA_APP_DESC_MAGIC     0xABCD5432UL
#define OTA_HEADER_CHECK_LEN   36      // bytes needed by otaCheckAppHeader()

/**
 * @brief Checks the start of an image for an application of the given chip.
 * @param h       the first bytes of the image
 * @param n       how many there are (at least OTA_HEADER_CHECK_LEN, else it is refused)
 * @param chipId  the chip ID the image must be built for (CONFIG_IDF_FIRMWARE_CHIP_ID)
 * @return nullptr if the header is acceptable, otherwise a short English reason.
 */
static inline const char* otaCheckAppHeader(const uint8_t* h, size_t n, uint16_t chipId) {
    if (!h || n < OTA_HEADER_CHECK_LEN) return "image too short";
    if (h[0] != OTA_IMAGE_MAGIC) return "not an ESP32 firmware image (wrong magic byte)";
    if (h[1] == 0 || h[1] > OTA_IMAGE_MAX_SEGMENTS) return "not an ESP32 firmware image (bad segment count)";
    uint16_t chip = (uint16_t)(h[12] | (h[13] << 8));
    if (chip != chipId) return "firmware image for another chip";
    uint32_t desc = (uint32_t)h[32] | ((uint32_t)h[33] << 8) | ((uint32_t)h[34] << 16) | ((uint32_t)h[35] << 24);
    if (desc != OTA_APP_DESC_MAGIC) return "not an application image (no app description)";
    return nullptr;
}

/** @return true if s is exactly 32 hexadecimal digits (an MD5 digest). */
static inline bool otaIsMd5Hex(const char* s) {
    if (!s) return false;
    size_t i = 0;
    for (; s[i]; i++) {
        if (i >= 32 || !isxdigit((unsigned char)s[i])) return false;
    }
    return i == 32;
}
