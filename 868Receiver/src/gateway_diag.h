/**
 * @file gateway_diag.h
 * @brief Gateway diagnostics: counters that make attacks and faults visible (Home Assistant,
 *        `/api/status` "diag" object, serial `status`).
 *
 * The counters live in RAM and count since the last boot (a reboot resets them). They are only
 * incremented, from the radio path in loop() (main.cpp, crypto.cpp): no allocation, no locking
 * (everything that reads them runs in the same loop task).
 */

#pragma once

#include <Arduino.h>
#include <stdint.h>

/** Counters since boot. */
struct GatewayDiag {
    uint32_t rxCrcOk;        /**< Frames received with a good CRC (24-byte or 56-byte packet) */
    uint32_t authOk;         /**< 24-byte packets that passed authentication ('VS' + ID echo) */
    uint32_t authFail;       /**< 24-byte packets rejected by authentication (bad ID, wrong key, ID echo, 'VS') */
    uint32_t replayDrop;     /**< Authenticated 24-byte packets dropped by the replay filter */
    uint32_t picStale;       /**< Picture chunks dropped as stale (outside the replay window) */
    uint32_t sessionFull;    /**< A new device found the replay session table full */
    char lastRejectedId[9];  /**< ID of the last device rejected (authentication or replay), "" = none */
};

extern GatewayDiag g_gatewayDiag;

/** Values sampled when the diagnostics are published (not kept anywhere). */
struct GatewayDiagSample {
    uint32_t uptimeS;        /**< Seconds since boot */
    const char* resetReason; /**< Reason of the last reset, e.g. "poweron", "panic", "brownout" */
    uint32_t freeHeap;       /**< Free heap in bytes */
    uint32_t minFreeHeap;    /**< Lowest free heap since boot in bytes */
};

/** Remember the ID of a rejected device (only an 8-character alphanumeric ID; never packet data). */
void gatewayDiagNoteRejected(const char* deviceId);

/** Uptime, reset reason and heap now. */
GatewayDiagSample gatewayDiagSample();

/**
 * @brief Append the diagnostics as JSON members (`"uptime":..,"reset_reason":"..",...`, no braces,
 *        no trailing comma) for the MQTT gateway state and the `/api/status` "diag" object.
 */
void gatewayDiagAppendJson(String& out, const GatewayDiagSample& s);

/** Clear all counters (host unit tests). */
void gatewayDiagClear();
