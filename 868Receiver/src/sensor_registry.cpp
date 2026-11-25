/**
 * @file sensor_registry.cpp
 * @brief Implementation of In-Memory Sensor Registry
 */

#include "sensor_registry.h"
#include <string.h>

static SensorRecord s_sensors[MAX_SENSORS];
static int s_count = 0;

void sensorRegistryInit() {
    for (int i = 0; i < MAX_SENSORS; i++) {
        s_sensors[i] = SensorRecord{};
    }
    s_count = 0;
}

void sensorRegistryUpdate(const VerisureTelemetry& telem) {
    if (!telem.valid) return;

    // Search for existing device
    for (int i = 0; i < s_count; i++) {
        if (s_sensors[i].active && strcmp(s_sensors[i].lastTelem.deviceId, telem.deviceId) == 0) {
            s_sensors[i].lastTelem = telem;
            s_sensors[i].lastSeenMs = millis();
            s_sensors[i].packetCount++;
            if (telem.hasTemp) {
                s_sensors[i].hasTemp = true;
                s_sensors[i].tempC = telem.tempC;
            }
            return;
        }
    }

    // Allocate new slot if table is not full
    if (s_count < MAX_SENSORS) {
        int idx = s_count++;
        s_sensors[idx].active = true;
        s_sensors[idx].lastTelem = telem;
        s_sensors[idx].firstSeenMs = millis();
        s_sensors[idx].lastSeenMs = millis();
        s_sensors[idx].packetCount = 1;
        s_sensors[idx].hasTemp = telem.hasTemp;
        s_sensors[idx].tempC = telem.tempC;
        Serial.printf("[Registry] Registered new sensor: %s (Total: %d)\n", telem.deviceId, s_count);
    } else {
        Serial.printf("[Registry] Warning: Registry full (%d sensors), ignoring new node %s\n",
                      MAX_SENSORS, telem.deviceId);
    }
}

int sensorRegistryGetCount() {
    return s_count;
}

const SensorRecord* sensorRegistryGetByIndex(int idx) {
    if (idx < 0 || idx >= s_count) return nullptr;
    return &s_sensors[idx];
}

const SensorRecord* sensorRegistryFind(const char* deviceId) {
    if (!deviceId) return nullptr;
    for (int i = 0; i < s_count; i++) {
        if (s_sensors[i].active && strcmp(s_sensors[i].lastTelem.deviceId, deviceId) == 0) {
            return &s_sensors[i];
        }
    }
    return nullptr;
}
