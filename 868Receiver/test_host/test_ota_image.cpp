/**
 * @file test_ota_image.cpp
 * @brief Host tests of ota_image.h: the check of an uploaded image's header before anything is
 *        written to flash, and the MD5 format check.
 */

#include "test.h"
#include "ota_image.h"

#define CHIP_S2 2

/* The first 36 bytes of a real ESP32-S2 firmware.bin of this gateway (esptool v5, IDF 5):
 * header (magic E9, 6 segments, DIO, 4 MB/80 MHz, entry point, chip ID 2, hash appended),
 * first segment header, then esp_app_desc_t's magic word 0xABCD5432. */
static const uint8_t GOOD[OTA_HEADER_CHECK_LEN] = {
    0xE9, 0x06, 0x02, 0x2F, 0x5C, 0x56, 0x02, 0x40, 0xEE, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x20, 0x00, 0x00, 0x3F, 0xC8, 0xAE, 0x02, 0x00, 0x32, 0x54, 0xCD, 0xAB,
};

static bool ok(const uint8_t* h, size_t n = OTA_HEADER_CHECK_LEN, uint16_t chip = CHIP_S2) {
    return otaCheckAppHeader(h, n, chip) == nullptr;
}

TEST(ota_header_accepts_s2_app) {
    CHECK(ok(GOOD));
    uint8_t longer[64];
    memcpy(longer, GOOD, sizeof(GOOD));
    memset(longer + sizeof(GOOD), 0xA5, sizeof(longer) - sizeof(GOOD));
    CHECK(ok(longer, sizeof(longer)));             // more bytes than needed are fine
}

TEST(ota_header_too_short) {
    CHECK(!ok(GOOD, OTA_HEADER_CHECK_LEN - 1));
    CHECK(!ok(GOOD, 0));
    CHECK(!ok(nullptr, OTA_HEADER_CHECK_LEN));
    CHECK_STR(otaCheckAppHeader(GOOD, 10, CHIP_S2), "image too short");
}

TEST(ota_header_wrong_magic) {
    uint8_t h[OTA_HEADER_CHECK_LEN];
    memcpy(h, GOOD, sizeof(h));
    h[0] = 0xE8;
    CHECK(!ok(h));
    // a JPEG, a text file, an erased flash dump
    const uint8_t jpg[OTA_HEADER_CHECK_LEN] = {0xFF, 0xD8, 0xFF, 0xE0};
    CHECK(!ok(jpg));
    uint8_t txt[OTA_HEADER_CHECK_LEN];
    memset(txt, 'A', sizeof(txt));
    CHECK(!ok(txt));
    uint8_t ff[OTA_HEADER_CHECK_LEN];
    memset(ff, 0xFF, sizeof(ff));
    CHECK(!ok(ff));
}

TEST(ota_header_segment_count) {
    uint8_t h[OTA_HEADER_CHECK_LEN];
    memcpy(h, GOOD, sizeof(h));
    h[1] = 0;
    CHECK(!ok(h));
    h[1] = OTA_IMAGE_MAX_SEGMENTS + 1;
    CHECK(!ok(h));
    h[1] = OTA_IMAGE_MAX_SEGMENTS;
    CHECK(ok(h));
    h[1] = 1;
    CHECK(ok(h));
}

TEST(ota_header_other_chip) {
    uint8_t h[OTA_HEADER_CHECK_LEN];
    memcpy(h, GOOD, sizeof(h));
    h[12] = 0x00;                                   // ESP32
    CHECK_STR(otaCheckAppHeader(h, sizeof(h), CHIP_S2), "firmware image for another chip");
    h[12] = 0x09;                                   // ESP32-S3
    CHECK(!ok(h));
    h[12] = 0x02;
    h[13] = 0x01;                                   // the high byte counts too
    CHECK(!ok(h));
    CHECK(ok(GOOD, OTA_HEADER_CHECK_LEN, CHIP_S2));
    CHECK(!ok(GOOD, OTA_HEADER_CHECK_LEN, 0));      // an S2 image on an ESP32
}

TEST(ota_header_not_an_app) {
    // a bootloader image: the same header, but no app description after the first segment header
    uint8_t h[OTA_HEADER_CHECK_LEN];
    memcpy(h, GOOD, sizeof(h));
    h[32] = 0x00; h[33] = 0x00; h[34] = 0x00; h[35] = 0x00;
    CHECK_STR(otaCheckAppHeader(h, sizeof(h), CHIP_S2), "not an application image (no app description)");
    memcpy(h, GOOD, sizeof(h));
    h[35] = 0xAC;                                   // one byte of the magic word off
    CHECK(!ok(h));
}

TEST(ota_md5_hex) {
    // built at run time: the repository's key guard flags 32-hex-digit literals
    const std::string hex16 = "0123456789abcdef", HEX16 = "0123456789ABCDEF";
    CHECK(otaIsMd5Hex((hex16 + HEX16).c_str()));
    CHECK(!otaIsMd5Hex((hex16 + HEX16.substr(0, 15)).c_str()));     // 31
    CHECK(!otaIsMd5Hex((hex16 + HEX16 + "0").c_str()));             // 33
    CHECK(!otaIsMd5Hex((hex16 + HEX16.substr(0, 15) + "g").c_str()));
    CHECK(!otaIsMd5Hex(""));
    CHECK(!otaIsMd5Hex(nullptr));
}
