/**
 * @file mqtt_manager.cpp
 * @brief MQTT Client State Machine & Message Publisher Implementation
 */

#include "mqtt_manager.h"
#include "config_manager.h"
#include "sensor_registry.h"
#include "ha_discovery.h"
#include <WiFi.h>
#include <PubSubClient.h>

static WiFiClient s_wifiClient;
static PubSubClient s_mqttClient(s_wifiClient);

static unsigned long s_lastReconnectAttempt = 0;
static unsigned long s_lastGatewayStatePub = 0;
static bool s_wasConnected = false;

static char s_discoveredDevices[MAX_SENSORS][9];
static int s_numDiscovered = 0;

/**
 * @brief Check if Home Assistant auto-discovery has already been published for this device.
 * @param deviceId Sensor identifier.
 * @return True if previously published.
 */
static bool isDeviceDiscovered(const char* deviceId) {
    for (int i = 0; i < s_numDiscovered; i++) {
        if (strcmp(s_discoveredDevices[i], deviceId) == 0) return true;
    }
    return false;
}

/**
 * @brief Register a device as announced in the discovery cache.
 * @param deviceId Sensor identifier.
 */
static void markDeviceDiscovered(const char* deviceId) {
    if (isDeviceDiscovered(deviceId)) return;
    if (s_numDiscovered < MAX_SENSORS) {
        strncpy(s_discoveredDevices[s_numDiscovered++], deviceId, 8);
        s_discoveredDevices[s_numDiscovered - 1][8] = '\0';
    }
}

/**
 * @brief Publish Home Assistant discovery configs for all entities of a sensor.
 * @param deviceId Sensor identifier.
 * @param baseTopic Gateway base MQTT topic prefix.
 */
static void publishSensorDiscovery(const char* deviceId, const char* baseTopic) {
    bool isMotionCam = haIsMotionCameraSensor(deviceId);
    if (isMotionCam) {
        // Clear any legacy Door/Vibration discovery topics for this Motion Camera ID
        String oldContact = "homeassistant/binary_sensor/verisure_" + String(deviceId) + "_contact/config";
        String oldVib     = "homeassistant/binary_sensor/verisure_" + String(deviceId) + "_vibration/config";
        s_mqttClient.publish(oldContact.c_str(), "", true);
        s_mqttClient.publish(oldVib.c_str(), "", true);

        int count = haDiscoveryGetMotionSensorEntityCount();
        for (int i = 0; i < count; i++) {
            const HaEntityDef* def = haDiscoveryGetMotionSensorEntityDef(i);
            if (!def) continue;

            String topic = haDiscoveryBuildTopic(deviceId, def);
            String payload = haDiscoveryBuildPayload(deviceId, def, baseTopic);

            s_mqttClient.publish(topic.c_str(), payload.c_str(), true); // Retained message
        }

        // Publish Home Assistant Camera entity discovery
        String camTopic = haDiscoveryBuildCameraTopic(deviceId);
        String camPayload = haDiscoveryBuildCameraPayload(deviceId, baseTopic);
        s_mqttClient.publish(camTopic.c_str(), camPayload.c_str(), true); // Retained message

        markDeviceDiscovered(deviceId);
        Serial.printf("[MQTT] Published Home Assistant Discovery for Motion Camera \"%s\" (%d sensor entities + 1 camera retained)\n", deviceId, count);
    } else {
        int count = haDiscoveryGetSensorEntityCount();
        for (int i = 0; i < count; i++) {
            const HaEntityDef* def = haDiscoveryGetSensorEntityDef(i);
            if (!def) continue;

            String topic = haDiscoveryBuildTopic(deviceId, def);
            String payload = haDiscoveryBuildPayload(deviceId, def, baseTopic);

            s_mqttClient.publish(topic.c_str(), payload.c_str(), true); // Retained message
        }
        markDeviceDiscovered(deviceId);
        Serial.printf("[MQTT] Published Home Assistant Discovery for sensor \"%s\" (%d entities retained)\n", deviceId, count);
    }
}

/**
 * @brief Publish Home Assistant discovery configs for the Gateway device itself.
 * @param baseTopic Gateway base MQTT topic prefix.
 */
static void publishGatewayDiscovery(const char* baseTopic) {
    int count = haDiscoveryGetGatewayEntityCount();
    for (int i = 0; i < count; i++) {
        const HaEntityDef* def = haDiscoveryGetGatewayEntityDef(i);
        if (!def) continue;

        String topic = haDiscoveryBuildGatewayTopic(def);
        String payload = haDiscoveryBuildGatewayPayload(def, baseTopic);

        s_mqttClient.publish(topic.c_str(), payload.c_str(), true); // Retained message
    }
    Serial.printf("[MQTT] Published Home Assistant Discovery for Gateway (%d entities retained)\n", count);
}

void mqttInit() {
    s_mqttClient.setBufferSize(1024);
    s_numDiscovered = 0;
}

bool mqttIsConnected() {
    return s_mqttClient.connected();
}

String mqttGetStatusString() {
    if (!WiFi.isConnected()) return "No Wi-Fi";
    if (s_mqttClient.connected()) {
        const GatewayConfig& cfg = ConfigManager::get();
        return "Connected to " + String(cfg.mqttHost) + ":" + String(cfg.mqttPort);
    }
    return "Disconnected (reconnecting...)";
}

/**
 * @brief Internal connection and retry logic.
 */
static void checkMqttConnection() {
    if (s_mqttClient.connected()) return;
    if (WiFi.status() != WL_CONNECTED) return;

    const GatewayConfig& cfg = ConfigManager::get();
    if (cfg.mqttHost[0] == '\0') return; // MQTT is not configured

    unsigned long now = millis();
    if (now - s_lastReconnectAttempt < 5000) return; // Retry interval: 5 seconds
    s_lastReconnectAttempt = now;

    s_mqttClient.setServer(cfg.mqttHost, cfg.mqttPort);

    // Build unique Client-ID from ESP32-S2 MAC address
    String clientId = "verisure-gw-" + WiFi.macAddress();
    clientId.replace(":", "");

    Serial.printf("[MQTT] Connecting to broker %s:%d (Client-ID: %s)...\n",
                  cfg.mqttHost, cfg.mqttPort, clientId.c_str());

    String statusTopic = String(cfg.mqttTopic) + "/status";

    bool connected = false;
    if (cfg.mqttUser[0] != '\0') {
        connected = s_mqttClient.connect(clientId.c_str(), cfg.mqttUser, cfg.mqttPass,
                                         statusTopic.c_str(), 0, true, "offline");
    } else {
        connected = s_mqttClient.connect(clientId.c_str(), nullptr, nullptr,
                                         statusTopic.c_str(), 0, true, "offline");
    }

    if (connected) {
        Serial.println("[MQTT] Connected to broker!");
        s_wasConnected = true;

        // Publish Gateway Online Status (Retained)
        s_mqttClient.publish(statusTopic.c_str(), "online", true);

        // Clean up the accidental test ID 2NCEKT3X (Board #2's ID used during initial test) from HA discovery
        const char* staleKeys[] = {"contact", "vibration", "motion", "button"};
        for (int k = 0; k < 4; k++) {
            String t = "homeassistant/binary_sensor/verisure_2NCEKT3X_" + String(staleKeys[k]) + "/config";
            s_mqttClient.publish(t.c_str(), "", true);
        }
        const char* staleSensors[] = {"battery", "voltage", "rssi"};
        for (int k = 0; k < 3; k++) {
            String t = "homeassistant/sensor/verisure_2NCEKT3X_" + String(staleSensors[k]) + "/config";
            s_mqttClient.publish(t.c_str(), "", true);
        }

        // Immediately publish gateway discovery and announce all known sensors
        publishGatewayDiscovery(cfg.mqttTopic);

        int count = sensorRegistryGetCount();
        for (int i = 0; i < count; i++) {
            const SensorRecord* rec = sensorRegistryGetByIndex(i);
            if (rec && rec->active) {
                publishSensorDiscovery(rec->lastTelem.deviceId, cfg.mqttTopic);
            }
        }
    } else {
        Serial.printf("[MQTT] Connection error! rc=%d\n", s_mqttClient.state());
    }
}

void mqttLoop(int currentRssi, uint8_t marcstate, uint32_t totalPackets) {
    checkMqttConnection();

    if (s_mqttClient.connected()) {
        s_mqttClient.loop();

        // Periodic Gateway heartbeat publishing every 30 seconds
        unsigned long now = millis();
        if (now - s_lastGatewayStatePub >= 30000 || s_lastGatewayStatePub == 0) {
            s_lastGatewayStatePub = now;
            const GatewayConfig& cfg = ConfigManager::get();
            String topic = String(cfg.mqttTopic) + "/gateway/state";
            String payload = haStateBuildGatewayPayload(currentRssi, marcstate, totalPackets, sensorRegistryGetCount());
            s_mqttClient.publish(topic.c_str(), payload.c_str(), false);
        }
    }
}

void mqttPublishTelemetry(const VerisureTelemetry& telem) {
    if (!telem.valid) return;

    const GatewayConfig& cfg = ConfigManager::get();
    if (!s_mqttClient.connected()) return;

    // 1. Ensure Home Assistant discovery is registered for this device
    if (!isDeviceDiscovered(telem.deviceId)) {
        publishSensorDiscovery(telem.deviceId, cfg.mqttTopic);
    }

    // 2. Publish state payload: <baseTopic>/<deviceId>/state
    String topic = haStateBuildTopic(telem.deviceId, cfg.mqttTopic);
    String payload = haStateBuildPayload(telem);

    bool ok = s_mqttClient.publish(topic.c_str(), payload.c_str(), false);
    if (ok) {
        Serial.printf("[MQTT] >> Published to %s: %s\n", topic.c_str(), payload.c_str());
    } else {
        Serial.printf("[MQTT] Error publishing to %s\n", topic.c_str());
    }
}

bool mqttPublishCameraImage(const char* deviceId, const uint8_t* jpegBuf, size_t len) {
    if (!s_mqttClient.connected() || !jpegBuf || len == 0) return false;

    const GatewayConfig& cfg = ConfigManager::get();
    String topic = String(cfg.mqttTopic) + "/" + String(deviceId) + "/image";

    // Use PubSubClient streaming publish to support images larger than MQTT_MAX_PACKET_SIZE
    bool ok = s_mqttClient.beginPublish(topic.c_str(), len, true);
    if (ok) {
        size_t written = s_mqttClient.write(jpegBuf, len);
        ok = s_mqttClient.endPublish();
        if (ok && written == len) {
            Serial.printf("[MQTT] >> Successfully published Camera Image (%u bytes) to %s\n", (unsigned int)len, topic.c_str());
            return true;
        }
    }
    Serial.printf("[MQTT] Error publishing Camera Image (%u bytes) to %s\n", (unsigned int)len, topic.c_str());
    return false;
}
