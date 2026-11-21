/**
 * @file mqtt_manager.cpp
 * @brief MQTT Client State Machine & Message Publisher Implementation
 */

#include "mqtt_manager.h"
#include "config_manager.h"
#include "sensor_registry.h"
#include "ha_discovery.h"
#include "downlink.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <GuardedSocket.h>   // gwsWriteBounded (lib/GuardedWebServer)

#define MQTT_WRITE_STALL_MS 2000   // a broker that accepts no byte for this long is dropped

/**
 * The broker connection. NetworkClient::write() waits up to 10 x 1 s for a broker that stopped
 * reading, longer than the 5 s loop watchdog; here a write without progress for
 * MQTT_WRITE_STALL_MS closes the connection instead (PubSubClient then reports it as lost and the
 * reconnect back-off takes over). The loop watchdog is fed while the broker keeps up.
 */
#define MQTT_WRITE_TOTAL_MS 20000  // one write (e.g. a picture publish) may take at most this long
#define MQTT_READ_MAX_PASS  4096   // bytes read per mqttLoop() pass before the broker is dropped

class MqttSocket : public WiFiClient {
public:
    using WiFiClient::write;
    using WiFiClient::read;
    size_t write(const uint8_t* buf, size_t size) override {
        return gwsWriteBounded(*this, buf, size, MQTT_WRITE_STALL_MS, MQTT_WRITE_TOTAL_MS);
    }
    size_t write(uint8_t b) override { return write(&b, 1); }
    // PubSubClient reads a whole announced message byte by byte without returning, keeping only the
    // first MQTT_MAX_PACKET_SIZE bytes. Every message the gateway subscribes to is tiny, so a pass
    // that reads more than MQTT_READ_MAX_PASS bytes (an oversized, perhaps retained, message on a
    // command topic) drops the connection instead of reading megabytes past the loop watchdog;
    // PubSubClient then reports the connection lost and the back-off takes over.
    int read() override {
        if (!passBudget(1)) return -1;
        return WiFiClient::read();
    }
    int read(uint8_t* buf, size_t size) override {
        if (!passBudget(size)) return -1;
        return WiFiClient::read(buf, size);
    }
    void startPass() { m_passBytes = 0; }

private:
    size_t m_passBytes = 0;
    bool passBudget(size_t n) {
        m_passBytes += n;
        if (m_passBytes <= MQTT_READ_MAX_PASS) return true;
        if (connected()) {
            Serial.printf("[MQTT] Incoming message over %u bytes: dropping the broker connection\n",
                          (unsigned)MQTT_READ_MAX_PASS);
            stop();
        }
        return false;
    }
};

static MqttSocket s_wifiClient;
static PubSubClient s_mqttClient(s_wifiClient);

/* ---- Broker host name lookup ----
 * A lookup (getaddrinfo, also .local via mDNS) blocks ~7 s when the DNS server is silent, longer
 * than the 5 s loop watchdog, so it never runs in loop(): one helper task, not subscribed to the
 * watchdog, does it and posts the result. The last good address is cached and used for the TCP
 * connect (by IP: no lookup there); a failed connect starts a refresh in the background. */
static unsigned long s_lastReconnectAttempt = 0;
static unsigned long s_reconnectIntervalMs = 5000;

enum { DNS_IDLE = 0, DNS_RUNNING, DNS_OK, DNS_FAILED };
// The helper task is created once on first use from the heap and never deleted: a static 5 KB stack
// did not fit .dram0.bss (internal RAM), and one boot-time allocation does not fragment the heap.
#define DNS_TASK_STACK 5120                         // bytes in ESP-IDF FreeRTOS
static TaskHandle_t s_dnsTask = nullptr;
static char s_dnsHost[65];                         // host of the running or last lookup (config size)
static IPAddress s_dnsResult;                      // written by the task before s_dnsState becomes DNS_OK
static volatile uint8_t s_dnsState = DNS_IDLE;
static char s_cachedHost[65];
static IPAddress s_cachedIp;
static bool s_cachedValid = false;

static void dnsTaskBody(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        IPAddress ip;
        const bool ok = (WiFi.hostByName(s_dnsHost, ip) == 1);
        s_dnsResult = ip;
        s_dnsState = ok ? DNS_OK : DNS_FAILED;
    }
}

/** Starts a lookup unless one is running; never blocks. */
static void dnsStart(const char* host) {
    if (s_dnsState != DNS_IDLE) return;
    strncpy(s_dnsHost, host, sizeof(s_dnsHost) - 1);   // set first: a failed task creation below names it too
    s_dnsHost[sizeof(s_dnsHost) - 1] = '\0';
    if (!s_dnsTask) {
        if (xTaskCreate(dnsTaskBody, "mqttdns", DNS_TASK_STACK, nullptr, 1, &s_dnsTask) != pdPASS) {
            s_dnsTask = nullptr;
            static bool s_logged = false;
            if (!s_logged) {
                s_logged = true;
                Serial.println("[MQTT] ERROR: could not create the DNS lookup task (no heap?); use an IP address for the broker");
            }
            s_dnsState = DNS_FAILED;   // counts as a failed lookup: the back-off applies
            return;
        }
    }
    s_dnsState = DNS_RUNNING;
    Serial.printf("[MQTT] Looking up broker host %s...\n", host);
    xTaskNotifyGive(s_dnsTask);
}

/** Takes over a finished lookup (cheap, call on every pass). A failed lookup without a cached
 *  address counts as a failed connection attempt for the back-off. */
static void dnsCollect(const char* host) {
    const uint8_t state = s_dnsState;
    if (state != DNS_OK && state != DNS_FAILED) return;
    const bool same = (strcmp(s_dnsHost, host) == 0);   // the setting may have changed meanwhile
    const bool ok = (state == DNS_OK);
    if (same && ok) {
        s_cachedIp = s_dnsResult;
        strncpy(s_cachedHost, host, sizeof(s_cachedHost) - 1);
        s_cachedHost[sizeof(s_cachedHost) - 1] = '\0';
        s_cachedValid = true;
        Serial.printf("[MQTT] Broker host %s is %s\n", host, s_cachedIp.toString().c_str());
    } else if (same) {
        if (s_cachedValid && strcmp(s_cachedHost, host) == 0) {
            Serial.printf("[MQTT] Cannot resolve broker host %s, keeping the last address\n", host);
        } else {
            s_lastReconnectAttempt = millis();
            s_reconnectIntervalMs = (s_reconnectIntervalMs * 2 > 60000UL) ? 60000UL : s_reconnectIntervalMs * 2;
            Serial.printf("[MQTT] Cannot resolve broker host %s, next try in %lu s\n", host, s_reconnectIntervalMs / 1000);
        }
    }
    s_dnsState = DNS_IDLE;
}

/** @return true with the broker's address in *ip; false while a lookup is needed (started here). */
static bool brokerAddress(const char* host, IPAddress* ip) {
    if (ip->fromString(host)) return true;               // an IP address needs no lookup
    if (s_cachedValid && strcmp(s_cachedHost, host) == 0) {
        *ip = s_cachedIp;
        return true;
    }
    dnsStart(host);
    return false;
}

/** After a failed connection: look the host name up again in the background (the broker may have
 *  a new address); the cached one stays in use until the new one is known. */
static void dnsRefresh(const char* host) {
    IPAddress literal;
    if (!literal.fromString(host)) dnsStart(host);
}

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
/* ---- Camera downlink: alarm state and picture requests (see downlink.h) ---- */

static String gatewayDeviceJson() {
    return "\"device\":{\"identifiers\":[\"verisure_gateway\"],\"name\":\"Verisure 868 Gateway\"}";
}

static void publishAlarmState() {
    const GatewayConfig& cfg = ConfigManager::get();
    String t = String(cfg.mqttTopic) + "/alarm/state";
    s_mqttClient.publish(t.c_str(), downlinkArmed() ? "ON" : "OFF", true);
}

void mqttPublishAlarmState() {
    if (s_mqttClient.connected()) publishAlarmState();   // on reconnect it is sent anyway
}

/** Switch on the gateway device: the alarm state the cameras get. Its command topic also
 *  takes Home Assistant alarm_control_panel states (armed_*, triggered, disarmed). */
static void publishAlarmDiscovery(const char* baseTopic) {
    String b(baseTopic);
    String p = "{";
    p += "\"name\":\"Alarm (cameras)\",";
    p += "\"unique_id\":\"verisure_gateway_alarm\",";
    p += "\"icon\":\"mdi:shield-home\",";
    p += "\"command_topic\":\"" + b + "/alarm/set\",";
    p += "\"state_topic\":\"" + b + "/alarm/state\",";
    p += "\"availability_topic\":\"" + b + "/status\",";
    p += gatewayDeviceJson();
    p += "}";
    s_mqttClient.publish("homeassistant/switch/verisure_gateway_alarm/config", p.c_str(), true);
}

/** Button on a camera device that takes a picture (sent with the camera's next poll):
 *  suffix "" = 640x480 (D2), "_flash" = 320x240 grayscale lit by the flash (D4). */
static void publishPictureButtonDiscovery(const char* deviceId, const char* baseTopic, const char* suffix,
                                          const char* name, const char* icon, const char* payload) {
    String id(deviceId), b(baseTopic);
    String p = "{";
    p += "\"name\":\"" + String(name) + "\",";
    p += "\"unique_id\":\"verisure_" + id + "_picture" + suffix + "\",";
    p += "\"icon\":\"" + String(icon) + "\",";
    p += "\"command_topic\":\"" + b + "/" + id + "/picture/set\",";
    p += "\"payload_press\":\"" + String(payload) + "\",";
    p += "\"availability_topic\":\"" + b + "/status\",";
    p += "\"device\":{\"identifiers\":[\"verisure_" + id + "\"]}";
    p += "}";
    String topic = "homeassistant/button/verisure_" + id + "_picture" + suffix + "/config";
    s_mqttClient.publish(topic.c_str(), p.c_str(), true);
}

/** ON/OFF from the switch, or an alarm_control_panel state. -1 = no change (arming, pending). */
static int parseAlarmPayload(const String& v) {
    if (v == "ON" || v.startsWith("armed") || v == "triggered") return 1;
    if (v == "OFF" || v == "disarmed") return 0;
    return -1;
}

static bool isDeviceIdText(const String& s) {
    if (s.length() != 8) return false;
    for (unsigned int i = 0; i < 8; i++) {
        if (!isalnum((unsigned char)s[i])) return false;
    }
    return true;
}

static void onMqttMessage(char* topic, byte* payload, unsigned int len) {
    const GatewayConfig& cfg = ConfigManager::get();
    String base = String(cfg.mqttTopic) + "/";
    String t(topic);
    if (!t.startsWith(base) || len > 32) return;
    String v;
    for (unsigned int i = 0; i < len; i++) v += (char)payload[i];
    v.trim();
    String rest = t.substring(base.length());

    if (rest == "alarm/set") {
        int a = parseAlarmPayload(v);
        if (a < 0) return;
        downlinkSetArmed(a == 1);
        publishAlarmState();
        Serial.printf("[Downlink] Alarm %s (MQTT \"%s\")\n", a ? "ARMED" : "DISARMED", v.c_str());
        return;
    }
    const String suffix = "/picture/set";
    if (rest.endsWith(suffix)) {
        String dev = rest.substring(0, rest.length() - suffix.length());
        if (!isDeviceIdText(dev) || !haIsMotionCameraSensor(dev.c_str())) return;
        uint8_t cmd = 0xD2;
        if (v == "D1") cmd = 0xD1;
        else if (v == "D3") cmd = 0xD3;
        else if (v == "D4") cmd = 0xD4;
        else if (v == "D5") cmd = 0xD5;     // as D3 with light, flash picture in the dark
        else if (v == "D6") cmd = 0xD6;     // as D2 with light, flash picture in the dark
        else if (v == "HOLD") cmd = 0xE8;   // service: Blackfin on for 10 min, no picture (JTAG)
        bool ok = downlinkRequestPicture(dev.c_str(), cmd);
        Serial.printf("[Downlink] Picture 0x%02X for %s %s\n", cmd, dev.c_str(), ok ? "queued" : "NOT queued");
    }
}

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
        publishPictureButtonDiscovery(deviceId, baseTopic, "", "Take 640x480 picture", "mdi:camera", "D2");
        publishPictureButtonDiscovery(deviceId, baseTopic, "_flash", "Take flash picture", "mdi:camera-flash", "D4");

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
    s_mqttClient.setCallback(onMqttMessage);
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

    dnsCollect(cfg.mqttHost);   // takes over a finished host name lookup (a failure paces the retries below)

    // Everything below blocks the radio loop, so keep each attempt short and back off
    // (5 s, doubling to 60 s) while the broker stays unreachable.
    unsigned long now = millis();
    if (now - s_lastReconnectAttempt < s_reconnectIntervalMs) return;

    // The host name is resolved in the background (no blocking lookup in loop()); an attempt only
    // starts once the address is known, and a pending lookup does not use up an attempt.
    IPAddress brokerIp;
    if (!brokerAddress(cfg.mqttHost, &brokerIp)) return;

    s_lastReconnectAttempt = now;
    s_reconnectIntervalMs = (s_reconnectIntervalMs * 2 > 60000UL) ? 60000UL : s_reconnectIntervalMs * 2;

    s_mqttClient.setServer(brokerIp, cfg.mqttPort);
    s_mqttClient.setSocketTimeout(2);   // CONNACK wait, seconds (default 15)
    feedLoopWDT();   // a fresh window; worst case below: 1.5 s TCP connect + 2 s CONNACK wait, then the discovery publishes (each write bounded)

    // Open TCP ourselves, by IP address and with a bounded timeout; PubSubClient reuses a connected client.
    if (!s_wifiClient.connected() && !s_wifiClient.connect(brokerIp, cfg.mqttPort, 1500)) {
        Serial.printf("[MQTT] Broker %s:%d not reachable, next try in %lu s\n",
                      cfg.mqttHost, cfg.mqttPort, s_reconnectIntervalMs / 1000);
        dnsRefresh(cfg.mqttHost);
        return;
    }
    // (No s_wifiClient.setTimeout() here: in core 3.x that is Stream::setTimeout() in milliseconds
    // and only affects readBytes(), which PubSubClient does not use. A write that stalls is bounded
    // by MqttSocket::write(); the CONNACK wait by setSocketTimeout() above.)
    feedLoopWDT();   // between the TCP connect and the MQTT CONNECT

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
        s_reconnectIntervalMs = 5000;
        feedLoopWDT();

        // Publish Gateway Online Status (Retained)
        s_mqttClient.publish(statusTopic.c_str(), "online", true);

        // Immediately publish gateway discovery and announce all known sensors
        publishGatewayDiscovery(cfg.mqttTopic);
        feedLoopWDT();
        publishAlarmDiscovery(cfg.mqttTopic);
        publishAlarmState();
        String alarmSet = String(cfg.mqttTopic) + "/alarm/set";
        String pictureSet = String(cfg.mqttTopic) + "/+/picture/set";
        s_mqttClient.subscribe(alarmSet.c_str());
        s_mqttClient.subscribe(pictureSet.c_str());

        int count = sensorRegistryGetCount();
        for (int i = 0; i < count; i++) {
            const SensorRecord* rec = sensorRegistryGetByIndex(i);
            if (rec && rec->active) {
                publishSensorDiscovery(rec->lastTelem.deviceId, cfg.mqttTopic);
                feedLoopWDT();   // one sensor = up to ~20 publishes
            }
        }
    } else {
        Serial.printf("[MQTT] Connection error! rc=%d, next try in %lu s\n",
                      s_mqttClient.state(), s_reconnectIntervalMs / 1000);
        s_wifiClient.stop();
        dnsRefresh(cfg.mqttHost);
    }
}

void mqttLoop(int currentRssi, uint8_t marcstate, uint32_t totalPackets) {
    feedLoopWDT();   // a fresh window: the socket reads below wait at most 2 s, every write at most 2 s without progress
    s_wifiClient.startPass();   // read budget per pass (MQTT_READ_MAX_PASS)
    checkMqttConnection();

    if (s_mqttClient.connected()) {
        s_mqttClient.loop();

        // Periodic Gateway heartbeat publishing every 30 seconds
        unsigned long now = millis();
        if (now - s_lastGatewayStatePub >= 30000 || s_lastGatewayStatePub == 0) {
            s_lastGatewayStatePub = now;
            const GatewayConfig& cfg = ConfigManager::get();
            String topic = String(cfg.mqttTopic) + "/gateway/state";
            String payload = haStateBuildGatewayPayload(currentRssi, marcstate, totalPackets, sensorRegistryGetCount(),
                                                        gatewayDiagSample());
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

    if (!mqttCameraImageLengthOk(topic.length(), len)) {
        Serial.printf("[MQTT] Refusing Camera Image (%u bytes, topic %u B): exceeds PubSubClient 16-bit length limit\n",
                      (unsigned int)len, (unsigned int)topic.length());
        return false;
    }

    // Use PubSubClient streaming publish to support images larger than MQTT_MAX_PACKET_SIZE
    bool ok = s_mqttClient.beginPublish(topic.c_str(), len, true);
    if (ok) {
        // In pieces, feeding the loop task watchdog between them: a ~35 KB picture to a slow broker
        // link can take longer than the 5 s watchdog although it keeps moving. (MqttSocket drops a
        // broker that accepts nothing for MQTT_WRITE_STALL_MS, or one write after MQTT_WRITE_TOTAL_MS.)
        size_t written = 0;
        while (written < len) {
            const size_t n = (len - written > 1024) ? 1024 : (len - written);
            const size_t w = s_mqttClient.write(jpegBuf + written, n);
            written += w;
            feedLoopWDT();
            if (w != n) break;
        }
        ok = s_mqttClient.endPublish();
        if (ok && written == len) {
            Serial.printf("[MQTT] >> Successfully published Camera Image (%u bytes) to %s\n", (unsigned int)len, topic.c_str());
            return true;
        }
    }
    Serial.printf("[MQTT] Error publishing Camera Image (%u bytes) to %s\n", (unsigned int)len, topic.c_str());
    return false;
}
