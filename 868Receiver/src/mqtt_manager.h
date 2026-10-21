/**
 * @file mqtt_manager.h
 * @brief MQTT Client, Connection Management & Telemetry Publisher
 *
 * Handles MQTT broker connectivity, automated Home Assistant auto-discovery
 * announcement, telemetry packet publishing, and periodic gateway heartbeats.
 */

#pragma once

#include <Arduino.h>
#include "crypto.h"

/**
 * @brief Initialize MQTT client buffer and internal state tracking.
 */
void mqttInit();

/**
 * @brief Non-blocking MQTT service loop.
 * @param currentRssi Background RF signal/noise in dBm.
 * @param marcstate Radio modem state register.
 * @param totalPackets Total received packets counter.
 * @note Call regularly from main loop. Handles auto-reconnect and periodic heartbeats.
 */
void mqttLoop(int currentRssi, uint8_t marcstate, uint32_t totalPackets);

/**
 * @brief Publish a decrypted sensor telemetry packet to MQTT.
 * @param telem Decrypted and authenticated telemetry packet.
 * @details Also publishes Home Assistant discovery payload if this device hasn't been seen yet.
 */
void mqttPublishTelemetry(const VerisureTelemetry& telem);

/**
 * @brief Check if the gateway is currently connected to the MQTT broker.
 * @return True if connected.
 */
bool mqttIsConnected();

/**
 * @brief Get human-readable connection status string.
 * @return "Ansluten (IP:Port)" or "Frånkopplad / Försöker ansluta...".
 */
String mqttGetStatusString();
