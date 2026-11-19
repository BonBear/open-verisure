/**
 * @file ha_discovery.h
 * @brief Home Assistant MQTT Auto-Discovery Generator for Verisure Sensors & Gateway
 *
 * This component formats MQTT topics and JSON discovery payloads conforming to the
 * official Home Assistant Core MQTT Discovery Specification:
 * https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery
 *
 * Discovery topic format:
 *   `<discovery_prefix>/<component>/<node_id>/<object_id>/config`
 *   e.g.: `homeassistant/binary_sensor/verisure_2JHL2MVN_contact/config`
 *
 * Each sensor automatically exposes 6 distinct Home Assistant entities:
 * 1. Contact (Door Sensor, device_class: door) - binary_sensor
 * 2. Vibration (Shock / Break-in Sensor) - binary_sensor
 * 3. Tamper (Sabotage / Cover Sensor) - binary_sensor
 * 4. Battery (Battery Percentage 0-100%) - sensor
 * 5. Battery Voltage (Millivolts mV) - sensor
 * 6. Signal Strength (RF RSSI in dBm) - sensor
 */

#pragma once

#include <Arduino.h>
#include "crypto.h"
#include "gateway_diag.h"

/**
 * @struct HaEntityDef
 * @brief Metadata definition for an exposed Home Assistant entity.
 */
struct HaEntityDef {
    const char* key;         /**< JSON key identifier (e.g. "contact", "battery") */
    const char* component;   /**< HA platform ("binary_sensor" or "sensor") */
    const char* name;        /**< Friendly entity name displayed in HA UI */
    const char* deviceClass; /**< HA device class (e.g. "opening", "tamper", "battery") */
    const char* stateClass;  /**< HA state class ("measurement" for continuous gauges) */
    const char* unit;        /**< Unit of measurement ("%", "mV", "dBm") */
    const char* valTpl;      /**< Jinja2 value template extracting from state JSON */
    const char* plOn;        /**< Payload value representing ON/OPEN state */
    const char* plOff;       /**< Payload value representing OFF/CLOSED state */
    const char* category;    /**< Entity category ("diagnostic" or nullptr) */
};

/**
 * @brief Check if a sensor ID belongs to a Verisure Motion Detector Camera (GW MD CAM2).
 * @param deviceId 8-character sensor identifier.
 * @return True if device is a motion detector camera (e.g. "2NCE...").
 */
bool haIsMotionCameraSensor(const char* deviceId);

/**
 * @brief Get total number of entity definitions exposed per door/window sensor node.
 * @return Number of sensor entities (typically 6).
 */
int haDiscoveryGetSensorEntityCount();

/**
 * @brief Get door/window entity definition by zero-based index.
 * @param idx Index from 0 to count-1.
 * @return Pointer to HaEntityDef structure.
 */
const HaEntityDef* haDiscoveryGetSensorEntityDef(int idx);

/**
 * @brief Get total number of entity definitions exposed per motion camera sensor node.
 * @return Number of motion sensor entities (5).
 */
int haDiscoveryGetMotionSensorEntityCount();

/**
 * @brief Get motion camera entity definition by zero-based index.
 * @param idx Index from 0 to count-1.
 * @return Pointer to HaEntityDef structure.
 */
const HaEntityDef* haDiscoveryGetMotionSensorEntityDef(int idx);

/**
 * @brief Construct MQTT config discovery topic for a sensor entity.
 * @param deviceId 8-character sensor identifier.
 * @param def Entity definition pointer.
 * @return Full topic string: `homeassistant/<component>/verisure_<deviceId>_<key>/config`.
 */
String haDiscoveryBuildTopic(const char* deviceId, const HaEntityDef* def);

/**
 * @brief Construct MQTT config discovery topic for a Motion Detector Camera.
 * @param deviceId 8-character sensor identifier.
 * @return Full topic string: `homeassistant/camera/verisure_<deviceId>_camera/config`.
 */
String haDiscoveryBuildCameraTopic(const char* deviceId);

/**
 * @brief Construct JSON payload for Motion Detector Camera auto-discovery.
 * @param deviceId 8-character sensor identifier.
 * @param baseTopic Base MQTT topic (e.g. "verisure").
 * @return JSON configuration string for Home Assistant MQTT Camera entity.
 */
String haDiscoveryBuildCameraPayload(const char* deviceId, const char* baseTopic);

/**
 * @brief Construct JSON payload for sensor entity auto-discovery.
 * @param deviceId 8-character sensor identifier.
 * @param def Entity definition pointer.
 * @param baseTopic Base MQTT topic configured on gateway (e.g. "verisure").
 * @return JSON configuration string with device metadata, identifiers, and connections.
 */
String haDiscoveryBuildPayload(const char* deviceId, const HaEntityDef* def, const char* baseTopic);

/**
 * @brief Construct telemetry state topic for a sensor node.
 * @param deviceId 8-character sensor identifier.
 * @param baseTopic Base MQTT topic (e.g. "verisure").
 * @return Full topic string: `<baseTopic>/<deviceId>/state`.
 */
String haStateBuildTopic(const char* deviceId, const char* baseTopic);

/**
 * @brief Serialize a VerisureTelemetry struct into a JSON state message.
 * @param telem Validated telemetry data.
 * @return JSON payload containing contact, vibration, button, battery, etc.
 */
String haStateBuildPayload(const VerisureTelemetry& telem);

/**
 * @brief Get total number of entity definitions exposed for the gateway itself.
 * @return Gateway entity count (RF RSSI, packet count, active sensors, diagnostics).
 */
int haDiscoveryGetGatewayEntityCount();

/**
 * @brief Get gateway entity definition by zero-based index.
 * @param idx Index from 0 to count-1.
 * @return Pointer to HaEntityDef structure.
 */
const HaEntityDef* haDiscoveryGetGatewayEntityDef(int idx);

/**
 * @brief Construct MQTT config discovery topic for a gateway entity.
 * @param def Entity definition pointer.
 * @return Full topic string.
 */
String haDiscoveryBuildGatewayTopic(const HaEntityDef* def);

/**
 * @brief Construct JSON payload for gateway entity auto-discovery.
 * @param def Entity definition pointer.
 * @param baseTopic Base MQTT topic.
 * @return JSON configuration string.
 */
String haDiscoveryBuildGatewayPayload(const HaEntityDef* def, const char* baseTopic);

/**
 * @brief Serialize gateway heartbeat and diagnostic metrics into a JSON state message.
 * @param radioRssi Background RF noise/signal in dBm.
 * @param marcstate Radio modem state register.
 * @param totalPackets Cumulative number of successfully received packets.
 * @param activeSensors Number of active sensors registered.
 * @param diag Uptime, reset reason and heap; the counters come from g_gatewayDiag.
 * @return JSON payload.
 */
String haStateBuildGatewayPayload(int radioRssi, uint8_t marcstate, uint32_t totalPackets, int activeSensors,
                                  const GatewayDiagSample& diag);
