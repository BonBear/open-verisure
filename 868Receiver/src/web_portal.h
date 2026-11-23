/**
 * @file web_portal.h
 * @brief Embedded Web Management Portal & Captive Portal
 *
 * Provides a responsive HTTP web interface (port 80) and captive portal DNS server
 * for managing Wi-Fi, MQTT broker parameters, Master AES Key, radio diagnostics,
 * and live sensor states.
 *
 * REST Endpoints:
 * - `GET  /`          : Responsive single-page web management dashboard
 * - `GET  /api/status`: System status JSON (uptime, RAM, Wi-Fi, IP, sensors)
 * - `GET  /api/radio` : CC1101 radio diagnostics (MARCSTATE, RSSI, registers)
 * - `GET  /api/scan`  : Perform Wi-Fi scan and return JSON array of networks
 * - `POST /api/save`  : Save new configuration parameters to NVS
 * - `POST /api/reboot`: Reboot the ESP32-S2
 * - `POST /api/reset` : Factory reset NVS configuration
 * - `POST /api/ota`   : Firmware update (raw image body), then reboot
 */

#pragma once

#include <Arduino.h>
#include <stdint.h>

/**
 * @brief Initialize the Web Server, Captive Portal DNS, and network listeners.
 */
void webPortalInit();

/**
 * @brief Non-blocking web server and DNS query processing loop.
 * @param currentRssi Current background RF signal/noise in dBm.
 * @param marcstate Radio modem state register.
 * @param totalPackets Total received packets counter.
 */
void webPortalLoop(int currentRssi, uint8_t marcstate, uint32_t totalPackets);

/**
 * @brief Check if the gateway is running in standalone SoftAP mode.
 * @return True if in AP mode (SSID "Verisure-Gateway").
 */
bool webPortalIsApMode();

/**
 * @brief Get the current IP address string.
 * @return SoftAP IP or Station local IP.
 */
String webPortalGetIp();

/**
 * @brief Cleanly exit setup SoftAP and Captive Portal DNS once STA is connected.
 *        Must be called from loopTask.
 */
void webPortalOnStaConnected();
