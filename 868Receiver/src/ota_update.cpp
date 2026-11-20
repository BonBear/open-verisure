/**
 * @file ota_update.cpp
 * @brief Firmware update over Wi-Fi: header check, streaming into the Update library, rollback.
 * See ota_update.h.
 */

#include "ota_update.h"
#include "ota_image.h"
#include <Update.h>
#include <esp_ota_ops.h>
#include <sdkconfig.h>

#define OTA_CONFIRM_AFTER_MS   30000UL     // the new firmware must run this long with Wi-Fi...
#define OTA_ROLLBACK_AFTER_MS  300000UL    // ...within this time, or it is rolled back

enum OtaState { OTA_IDLE, OTA_HEADER, OTA_WRITING, OTA_FAILED };

static OtaState s_state = OTA_IDLE;
static uint8_t s_header[OTA_HEADER_CHECK_LEN];
static size_t s_headerLen = 0;
static size_t s_total = 0;
static size_t s_received = 0;
static char s_md5[33];
static char s_error[96];
static bool s_pendingVerify = false;
static String s_firmwareMd5;

/* The Arduino core would mark a new image valid as soon as it starts (before setup()); with this
 * override it stays PENDING_VERIFY until otaLoop() confirms it (CONFIG_APP_ROLLBACK_ENABLE). */
extern "C" bool verifyRollbackLater() { return true; }

static bool fail(const char* why) {
    if (why != s_error) snprintf(s_error, sizeof(s_error), "%s", why ? why : "update error");
    if (Update.isRunning()) Update.abort();
    s_state = OTA_FAILED;
    Serial.printf("[OTA] Update refused: %s (running firmware unchanged)\n", s_error);
    return false;
}

void otaInit() {
    const esp_partition_t* run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    s_pendingVerify = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    s_firmwareMd5 = ESP.getSketchMD5();
    Serial.printf("[OTA] Running from %s (%s), firmware MD5 %s\n", otaRunningPartition(), otaAppState(),
                  s_firmwareMd5.c_str());
    if (s_pendingVerify)
        Serial.println("[OTA] New firmware: it is confirmed after 30 s with Wi-Fi, else rolled back");
}

void otaLoop(bool wifiConnected) {
    if (!s_pendingVerify) return;
    unsigned long up = millis();
    if (wifiConnected && up >= OTA_CONFIRM_AFTER_MS) {
        esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        s_pendingVerify = false;
        Serial.printf("[OTA] New firmware confirmed (%s)\n", e == ESP_OK ? "valid" : esp_err_to_name(e));
    } else if (up >= OTA_ROLLBACK_AFTER_MS) {
        Serial.println("[OTA] New firmware did not connect to Wi-Fi within 5 min: rolling back");
        Serial.flush();
        delay(100);
        esp_ota_mark_app_invalid_rollback_and_reboot();   // returns only if there is nothing to roll back to
        s_pendingVerify = false;
    }
}

bool otaBegin(size_t totalSize, const char* md5Hex) {
    if (Update.isRunning()) Update.abort();
    s_state = OTA_HEADER;
    s_headerLen = 0;
    s_total = totalSize;
    s_received = 0;
    s_md5[0] = 0;
    s_error[0] = 0;

    if (s_pendingVerify)
        return fail("the running firmware is not confirmed yet (wait 30 s after it has joined Wi-Fi)");
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (!next) return fail("no OTA partition");
    if (totalSize <= OTA_HEADER_CHECK_LEN) return fail("empty or too short image");
    if (totalSize > next->size) {
        snprintf(s_error, sizeof(s_error), "image larger than the OTA slot (%u bytes)", (unsigned)next->size);
        return fail(s_error);
    }
    if (md5Hex && md5Hex[0]) {
        if (!otaIsMd5Hex(md5Hex)) return fail("the MD5 must be 32 hex digits");
        snprintf(s_md5, sizeof(s_md5), "%s", md5Hex);
    }
    Serial.printf("[OTA] Receiving %u bytes for %s%s\n", (unsigned)totalSize, next->label, s_md5[0] ? " (MD5 given)" : "");
    return true;
}

bool otaWrite(const uint8_t* data, size_t len) {
    if (s_state != OTA_HEADER && s_state != OTA_WRITING) return false;
    s_received += len;
    if (s_received > s_total) return fail("more data than announced");

    if (s_state == OTA_HEADER) {
        size_t take = OTA_HEADER_CHECK_LEN - s_headerLen;
        if (take > len) take = len;
        memcpy(s_header + s_headerLen, data, take);
        s_headerLen += take;
        data += take;
        len -= take;
        if (s_headerLen < OTA_HEADER_CHECK_LEN) return true;

        // nothing is written to flash before the header has been checked
        const char* why = otaCheckAppHeader(s_header, s_headerLen, CONFIG_IDF_FIRMWARE_CHIP_ID);
        if (why) return fail(why);
        if (!Update.begin(s_total, U_FLASH)) return fail(Update.errorString());
        if (s_md5[0] && !Update.setMD5(s_md5)) return fail("the MD5 must be 32 hex digits");
        s_state = OTA_WRITING;
        if (Update.write(s_header, s_headerLen) != s_headerLen) return fail(Update.errorString());
    }
    if (len && Update.write((uint8_t*)data, len) != len) return fail(Update.errorString());
    return true;
}

bool otaFinish() {
    if (s_state == OTA_HEADER) fail(s_received ? "image too short" : "no image in the request body");
    if (s_state != OTA_WRITING) {
        if (!s_error[0]) snprintf(s_error, sizeof(s_error), "no upload");
        s_state = OTA_IDLE;
        return false;
    }
    if (!Update.isFinished()) {
        snprintf(s_error, sizeof(s_error), "image incomplete (%u of %u bytes)", (unsigned)s_received, (unsigned)s_total);
        fail(s_error);
        s_state = OTA_IDLE;
        return false;
    }
    // all bytes are there (checked above); end() checks the MD5 and, in esp_ota_set_boot_partition(),
    // verifies the whole image (checksum, appended SHA-256) before it becomes the boot slot
    if (!Update.end(true)) {
        fail(Update.getError() == UPDATE_ERROR_ACTIVATE ? "the image does not verify (truncated or corrupt)"
                                                        : Update.errorString());
        s_state = OTA_IDLE;
        return false;
    }
    s_state = OTA_IDLE;
    Serial.printf("[OTA] Update of %u bytes written and verified; it runs after the reboot\n", (unsigned)s_total);
    return true;
}

void otaAbort() {
    bool active = s_state == OTA_HEADER || s_state == OTA_WRITING;
    if (Update.isRunning()) Update.abort();
    s_state = OTA_IDLE;
    if (active) {
        snprintf(s_error, sizeof(s_error), "upload aborted after %u of %u bytes", (unsigned)s_received, (unsigned)s_total);
        Serial.printf("[OTA] %s (running firmware unchanged)\n", s_error);
    }
}

const char* otaError() { return s_error[0] ? s_error : "unknown error"; }

const char* otaRunningPartition() {
    const esp_partition_t* run = esp_ota_get_running_partition();
    return run ? run->label : "?";
}

const char* otaAppState() {
    const esp_partition_t* run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK) return "undefined";
    switch (st) {
        case ESP_OTA_IMG_NEW:            return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_VALID:          return "valid";
        case ESP_OTA_IMG_INVALID:        return "invalid";
        case ESP_OTA_IMG_ABORTED:        return "aborted";
        default:                         return "undefined";
    }
}

const String& otaFirmwareMd5() { return s_firmwareMd5; }
