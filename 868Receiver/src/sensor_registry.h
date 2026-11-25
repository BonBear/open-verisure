/**
 * @file sensor_registry.h
 * @brief In-Memory Sensor Node Registry & Telemetry Cache
 *
 * Keeps track of all discovered Verisure sensor nodes, storing their latest
 * telemetry state, packet counters, RSSI history, and first/last seen timestamps.
 */

#pragma once

#include <Arduino.h>
#include "crypto.h"

/** @brief Maximum number of distinct sensors held in RAM simultaneously. */
#define MAX_SENSORS 16

/**
 * @struct SensorRecord
 * @brief In-memory cache record for a registered sensor node.
 */
struct SensorRecord {
    bool active;                 /**< True if slot contains an active registered sensor */
    VerisureTelemetry lastTelem; /**< Most recent validated telemetry data */
    unsigned long firstSeenMs;   /**< Uptime millis when first packet was received */
    unsigned long lastSeenMs;    /**< Uptime millis when last packet arrived */
    uint32_t packetCount;        /**< Cumulative packet counter for this device */
    bool hasTemp;                /**< True when tempC holds a valid decoded temperature */
    float tempC;                 /**< Most recent temperature in deg C (from 0xB6 packet) */
};

/**
 * @brief Clear and initialize sensor registry storage.
 */
void sensorRegistryInit();

/**
 * @brief Update or insert a sensor telemetry record.
 * @param telem Decrypted and validated telemetry packet.
 */
void sensorRegistryUpdate(const VerisureTelemetry& telem);

/**
 * @brief Get total number of distinct sensors currently registered.
 * @return Active sensor count.
 */
int sensorRegistryGetCount();

/**
 * @brief Get sensor record by index.
 * @param idx Index from 0 to count-1.
 * @return Const pointer to record or nullptr if invalid index.
 */
const SensorRecord* sensorRegistryGetByIndex(int idx);

/**
 * @brief Find an active sensor record by its 8-character ID.
 * @param deviceId Sensor identifier string.
 * @return Const pointer to record or nullptr if not found.
 */
const SensorRecord* sensorRegistryFind(const char* deviceId);
