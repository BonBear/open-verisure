/**
 * @file ha_discovery.cpp
 * @brief Home Assistant MQTT Auto-Discovery Implementation
 */

#include "ha_discovery.h"

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
        "Batteri",
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
        "Batterispänning",
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
        "Signalstyrka",
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
        "Radiobrus",
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
        "Mottagna paket",
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
        "Aktiva sensorer",
        nullptr,
        "measurement",
        "st",
        "{{ value_json.sensors }}",
        nullptr,
        nullptr,
        "diagnostic"
    }
};

static const int NUM_SENSOR_ENTITIES = sizeof(S_SENSOR_ENTITIES) / sizeof(S_SENSOR_ENTITIES[0]);
static const int NUM_GATEWAY_ENTITIES = sizeof(S_GATEWAY_ENTITIES) / sizeof(S_GATEWAY_ENTITIES[0]);

int haDiscoveryGetSensorEntityCount() {
    return NUM_SENSOR_ENTITIES;
}

const HaEntityDef* haDiscoveryGetSensorEntityDef(int idx) {
    if (idx < 0 || idx >= NUM_SENSOR_ENTITIES) return nullptr;
    return &S_SENSOR_ENTITIES[idx];
}

String haDiscoveryBuildTopic(const char* deviceId, const HaEntityDef* def) {
    return "homeassistant/" + String(def->component) + "/verisure_" + String(deviceId) + "_" + String(def->key) + "/config";
}

String haDiscoveryBuildPayload(const char* deviceId, const HaEntityDef* def, const char* baseTopic) {
    String stateTopic = haStateBuildTopic(deviceId, baseTopic);
    String uniqueId = "verisure_" + String(deviceId) + "_" + String(def->key);

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

    String availTopic = String(baseTopic) + "/status";
    p += "\"availability_topic\":\"" + availTopic + "\",";
    p += "\"payload_available\":\"online\",";
    p += "\"payload_not_available\":\"offline\",";

    // Home Assistant Device Registry Block
    p += "\"device\":{";
    p += "\"identifiers\":[\"verisure_" + String(deviceId) + "\"],";
    p += "\"name\":\"Verisure Sensor " + String(deviceId) + "\",";
    p += "\"model\":\"Door/Window Magnetic & Vibration Sensor\",";
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
    p += "\"vibration\":\"" + String(telem.isShock ? "ON" : "OFF") + "\",";
    p += "\"button\":\"" + String(telem.isButton ? "ON" : "OFF") + "\",";
    p += "\"power_on\":\"" + String(telem.isPowerOn ? "ON" : "OFF") + "\",";
    p += "\"battery_pct\":" + String(telem.batteryPct) + ",";
    p += "\"battery_mv\":" + String(telem.batteryMv) + ",";
    p += "\"low_battery\":\"" + String(telem.isLowBatt ? "ON" : "OFF") + "\",";
    p += "\"rssi\":" + String(telem.rssi) + ",";
    p += "\"lqi\":" + String(telem.lqi) + ",";
    p += "\"seq\":" + String(telem.sequence);
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
