/**
 * @file sensor_registry.cpp
 * @brief Implementation of In-Memory Sensor Registry
 */

#include "sensor_registry.h"
#include <string.h>

static SensorRecord s_sensors[MAX_SENSORS];
static int s_count = 0;

void sensorRegistryInit() {
    memset(s_sensors, 0, sizeof(s_sensors));
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
        Serial.printf("[Registry] Registrerade ny sensor: %s (Totalt: %d)\n", telem.deviceId, s_count);
    } else {
        Serial.printf("[Registry] Varning: Registret fullt (%d sensorer), ignorerar ny enhet %s\n",
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
