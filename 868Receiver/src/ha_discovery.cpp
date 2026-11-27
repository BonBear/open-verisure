/**
 * @file ha_discovery.cpp
 * @brief Home Assistant MQTT Auto-Discovery Implementation
 */

#include "ha_discovery.h"
#include "smoke_packet.h"

/** Seconds without a packet before Home Assistant shows a camera's entities as unavailable
 *  (the camera polls every ~30 s, every ~2 minutes when disarmed and idle). */
#define HA_CAMERA_EXPIRE_S 600
/** The same for the door sensor: heartbeat every 3600 s (60.0 min apart since its sleep tick
 *  fix); 2.5 h is two missed heartbeats plus margin. */
#define HA_SENSOR_EXPIRE_S 9000
/** The same for the smoke detector. Same value as the door sensor for now: the production
 *  heartbeat interval is not decided yet. */
#define HA_SMOKE_EXPIRE_S HA_SENSOR_EXPIRE_S

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
        // the door sensor sets its low-battery flag below 2.4 V (VerisureDoorSensor/main.c)
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
 * @brief Verisure Siren (GW-SI) diagnostic/telemetry entity configuration table.
 */
static const HaEntityDef S_SIREN_ENTITIES[] = {
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
        "temperature",
        "sensor",
        "Temperature",
        "temperature",
        "measurement",
        "°C",
        "{{ value_json.temperature_c if value_json.temperature_c is defined else None }}",
        nullptr,
        nullptr,
        nullptr
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
 * @brief Verisure Smoke Detector (GW-SD3) entity configuration table.
 */
static const HaEntityDef S_SMOKE_ENTITIES[] = {
    {
        "smoke",
        "binary_sensor",
        "Smoke",
        "smoke",
        nullptr,
        nullptr,
        "{{ value_json.smoke }}",
        "ON",
        "OFF",
        nullptr
    },
    {
        "chamber_fault",
        "binary_sensor",
        "Chamber Fault",
        "problem",
        nullptr,
        nullptr,
        "{{ value_json.chamber_fault }}",
        "ON",
        "OFF",
        "diagnostic"
    },
    {
        "maintenance",
        "binary_sensor",
        "Chamber Dirty",
        "problem",
        nullptr,
        nullptr,
        "{{ value_json.maintenance }}",
        "ON",
        "OFF",
        "diagnostic"
    },
    {
        "test",
        "binary_sensor",
        "Test Button",
        nullptr,
        nullptr,
        nullptr,
        "{{ value_json.test }}",
        "ON",
        "OFF",
        "diagnostic"
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
    },
    {
        "temperature",
        "sensor",
        "Temperature",
        "temperature",
        "measurement",
        "°C",
        "{{ value_json.temperature_c if value_json.temperature_c is defined else None }}",
        nullptr,
        nullptr,
        nullptr
    },
    {
        "humidity",
        "sensor",
        "Humidity",
        "humidity",
        "measurement",
        "%",
        "{{ value_json.humidity_pct if value_json.humidity_pct is defined else None }}",
        nullptr,
        nullptr,
        nullptr
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
    },
    // Diagnostics since the last boot (gateway_diag.h): counters restart at 0 after a reboot
    {
        "uptime",
        "sensor",
        "Uptime",
        "duration",
        "total_increasing",
        "s",
        "{{ value_json.uptime }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "reset_reason",
        "sensor",
        "Last Reset Reason",
        nullptr,
        nullptr,
        nullptr,
        "{{ value_json.reset_reason }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "free_heap",
        "sensor",
        "Free Heap",
        "data_size",
        "measurement",
        "B",
        "{{ value_json.free_heap }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "min_free_heap",
        "sensor",
        "Minimum Free Heap",
        "data_size",
        "measurement",
        "B",
        "{{ value_json.min_free_heap }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "rx_crc_ok",
        "sensor",
        "Packets CRC OK",
        nullptr,
        "total_increasing",
        "pkts",
        "{{ value_json.rx_crc_ok }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "auth_ok",
        "sensor",
        "Packets Authenticated",
        nullptr,
        "total_increasing",
        "pkts",
        "{{ value_json.auth_ok }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "auth_fail",
        "sensor",
        "Packets Rejected (Authentication)",
        nullptr,
        "total_increasing",
        "pkts",
        "{{ value_json.auth_fail }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "replay_drop",
        "sensor",
        "Packets Dropped (Replay)",
        nullptr,
        "total_increasing",
        "pkts",
        "{{ value_json.replay_drop }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "pic_stale",
        "sensor",
        "Picture Chunks Dropped (Stale)",
        nullptr,
        "total_increasing",
        "chunks",
        "{{ value_json.pic_stale }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "session_full",
        "sensor",
        "Session Table Full",
        nullptr,
        "total_increasing",
        "events",
        "{{ value_json.session_full }}",
        nullptr,
        nullptr,
        "diagnostic"
    },
    {
        "last_rejected_id",
        "sensor",
        "Last Rejected Device",
        nullptr,
        nullptr,
        nullptr,
        "{{ value_json.last_rejected_id }}",
        nullptr,
        nullptr,
        "diagnostic"
    }
};

static const int NUM_SENSOR_ENTITIES = sizeof(S_SENSOR_ENTITIES) / sizeof(S_SENSOR_ENTITIES[0]);
static const int NUM_MOTION_SENSOR_ENTITIES = sizeof(S_MOTION_SENSOR_ENTITIES) / sizeof(S_MOTION_SENSOR_ENTITIES[0]);
static const int NUM_SIREN_ENTITIES = sizeof(S_SIREN_ENTITIES) / sizeof(S_SIREN_ENTITIES[0]);
static const int NUM_SMOKE_ENTITIES = sizeof(S_SMOKE_ENTITIES) / sizeof(S_SMOKE_ENTITIES[0]);
static const int NUM_GATEWAY_ENTITIES = sizeof(S_GATEWAY_ENTITIES) / sizeof(S_GATEWAY_ENTITIES[0]);

bool haIsMotionCameraSensor(const char* deviceId) {
    return deviceId && strncmp(deviceId, "2NCE", 4) == 0;
}

bool haIsSirenSensor(const char* deviceId) {
    return deviceId && strncmp(deviceId, "2S5F", 4) == 0;
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

int haDiscoveryGetSirenEntityCount() {
    return NUM_SIREN_ENTITIES;
}

const HaEntityDef* haDiscoveryGetSirenEntityDef(int idx) {
    if (idx < 0 || idx >= NUM_SIREN_ENTITIES) return nullptr;
    return &S_SIREN_ENTITIES[idx];
}

int haDiscoveryGetSmokeEntityCount() {
    return NUM_SMOKE_ENTITIES;
}

const HaEntityDef* haDiscoveryGetSmokeEntityDef(int idx) {
    if (idx < 0 || idx >= NUM_SMOKE_ENTITIES) return nullptr;
    return &S_SMOKE_ENTITIES[idx];
}

String haDiscoveryBuildTopic(const char* deviceId, const HaEntityDef* def) {
    return "homeassistant/" + String(def->component) + "/verisure_" + String(deviceId) + "_" + String(def->key) + "/config";
}

String haDiscoveryBuildSirenTopic(const char* deviceId) {
    return "homeassistant/siren/verisure_" + String(deviceId) + "_siren/config";
}

String haDiscoveryBuildSirenPayload(const char* deviceId, const char* baseTopic) {
    String b(baseTopic);
    String id(deviceId);
    String stateTopic = haStateBuildTopic(deviceId, baseTopic);
    String cmdTopic = b + "/" + id + "/siren/set";
    String uniqueId = "verisure_" + id + "_siren";

    String p = "{";
    p += "\"name\":\"Siren\",";
    p += "\"unique_id\":\"" + uniqueId + "\",";
    p += "\"command_topic\":\"" + cmdTopic + "\",";
    p += "\"state_topic\":\"" + stateTopic + "\",";
    p += "\"value_template\":\"{{ value_json.siren }}\",";
    p += "\"payload_on\":\"ON\",";
    p += "\"payload_off\":\"OFF\",";
    p += "\"support_duration\":true,";
    p += "\"available_tones\":[\"alarm\",\"ding_dong\",\"chime\",\"double_beep\",\"beep\",\"tone\"],";
    p += "\"expire_after\":" + String(HA_CAMERA_EXPIRE_S) + ",";
    p += "\"availability_topic\":\"" + b + "/status\",";
    p += "\"payload_available\":\"online\",";
    p += "\"payload_not_available\":\"offline\",";
    p += "\"device\":{";
    p += "\"identifiers\":[\"verisure_" + id + "\"],";
    p += "\"name\":\"Verisure Siren " + id + "\",";
    p += "\"model\":\"Verisure Siren (GW-SI)\",";
    p += "\"manufacturer\":\"Verisure\",";
    p += "\"sw_version\":\"1.0-MSP430\",";
    p += "\"via_device\":\"verisure_gateway\"";
    p += "}}";
    return p;
}

String haDiscoveryBuildSirenFastWakeTopic(const char* deviceId) {
    return "homeassistant/switch/verisure_" + String(deviceId) + "_fast_wake/config";
}

String haDiscoveryBuildSirenFastWakePayload(const char* deviceId, const char* baseTopic) {
    String b(baseTopic);
    String id(deviceId);
    String stateTopic = haStateBuildTopic(deviceId, baseTopic);
    String cmdTopic = b + "/" + id + "/siren/fast_wake/set";
    String uniqueId = "verisure_" + id + "_fast_wake";

    String p = "{";
    p += "\"name\":\"Fast Wake (Doorbell)\",";
    p += "\"unique_id\":\"" + uniqueId + "\",";
    p += "\"icon\":\"mdi:bell-ring-outline\",";
    p += "\"entity_category\":\"config\",";
    p += "\"command_topic\":\"" + cmdTopic + "\",";
    p += "\"state_topic\":\"" + stateTopic + "\",";
    p += "\"value_template\":\"{{ value_json.fast_wake if value_json.fast_wake is defined else None }}\",";
    p += "\"payload_on\":\"ON\",";
    p += "\"payload_off\":\"OFF\",";
    p += "\"expire_after\":" + String(HA_CAMERA_EXPIRE_S) + ",";
    p += "\"availability_topic\":\"" + b + "/status\",";
    p += "\"payload_available\":\"online\",";
    p += "\"payload_not_available\":\"offline\",";
    p += "\"device\":{";
    p += "\"identifiers\":[\"verisure_" + id + "\"],";
    p += "\"name\":\"Verisure Siren " + id + "\",";
    p += "\"model\":\"Verisure Siren (GW-SI)\",";
    p += "\"manufacturer\":\"Verisure\",";
    p += "\"sw_version\":\"1.0-MSP430\",";
    p += "\"via_device\":\"verisure_gateway\"";
    p += "}}";
    return p;
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

String haDiscoveryBuildPayload(const char* deviceId, const HaEntityDef* def, const char* baseTopic, bool isSmoke) {
    String stateTopic = haStateBuildTopic(deviceId, baseTopic);
    String uniqueId = "verisure_" + String(deviceId) + "_" + String(def->key);
    bool isMotionCam = haIsMotionCameraSensor(deviceId);
    bool isSiren = haIsSirenSensor(deviceId);
    if (isMotionCam || isSiren) isSmoke = false;   // the ID prefix wins, as in main.cpp

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
    // Without packets for this long the entities turn unavailable (dead battery, out of range)
    // instead of keeping old values: cameras and sirens poll at least every ~2 minutes, the door
    // sensor sends a heartbeat about every hour
    p += "\"expire_after\":" + String((isMotionCam || isSiren) ? HA_CAMERA_EXPIRE_S
                                      : isSmoke ? HA_SMOKE_EXPIRE_S : HA_SENSOR_EXPIRE_S) + ",";

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
        p += "\"sw_version\":\"2.0-CC1110\",";
    } else if (isSiren) {
        p += "\"name\":\"Verisure Siren " + String(deviceId) + "\",";
        p += "\"model\":\"Verisure Siren (GW-SI)\",";
        p += "\"sw_version\":\"1.0-MSP430\",";
    } else if (isSmoke) {
        p += "\"name\":\"Verisure Smoke Detector " + String(deviceId) + "\",";
        p += "\"model\":\"GW-SD3 (Smoke Detector)\",";
        p += "\"sw_version\":\"1.0-EFR32FG14\",";
    } else {
        p += "\"name\":\"Verisure Sensor " + String(deviceId) + "\",";
        p += "\"model\":\"GW-MC-SHOCK (Door/Window & Shock)\",";
        p += "\"sw_version\":\"2.0-CC1110\",";
    }
    p += "\"manufacturer\":\"Verisure\",";
    p += "\"via_device\":\"verisure_gateway\"";
    p += "}}";

    return p;
}

String haStateBuildTopic(const char* deviceId, const char* baseTopic) {
    return String(baseTopic) + "/" + String(deviceId) + "/state";
}

String haStateBuildPayload(const VerisureTelemetry& telem) {
    String p = "{";
    // The camera and siren ID prefixes win over the in-band smoke marker (as in main.cpp)
    const bool smoke = telem.isSmoke && !haIsMotionCameraSensor(telem.deviceId) && !haIsSirenSensor(telem.deviceId);
    if (smoke) {
        p += "\"smoke\":\"" + String(telem.isOpen ? "ON" : "OFF") + "\",";   // flags bit 0
        p += "\"chamber_fault\":\"" + String((telem.smokeStatus & SMOKE_STATUS_FAULT) ? "ON" : "OFF") + "\",";
        p += "\"maintenance\":\"" + String((telem.smokeStatus & SMOKE_STATUS_DIRTY) ? "ON" : "OFF") + "\",";
        p += "\"test\":\"" + String((telem.smokeStatus & SMOKE_STATUS_TEST) ? "ON" : "OFF") + "\",";
        p += "\"hushed\":\"" + String((telem.smokeStatus & SMOKE_STATUS_HUSHED) ? "ON" : "OFF") + "\",";
    } else {
        p += "\"contact\":\"" + String(telem.isOpen ? "OPEN" : "CLOSED") + "\",";
        p += "\"motion\":\"" + String(telem.isOpen ? "ON" : "OFF") + "\",";
        p += "\"vibration\":\"" + String(telem.isShock ? "ON" : "OFF") + "\",";
    }
    p += "\"button\":\"" + String(telem.isButton ? "ON" : "OFF") + "\",";
    p += "\"power_on\":\"" + String(telem.isPowerOn ? "ON" : "OFF") + "\",";
    p += "\"battery_pct\":" + String(telem.batteryPct) + ",";
    p += "\"battery_mv\":" + String(telem.batteryMv) + ",";
    p += "\"low_battery\":\"" + String(telem.isLowBatt ? "ON" : "OFF") + "\",";
    if (haIsSirenSensor(telem.deviceId)) {
        p += "\"siren\":\"" + String(telem.isOpen ? "ON" : "OFF") + "\",";
        if (telem.fastWakeState != 0) {
            p += "\"fast_wake\":\"" + String(telem.fastWakeState == 2 ? "ON" : "OFF") + "\",";
        }
    }
    if (telem.hasTemp) {
        char tbuf[12];
        snprintf(tbuf, sizeof(tbuf), "%.1f", telem.tempC);
        p += "\"temperature_c\":" + String(tbuf) + ",";
    }
    if (smoke && telem.hasHumidity) p += "\"humidity_pct\":" + String(telem.humidityPct) + ",";
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
    p += "\"manufacturer\":\"OpenVerisure\",";
    p += "\"sw_version\":\"2.0.0\"";
    p += "}}";

    return p;
}

String haStateBuildGatewayPayload(int radioRssi, uint8_t marcstate, uint32_t totalPackets, int activeSensors,
                                  const GatewayDiagSample& diag) {
    String p = "{";
    p += "\"radio_rssi\":" + String(radioRssi) + ",";
    p += "\"marcstate\":" + String(marcstate) + ",";
    p += "\"packets\":" + String(totalPackets) + ",";
    p += "\"sensors\":" + String(activeSensors) + ",";
    gatewayDiagAppendJson(p, diag);
    p += "}";
    return p;
}
