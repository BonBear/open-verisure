/**
 * @file serial_cli.h
 * @brief Interactive Serial Command Line Interface (CLI)
 *
 * Provides diagnostic, debugging, and configuration commands over the
 * USB Serial interface (115200 baud).
 *
 * Commands:
 * - `help`                 : List available commands
 * - `status`               : Display gateway uptime, radio, Wi-Fi, and active sensors
 * - `testspi`              : Hardware CC1101 SPI bus diagnostic and register check
 * - `scan`                 : Scan for 2.4 GHz Wi-Fi networks (SSID, channel, RSSI)
 * - `connect <ssid> [pass]`: Connect to Wi-Fi and save to NVS
 * - `key <32hex>`          : Update Master AES Key
 * - `save`                 : Save active configuration to NVS
 * - `reset`                : Reset NVS settings to factory defaults
 * - `reboot`               : Restart the ESP32-S2
 */

#pragma once

#include <Arduino.h>

/** @brief Running calibration statistics for `pch cal` / `pch status` (implemented in main.cpp). */
struct PchCalStats {
    uint32_t count;
    int32_t  freqEstSum;
    int16_t  lastRssi1;
    int16_t  lastRssi2;
    int8_t   lastFreqEst1;
    int8_t   lastFreqEst2;
    int8_t   suggestedOffset;
};

void pchCalReset();
void pchCalGetStats(PchCalStats* out);

/**
 * @brief Initialize the serial command buffer.
 */
void serialCliInit();

/**
 * @brief Non-blocking serial input processing loop. Call frequently from main loop.
 */
void serialCliLoop();
