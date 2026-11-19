/**
 * @file ota_update.h
 * @brief Firmware update over Wi-Fi (POST /api/ota in web_portal.cpp) and rollback safety.
 *
 * The image is streamed into the other OTA app slot with the Update library. Nothing is written
 * before the first OTA_HEADER_CHECK_LEN bytes have passed otaCheckAppHeader() (an ESP32-S2
 * application image); the Update library also keeps the image's first bytes back until
 * Update.end(), so a broken or partial upload never becomes bootable and the running firmware
 * stays. The new firmware boots in the PENDING_VERIFY state: otaLoop() marks it valid once it
 * has run for 30 s with Wi-Fi connected, and rolls back to the previous firmware if that does
 * not happen within 5 minutes (a crash or reboot before that also rolls back, in the bootloader).
 */

#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

/** At boot: reads the running slot's state (and the firmware MD5 for /api/status). */
void otaInit();

/** Rollback check; call often. @param wifiConnected the station is connected to Wi-Fi. */
void otaLoop(bool wifiConnected);

/**
 * @brief Starts an upload of totalSize bytes (the request's Content-Length).
 * @param md5Hex optional 32 hex digit MD5 of the whole image ("" = none), checked by Update.end().
 * @return false if refused; otaError() says why.
 */
bool otaBegin(size_t totalSize, const char* md5Hex);

/** Next piece of the image. @return false once the upload has failed (the rest is ignored). */
bool otaWrite(const uint8_t* data, size_t len);

/** All bytes received: verifies and activates the new image. @return true = reboot to run it. */
bool otaFinish();

/** Drops an upload in progress (connection lost); the running firmware stays. */
void otaAbort();

/** Reason of the last refused or failed upload (English, for the HTTP reply). */
const char* otaError();

/** Label of the running app slot ("app0"/"app1"). */
const char* otaRunningPartition();

/** State of the running slot: "valid", "pending_verify", "undefined" (flashed over USB), ... */
const char* otaAppState();

/** MD5 of the running firmware image (equals the MD5 of the firmware.bin that was flashed). */
const String& otaFirmwareMd5();
