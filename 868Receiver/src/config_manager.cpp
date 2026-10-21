/**
 * @file config_manager.cpp
 * @brief Persistent Configuration Storage Implementation using ESP32 Preferences
 */

#include "config_manager.h"
#include <Preferences.h>
#include <ctype.h>
#include <string.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif

#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS ""
#endif

#ifndef DEFAULT_MQTT_HOST
#define DEFAULT_MQTT_HOST ""
#endif

#ifndef DEFAULT_MQTT_PORT
#define DEFAULT_MQTT_PORT 1883
#endif

#ifndef DEFAULT_MQTT_USER
#define DEFAULT_MQTT_USER ""
#endif

#ifndef DEFAULT_MQTT_PASS
#define DEFAULT_MQTT_PASS ""
#endif

#ifndef DEFAULT_MQTT_TOPIC
#define DEFAULT_MQTT_TOPIC "verisure"
#endif

#ifndef DEFAULT_MASTER_KEY_HEX
#define DEFAULT_MASTER_KEY_HEX "0102030405060708090A0B0C0D0E0F10"
#endif

static Preferences s_prefs;
static GatewayConfig s_config;

void ConfigManager::bytesToHex(const uint8_t* in, int len, char* out) {
    static const char hexChars[] = "0123456789ABCDEF";
    for (int i = 0; i < len; i++) {
        out[i * 2]     = hexChars[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hexChars[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

static inline int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ConfigManager::hexToBytes(const char* in, uint8_t* out, int len) {
    if (!in || !out) return false;
    for (int i = 0; i < len; i++) {
        int hi = hexNibble(in[i * 2]);
        int lo = hexNibble(in[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

void ConfigManager::begin() {
    s_prefs.begin("gateway", false);

    // 1. Wi-Fi Settings
    String ssid = s_prefs.getString("wifi_ssid", DEFAULT_WIFI_SSID);
    String pass = s_prefs.getString("wifi_pass", DEFAULT_WIFI_PASS);
    strncpy(s_config.wifiSsid, ssid.c_str(), sizeof(s_config.wifiSsid) - 1);
    s_config.wifiSsid[sizeof(s_config.wifiSsid) - 1] = '\0';
    strncpy(s_config.wifiPass, pass.c_str(), sizeof(s_config.wifiPass) - 1);
    s_config.wifiPass[sizeof(s_config.wifiPass) - 1] = '\0';

    // 2. MQTT Settings
    String mHost = s_prefs.getString("mqtt_host", DEFAULT_MQTT_HOST);
    uint16_t mPort = s_prefs.getUShort("mqtt_port", DEFAULT_MQTT_PORT);
    String mUser = s_prefs.getString("mqtt_user", DEFAULT_MQTT_USER);
    String mPass = s_prefs.getString("mqtt_pass", DEFAULT_MQTT_PASS);
    String mTopic = s_prefs.getString("mqtt_topic", DEFAULT_MQTT_TOPIC);

    strncpy(s_config.mqttHost, mHost.c_str(), sizeof(s_config.mqttHost) - 1);
    s_config.mqttHost[sizeof(s_config.mqttHost) - 1] = '\0';
    s_config.mqttPort = mPort;
    strncpy(s_config.mqttUser, mUser.c_str(), sizeof(s_config.mqttUser) - 1);
    s_config.mqttUser[sizeof(s_config.mqttUser) - 1] = '\0';
    strncpy(s_config.mqttPass, mPass.c_str(), sizeof(s_config.mqttPass) - 1);
    s_config.mqttPass[sizeof(s_config.mqttPass) - 1] = '\0';
    strncpy(s_config.mqttTopic, mTopic.c_str(), sizeof(s_config.mqttTopic) - 1);
    s_config.mqttTopic[sizeof(s_config.mqttTopic) - 1] = '\0';

    // 3. Master Key (16 bytes)
    size_t keyLen = s_prefs.getBytes("master_key", s_config.masterKey, 16);
    if (keyLen != 16) {
        hexToBytes(DEFAULT_MASTER_KEY_HEX, s_config.masterKey, 16);
    }
    bytesToHex(s_config.masterKey, 16, s_config.masterKeyHex);

    Serial.println("[Config] Läste in inställningar:");
    Serial.printf("         Wi-Fi SSID  : %s\n", s_config.wifiSsid[0] ? s_config.wifiSsid : "(ej konfigurerat)");
    Serial.printf("         MQTT Broker : %s:%d (Topic: %s)\n",
                  s_config.mqttHost[0] ? s_config.mqttHost : "(ej konfigurerat)",
                  s_config.mqttPort, s_config.mqttTopic);
    Serial.printf("         Master Key  : %s\n", s_config.masterKeyHex);
}

const GatewayConfig& ConfigManager::get() {
    return s_config;
}

bool ConfigManager::save() {
    s_prefs.putString("wifi_ssid", s_config.wifiSsid);
    s_prefs.putString("wifi_pass", s_config.wifiPass);
    s_prefs.putString("mqtt_host", s_config.mqttHost);
    s_prefs.putUShort("mqtt_port", s_config.mqttPort);
    s_prefs.putString("mqtt_user", s_config.mqttUser);
    s_prefs.putString("mqtt_pass", s_config.mqttPass);
    s_prefs.putString("mqtt_topic", s_config.mqttTopic);
    s_prefs.putBytes("master_key", s_config.masterKey, 16);
    Serial.println("[Config] Inställningar sparade till NVS!");
    return true;
}

void ConfigManager::resetToDefaults() {
    s_prefs.clear();
    strncpy(s_config.wifiSsid, DEFAULT_WIFI_SSID, sizeof(s_config.wifiSsid) - 1);
    strncpy(s_config.wifiPass, DEFAULT_WIFI_PASS, sizeof(s_config.wifiPass) - 1);
    strncpy(s_config.mqttHost, DEFAULT_MQTT_HOST, sizeof(s_config.mqttHost) - 1);
    s_config.mqttPort = DEFAULT_MQTT_PORT;
    strncpy(s_config.mqttUser, DEFAULT_MQTT_USER, sizeof(s_config.mqttUser) - 1);
    strncpy(s_config.mqttPass, DEFAULT_MQTT_PASS, sizeof(s_config.mqttPass) - 1);
    strncpy(s_config.mqttTopic, DEFAULT_MQTT_TOPIC, sizeof(s_config.mqttTopic) - 1);
    hexToBytes(DEFAULT_MASTER_KEY_HEX, s_config.masterKey, 16);
    bytesToHex(s_config.masterKey, 16, s_config.masterKeyHex);
    save();
    Serial.println("[Config] NVS återställd till fabriksinställningar.");
}

void ConfigManager::setWifi(const char* ssid, const char* pass) {
    if (ssid) {
        strncpy(s_config.wifiSsid, ssid, sizeof(s_config.wifiSsid) - 1);
        s_config.wifiSsid[sizeof(s_config.wifiSsid) - 1] = '\0';
    }
    if (pass) {
        strncpy(s_config.wifiPass, pass, sizeof(s_config.wifiPass) - 1);
        s_config.wifiPass[sizeof(s_config.wifiPass) - 1] = '\0';
    }
}

void ConfigManager::setMqtt(const char* host, uint16_t port, const char* user, const char* pass, const char* topic) {
    if (host) {
        strncpy(s_config.mqttHost, host, sizeof(s_config.mqttHost) - 1);
        s_config.mqttHost[sizeof(s_config.mqttHost) - 1] = '\0';
    }
    if (port > 0) s_config.mqttPort = port;
    if (user) {
        strncpy(s_config.mqttUser, user, sizeof(s_config.mqttUser) - 1);
        s_config.mqttUser[sizeof(s_config.mqttUser) - 1] = '\0';
    }
    if (pass) {
        strncpy(s_config.mqttPass, pass, sizeof(s_config.mqttPass) - 1);
        s_config.mqttPass[sizeof(s_config.mqttPass) - 1] = '\0';
    }
    if (topic) {
        strncpy(s_config.mqttTopic, topic, sizeof(s_config.mqttTopic) - 1);
        s_config.mqttTopic[sizeof(s_config.mqttTopic) - 1] = '\0';
    }
}

bool ConfigManager::setMasterKeyHex(const char* hexStr) {
    if (!hexStr || strlen(hexStr) != 32) return false;
    uint8_t temp[16];
    if (hexToBytes(hexStr, temp, 16)) {
        memcpy(s_config.masterKey, temp, 16);
        bytesToHex(s_config.masterKey, 16, s_config.masterKeyHex);
        return true;
    }
    return false;
}

void ConfigManager::setMasterKeyBytes(const uint8_t key[16]) {
    if (!key) return;
    memcpy(s_config.masterKey, key, 16);
    bytesToHex(s_config.masterKey, 16, s_config.masterKeyHex);
}

bool ConfigManager::hasWifi() {
    return s_config.wifiSsid[0] != '\0';
}
