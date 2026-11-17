/**
 * @file ha_discovery.cpp
 * @brief Home Assistant MQTT Auto-Discovery Implementation
 */

#include "ha_discovery.h"

/** Seconds without a packet before Home Assistant shows a camera's entities as unavailable
 *  (the camera polls every ~30 s, every ~2 minutes when disarmed and idle). */
#define HA_CAMERA_EXPIRE_S 600

/**
 * @brief Sensor entity configuration table.
 * @details Defines discovery parameters for all sensor metrics sent by a Verisure node.
 */
static const HaEntityDef S_SENSOR_ENTITIES[] = {
    {
        "contact",
        "binary_sensor",
        "Contact",
        "door",
        nullptr,
        nullptr,
        "{{ value_json.contact }}",
        "OPEN",
        "CLOSED",
        nullptr
    },
    {
        "vibration",
        "binary_sensor",
        "Vibration",
        "vibration",
        nullptr,
        nullptr,
        "{{ value_json.vibration }}",
        "ON",
        "OFF",
        nullptr
    },
    {
        "button",
        "binary_sensor",
        "Tamper",
        "tamper",
        nullptr,
        nullptr,
        "{{ value_json.button }}",
        "ON",
        "OFF",
        "diagnostic"
    },
    {
        "battery",
        "sensor",
        "Battery",
        "battery",
        "measurement",
        "%",
        "{{ value_json.battery_pct }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "voltage",
        "sensor",
        "Battery Voltage",
        "voltage",
        "measurement",
        "mV",
        "{{ value_json.battery_mv }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "rssi",
        "sensor",
        "Signal Strength",
        "signal_strength",
        "measurement",
        "dBm",
        "{{ value_json.rssi }}",
        nullptr,
        nullptr,
        "diagnostic"
    }
};

/**
 * @brief Motion Detector Camera (GW MD CAM2) entity configuration table.
 */
static const HaEntityDef S_MOTION_SENSOR_ENTITIES[] = {
    {
        "motion",
        "binary_sensor",
        "Motion",
        "motion",
        nullptr,
        nullptr,
        "{{ value_json.contact }}",
        "OPEN",
        "CLOSED",
        nullptr
    },
    {
        "button",
        "binary_sensor",
        "Tamper",
        "tamper",
        nullptr,
        nullptr,
        "{{ value_json.button }}",
        "ON",
        "OFF",
        "diagnostic"
    },
    {
        "battery",
        "sensor",
        "Battery",
        "battery",
        "measurement",
        "%",
        "{{ value_json.battery_pct }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "voltage",
        "sensor",
        "Battery Voltage",
        "voltage",
        "measurement",
        "mV",
        "{{ value_json.battery_mv }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "battery_load",
        "sensor",
        "Battery Voltage Under Load",
        "voltage",
        "measurement",
        "mV",
        // 0 until the camera has reported a value (after its first picture since a restart)
        "{{ value_json.battery_load_mv if value_json.battery_load_mv | default(0) > 0 else None }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "low_battery",
        "binary_sensor",
        "Low Battery",
        "battery",
        nullptr,
        nullptr,
        "{{ value_json.low_battery }}",
        "ON",
        "OFF",
        "diagnostic"
    },
    {
        "rssi",
        "sensor",
        "Signal Strength",
        "signal_strength",
        "measurement",
        "dBm",
        "{{ value_json.rssi }}",
        nullptr,
        nullptr,
        "diagnostic"
    }
};

/**
 * @brief Gateway health and diagnostics entity configuration table.
 */
static const HaEntityDef S_GATEWAY_ENTITIES[] = {
    {
        "radio_rssi",
        "sensor",
        "Radio Noise Floor",
        "signal_strength",
        "measurement",
        "dBm",
        "{{ value_json.radio_rssi }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "packets",
        "sensor",
        "Packets Received",
        nullptr,
        "total_increasing",
        "pkts",
        "{{ value_json.packets }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "sensors",
        "sensor",
        "Active Sensors",
        nullptr,
        "measurement",
        "sensors",
        "{{ value_json.sensors }}",
        nullptr,
        nullptr,
        "diagnostic"
    }
};

static const int NUM_SENSOR_ENTITIES = sizeof(S_SENSOR_ENTITIES) / sizeof(S_SENSOR_ENTITIES[0]);
static const int NUM_MOTION_SENSOR_ENTITIES = sizeof(S_MOTION_SENSOR_ENTITIES) / sizeof(S_MOTION_SENSOR_ENTITIES[0]);
static const int NUM_GATEWAY_ENTITIES = sizeof(S_GATEWAY_ENTITIES) / sizeof(S_GATEWAY_ENTITIES[0]);

bool haIsMotionCameraSensor(const char* deviceId) {
    return deviceId && strncmp(deviceId, "2NCE", 4) == 0;
}

int haDiscoveryGetSensorEntityCount() {
    return NUM_SENSOR_ENTITIES;
}

const HaEntityDef* haDiscoveryGetSensorEntityDef(int idx) {
    if (idx < 0 || idx >= NUM_SENSOR_ENTITIES) return nullptr;
    return &S_SENSOR_ENTITIES[idx];
}

int haDiscoveryGetMotionSensorEntityCount() {
    return NUM_MOTION_SENSOR_ENTITIES;
}

const HaEntityDef* haDiscoveryGetMotionSensorEntityDef(int idx) {
    if (idx < 0 || idx >= NUM_MOTION_SENSOR_ENTITIES) return nullptr;
    return &S_MOTION_SENSOR_ENTITIES[idx];
}

String haDiscoveryBuildTopic(const char* deviceId, const HaEntityDef* def) {
    return "homeassistant/" + String(def->component) + "/verisure_" + String(deviceId) + "_" + String(def->key) + "/config";
}

String haDiscoveryBuildCameraTopic(const char* deviceId) {
    return "homeassistant/camera/verisure_" + String(deviceId) + "_camera/config";
}

String haDiscoveryBuildCameraPayload(const char* deviceId, const char* baseTopic) {
    String imageTopic = String(baseTopic) + "/" + String(deviceId) + "/image";
    String uniqueId = "verisure_" + String(deviceId) + "_camera";

    String p = "{";
    p += "\"name\":\"Camera\",";
    p += "\"unique_id\":\"" + uniqueId + "\",";
    p += "\"topic\":\"" + imageTopic + "\",";
    p += "\"availability_topic\":\"" + String(baseTopic) + "/status\",";
    p += "\"payload_available\":\"online\",";
    p += "\"payload_not_available\":\"offline\",";
    p += "\"device\":{";
    p += "\"identifiers\":[\"verisure_" + String(deviceId) + "\"],";
    p += "\"name\":\"Verisure Motion Camera " + String(deviceId) + "\",";
    p += "\"model\":\"GW MD CAM2 (Motion Detector & Camera)\",";
    p += "\"manufacturer\":\"Verisure\",";
    p += "\"sw_version\":\"2.0-CC1110\",";
    p += "\"via_device\":\"verisure_gateway\"";
    p += "}}";
    return p;
}

String haDiscoveryBuildPayload(const char* deviceId, const HaEntityDef* def, const char* baseTopic) {
    String stateTopic = haStateBuildTopic(deviceId, baseTopic);
    String uniqueId = "verisure_" + String(deviceId) + "_" + String(def->key);
    bool isMotionCam = haIsMotionCameraSensor(deviceId);

    String p = "{";
    p += "\"name\":\"" + String(def->name) + "\",";
    p += "\"unique_id\":\"" + uniqueId + "\",";
    p += "\"state_topic\":\"" + stateTopic + "\",";
    p += "\"value_template\":\"" + String(def->valTpl) + "\",";

    if (def->deviceClass) {
        p += "\"device_class\":\"" + String(def->deviceClass) + "\",";
    }
    if (def->stateClass) {
        p += "\"state_class\":\"" + String(def->stateClass) + "\",";
    }
    if (def->unit) {
        p += "\"unit_of_measurement\":\"" + String(def->unit) + "\",";
    }
    if (def->plOn) {
        p += "\"payload_on\":\"" + String(def->plOn) + "\",";
    }
    if (def->plOff) {
        p += "\"payload_off\":\"" + String(def->plOff) + "\",";
    }
    if (def->category) {
        p += "\"entity_category\":\"" + String(def->category) + "\",";
    }
    if (isMotionCam) {
        // the camera polls at least every ~2 minutes: after 10 minutes without a packet its
        // entities turn unavailable (dead battery, out of range) instead of keeping old values
        p += "\"expire_after\":" + String(HA_CAMERA_EXPIRE_S) + ",";
    }

    String availTopic = String(baseTopic) + "/status";
    p += "\"availability_topic\":\"" + availTopic + "\",";
    p += "\"payload_available\":\"online\",";
    p += "\"payload_not_available\":\"offline\",";

    // Home Assistant Device Registry Block
    p += "\"device\":{";
    p += "\"identifiers\":[\"verisure_" + String(deviceId) + "\"],";
    if (isMotionCam) {
        p += "\"name\":\"Verisure Motion Camera " + String(deviceId) + "\",";
        p += "\"model\":\"GW MD CAM2 (Motion Detector & Camera)\",";
    } else {
        p += "\"name\":\"Verisure Sensor " + String(deviceId) + "\",";
        p += "\"model\":\"GW-MC-SHOCK (Door/Window & Shock)\",";
    }
    p += "\"manufacturer\":\"Verisure\",";
    p += "\"sw_version\":\"2.0-CC1110\",";
    p += "\"via_device\":\"verisure_gateway\"";
    p += "}}";

    return p;
}

String haStateBuildTopic(const char* deviceId, const char* baseTopic) {
    return String(baseTopic) + "/" + String(deviceId) + "/state";
}

String haStateBuildPayload(const VerisureTelemetry& telem) {
    String p = "{";
    p += "\"contact\":\"" + String(telem.isOpen ? "OPEN" : "CLOSED") + "\",";
    p += "\"motion\":\"" + String(telem.isOpen ? "ON" : "OFF") + "\",";
    p += "\"vibration\":\"" + String(telem.isShock ? "ON" : "OFF") + "\",";
    p += "\"button\":\"" + String(telem.isButton ? "ON" : "OFF") + "\",";
    p += "\"power_on\":\"" + String(telem.isPowerOn ? "ON" : "OFF") + "\",";
    p += "\"battery_pct\":" + String(telem.batteryPct) + ",";
    p += "\"battery_mv\":" + String(telem.batteryMv) + ",";
    p += "\"low_battery\":\"" + String(telem.isLowBatt ? "ON" : "OFF") + "\",";
    if (telem.batteryLoadMv) p += "\"battery_load_mv\":" + String(telem.batteryLoadMv) + ",";
    p += "\"rssi\":" + String(telem.rssi) + ",";
    p += "\"lqi\":" + String(telem.lqi) + ",";
    p += "\"seq\":" + String(telem.counter);
    p += "}";
    return p;
}

int haDiscoveryGetGatewayEntityCount() {
    return NUM_GATEWAY_ENTITIES;
}

const HaEntityDef* haDiscoveryGetGatewayEntityDef(int idx) {
    if (idx < 0 || idx >= NUM_GATEWAY_ENTITIES) return nullptr;
    return &S_GATEWAY_ENTITIES[idx];
}

String haDiscoveryBuildGatewayTopic(const HaEntityDef* def) {
    return "homeassistant/" + String(def->component) + "/verisure_gateway_" + String(def->key) + "/config";
}

String haDiscoveryBuildGatewayPayload(const HaEntityDef* def, const char* baseTopic) {
    String stateTopic = String(baseTopic) + "/gateway/state";
    String uniqueId = "verisure_gateway_" + String(def->key);

    String p = "{";
    p += "\"name\":\"" + String(def->name) + "\",";
    p += "\"unique_id\":\"" + uniqueId + "\",";
    p += "\"state_topic\":\"" + stateTopic + "\",";
    p += "\"value_template\":\"" + String(def->valTpl) + "\",";

    if (def->deviceClass) {
        p += "\"device_class\":\"" + String(def->deviceClass) + "\",";
    }
    if (def->stateClass) {
        p += "\"state_class\":\"" + String(def->stateClass) + "\",";
    }
    if (def->unit) {
        p += "\"unit_of_measurement\":\"" + String(def->unit) + "\",";
    }
    if (def->category) {
        p += "\"entity_category\":\"" + String(def->category) + "\",";
    }

    String availTopic = String(baseTopic) + "/status";
    p += "\"availability_topic\":\"" + availTopic + "\",";
    p += "\"payload_available\":\"online\",";
    p += "\"payload_not_available\":\"offline\",";

    p += "\"device\":{";
    p += "\"identifiers\":[\"verisure_gateway\"],";
    p += "\"name\":\"Verisure 868 Gateway\",";
    p += "\"model\":\"ESP32-S2 CC1101 Sub-1GHz Bridge\",";
    p += "\"manufacturer\":\"Antigravity Engineering\",";
    p += "\"sw_version\":\"2.0.0\"";
    p += "}}";

    return p;
}

String haStateBuildGatewayPayload(int radioRssi, uint8_t marcstate, uint32_t totalPackets, int activeSensors) {
    String p = "{";
    p += "\"radio_rssi\":" + String(radioRssi) + ",";
    p += "\"marcstate\":" + String(marcstate) + ",";
    p += "\"packets\":" + String(totalPackets) + ",";
    p += "\"sensors\":" + String(activeSensors);
    p += "}";
    return p;
}
