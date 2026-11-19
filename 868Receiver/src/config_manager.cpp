/**
 * @file config_manager.cpp
 * @brief Persistent Configuration Storage Implementation using ESP32 Preferences
 */

#include "config_manager.h"
#include "crypto.h"
#include <Preferences.h>
#include <ctype.h>
#include <string.h>
#include <esp_mac.h>
#include <mbedtls/sha256.h>

// HOST_TEST: the host unit tests (868Receiver/test_host) never see a local secrets.h
#if !defined(HOST_TEST) && __has_include("secrets.h")
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

// CC1101 frequency correction (FSCTRL0 steps of 1.587 kHz). 0 for a module with a normal
// crystal; set it in secrets.h or with the CLI command "freqoffset <n>" when the received
// packets log a large "Freq. error" (see documentation/hardware_gateway_esp32s2.md).
#ifndef DEFAULT_FREQ_OFFSET
#define DEFAULT_FREQ_OFFSET 0
#endif

#if defined(MASTER_KEY_HEX)
#define ACTIVE_MASTER_KEY_HEX MASTER_KEY_HEX
#elif defined(MASTER_AES_KEY_HEX)
#define ACTIVE_MASTER_KEY_HEX MASTER_AES_KEY_HEX
#elif defined(DEFAULT_MASTER_KEY_HEX)
#define ACTIVE_MASTER_KEY_HEX DEFAULT_MASTER_KEY_HEX
#else
#define ACTIVE_MASTER_KEY_HEX "0102030405060708090A0B0C0D0E0F10"
#endif

// The public example key of secrets.h.example (and the fallback above): a gateway that runs
// with it accepts packets anyone can make, so it is reported at boot and on the web page.
#define EXAMPLE_MASTER_KEY_HEX "0102030405060708090A0B0C0D0E0F10"

// Placeholder SSID of secrets.h.example: a copy left unchanged must not stop the setup access point
#define EXAMPLE_WIFI_SSID "YOUR_WIFI_SSID"

#ifndef DEFAULT_PORTAL_PASS
#define DEFAULT_PORTAL_PASS ""
#endif

static Preferences s_prefs;
static GatewayConfig s_config;
static uint8_t s_portalHash[32];
static bool s_portalHashSet = false;

void ConfigManager::bytesToHex(const uint8_t* in, int len, char* out) {
    static const char hexChars[] = "0123456789ABCDEF";
    for (int i = 0; i < len; i++) {
        out[i * 2]     = hexChars[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hexChars[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

// The base topic goes unescaped into MQTT topics and HA discovery JSON, so only plain
// topic levels are allowed: [A-Za-z0-9_-] separated by single '/', no wildcards or quotes.
static bool isValidBaseTopic(const char* t) {
    size_t n = t ? strlen(t) : 0;
    if (n == 0 || t[0] == '/' || t[n - 1] == '/') return false;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (c == '/') {
            if (t[i + 1] == '/') return false;
        } else if (!isalnum((unsigned char)c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
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
    if (strcmp(s_config.wifiSsid, EXAMPLE_WIFI_SSID) == 0) {
        Serial.println("[Config] Wi-Fi SSID is the secrets.h.example placeholder: ignored (setup access point).");
        s_config.wifiSsid[0] = '\0';
        s_config.wifiPass[0] = '\0';
    }

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
    if (!isValidBaseTopic(s_config.mqttTopic)) {
        Serial.println("[Config] Stored MQTT topic is invalid, using the default.");
        strncpy(s_config.mqttTopic, DEFAULT_MQTT_TOPIC, sizeof(s_config.mqttTopic) - 1);
        s_config.mqttTopic[sizeof(s_config.mqttTopic) - 1] = '\0';
    }

    // 3. Radio frequency correction
    s_config.freqOffset = s_prefs.getChar("freq_offset", DEFAULT_FREQ_OFFSET);

    // 4. Master Key (16 bytes)
    // If the firmware was recompiled with a new MASTER_KEY_HEX in secrets.h, automatically adopt it!
    String lastCompiledKey = s_prefs.getString("compiled_key", "");
    String currentCompiledKey = String(ACTIVE_MASTER_KEY_HEX);

    if (lastCompiledKey != currentCompiledKey) {
        Serial.println("[Config] Firmware update: Detected new MASTER_KEY_HEX in secrets.h! Updating active key...");
        hexToBytes(ACTIVE_MASTER_KEY_HEX, s_config.masterKey, 16);
        bytesToHex(s_config.masterKey, 16, s_config.masterKeyHex);
        s_prefs.putBytes("master_key", s_config.masterKey, 16);
        s_prefs.putString("compiled_key", currentCompiledKey);
    } else {
        size_t keyLen = s_prefs.getBytes("master_key", s_config.masterKey, 16);
        if (keyLen != 16) {
            hexToBytes(ACTIVE_MASTER_KEY_HEX, s_config.masterKey, 16);
            s_prefs.putBytes("master_key", s_config.masterKey, 16);
            s_prefs.putString("compiled_key", currentCompiledKey);
        }
        bytesToHex(s_config.masterKey, 16, s_config.masterKeyHex);
    }

    Serial.println("[Config] Loaded configuration:");
    Serial.printf("         Wi-Fi SSID  : %s\n", s_config.wifiSsid[0] ? s_config.wifiSsid : "(not configured)");
    Serial.printf("         MQTT Broker : %s:%d (Topic: %s)\n",
                  s_config.mqttHost[0] ? s_config.mqttHost : "(not configured)",
                  s_config.mqttPort, s_config.mqttTopic);
    Serial.printf("         Freq. offset: %d (FSCTRL0, x 1.587 kHz)\n", s_config.freqOffset);
    char kcv[7];
    cryptoKeyCheckValue(s_config.masterKey, kcv);
    Serial.printf("         Master Key  : set (check value %s)\n", kcv);   // never print the key
    if (masterKeyIsExample())
        Serial.println("[Config] WARNING: the master key is the PUBLIC example key: set your own "
                       "(python derive_key.py --generate-master) in secrets.h or on the web page.");

    // 5. Web portal password (hash only)
    s_portalHashSet = s_prefs.getBytes("portal_hash", s_portalHash, sizeof(s_portalHash)) == sizeof(s_portalHash);
    if (!s_portalHashSet && DEFAULT_PORTAL_PASS[0])
        setPortalPassword(DEFAULT_PORTAL_PASS);
    Serial.printf("         Portal pass : %s\n", s_portalHashSet ? "set" : "NOT SET (web page locked outside the setup AP; serial: portalpass <password>)");
}

bool ConfigManager::masterKeyIsExample() {
    return strcasecmp(s_config.masterKeyHex, EXAMPLE_MASTER_KEY_HEX) == 0;
}

// SHA-256 over a fixed label, this chip's factory MAC (salt) and the password
static void portalHash(const char* pw, uint8_t out[32]) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    static const char label[] = "verisure-gateway-portal:";
    mbedtls_sha256_update(&ctx, (const uint8_t*)label, sizeof(label) - 1);
    mbedtls_sha256_update(&ctx, mac, sizeof(mac));
    mbedtls_sha256_update(&ctx, (const uint8_t*)pw, strlen(pw));
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

bool ConfigManager::hasPortalPassword() {
    return s_portalHashSet;
}

bool ConfigManager::setPortalPassword(const char* pw) {
    size_t n = pw ? strlen(pw) : 0;
    if (n < 8 || n > 64) return false;
    portalHash(pw, s_portalHash);
    s_prefs.putBytes("portal_hash", s_portalHash, sizeof(s_portalHash));
    s_portalHashSet = true;
    return true;
}

bool ConfigManager::checkPortalPassword(const char* pw) {
    if (!s_portalHashSet || !pw) return false;
    uint8_t h[32];
    portalHash(pw, h);
    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof(h); i++) diff |= h[i] ^ s_portalHash[i];
    memset(h, 0, sizeof(h));
    return diff == 0;
}

void ConfigManager::clearPortalPassword() {
    s_prefs.remove("portal_hash");
    memset(s_portalHash, 0, sizeof(s_portalHash));
    s_portalHashSet = false;
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
    s_prefs.putChar("freq_offset", s_config.freqOffset);
    Serial.println("[Config] Configuration saved to NVS!");
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
    s_config.freqOffset = DEFAULT_FREQ_OFFSET;
    hexToBytes(ACTIVE_MASTER_KEY_HEX, s_config.masterKey, 16);
    bytesToHex(s_config.masterKey, 16, s_config.masterKeyHex);
    s_prefs.putString("compiled_key", ACTIVE_MASTER_KEY_HEX);
    s_portalHashSet = false;               // cleared with the rest of NVS
    memset(s_portalHash, 0, sizeof(s_portalHash));
    if (DEFAULT_PORTAL_PASS[0]) setPortalPassword(DEFAULT_PORTAL_PASS);
    save();
    Serial.println("[Config] NVS reset to factory defaults.");
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

bool ConfigManager::setMqtt(const char* host, uint16_t port, const char* user, const char* pass, const char* topic) {
    // validate first: a rejected call changes nothing
    if (topic && topic[0] && (strlen(topic) >= sizeof(s_config.mqttTopic) || !isValidBaseTopic(topic))) {
        Serial.println("[Config] MQTT topic rejected (allowed: A-Z a-z 0-9 _ - and single '/'), nothing changed.");
        return false;
    }
    if (host) {
        // a kept password must never go to another broker: a new host without a new password
        // drops the stored one (otherwise anyone who could change the host gets the password)
        if (!pass && strcmp(host, s_config.mqttHost) != 0)
            s_config.mqttPass[0] = '\0';
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
    if (topic && topic[0]) strcpy(s_config.mqttTopic, topic);
    return true;
}

void ConfigManager::setFreqOffset(int8_t steps) {
    s_config.freqOffset = steps;
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
