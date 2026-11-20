/**
 * @file config_manager.h
 * @brief Persistent Configuration Manager (NVS / Preferences & Compile-Time Defaults)
 *
 * Manages Wi-Fi credentials, MQTT connection settings, and Gateway Master AES Key.
 *
 * Configuration Priority:
 * 1. ESP32 Non-Volatile Storage (NVS / Preferences partition).
 * 2. Local compile-time defaults defined in `secrets.h` (if present).
 * 3. Safe fallback defaults (empty strings, port 1883, default development key).
 */

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @struct GatewayConfig
 * @brief Gateway configuration parameters structure.
 */
struct GatewayConfig {
    char wifiSsid[33];      /**< Wi-Fi network SSID (up to 32 chars + null) */
    char wifiPass[65];      /**< Wi-Fi WPA2 password (up to 64 chars + null) */
    char mqttHost[65];      /**< MQTT broker hostname or IPv4 address */
    uint16_t mqttPort;      /**< MQTT broker TCP port (default 1883) */
    char mqttUser[33];      /**< MQTT username (optional) */
    char mqttPass[65];      /**< MQTT password (optional) */
    char mqttTopic[33];     /**< Base MQTT topic prefix (default "verisure") */
    uint8_t masterKey[16];  /**< 16-byte Master AES Key for device key derivation */
    char masterKeyHex[33];  /**< Hex-encoded representation of Master Key (32 hex chars + null) */
    int8_t freqOffset;      /**< CC1101 FSCTRL0 frequency correction, 1.587 kHz per step (0 = none) */
};

/**
 * @class ConfigManager
 * @brief Static service managing loading, updating, and saving gateway settings.
 */
class ConfigManager {
public:
    /**
     * @brief Open NVS storage and load configuration parameters.
     */
    static void begin();

    /**
     * @brief Get immutable reference to active gateway configuration.
     * @return Const reference to GatewayConfig.
     */
    static const GatewayConfig& get();

    /**
     * @brief Persist current configuration settings to ESP32 NVS.
     * @return True if save succeeded.
     */
    static bool save();

    /**
     * @brief Wipe NVS settings and restore compile-time / factory defaults.
     */
    static void resetToDefaults();

    /**
     * @brief Update Wi-Fi credentials in memory.
     * @param ssid Wi-Fi network name.
     * @param pass Wi-Fi password.
     */
    static void setWifi(const char* ssid, const char* pass);

    /**
     * @brief Update MQTT broker parameters in memory.
     * @param host Broker IP or hostname.
     * @param port Broker port.
     * @param user Username.
     * @param pass Password.
     * @param topic Base topic.
     */
    /** @return false if the topic was rejected (nothing is changed then). A null pass keeps the stored one. */
    static bool setMqtt(const char* host, uint16_t port, const char* user, const char* pass, const char* topic);

    /**
     * @brief Set the CC1101 frequency correction (FSCTRL0) in memory; save() stores it.
     * @param steps Signed offset in 1.587 kHz steps (-128..127).
     */
    static void setFreqOffset(int8_t steps);

    /**
     * @brief Update Master AES Key from a 32-character hexadecimal string.
     * @param hexStr Hex string (e.g. "0102030405060708090A0B0C0D0E0F10").
     * @return True if valid 32-hex string and set successfully.
     */
    static bool setMasterKeyHex(const char* hexStr);

    /**
     * @brief Update Master AES Key from raw 16-byte array.
     * @param key 16-byte buffer.
     */
    static void setMasterKeyBytes(const uint8_t key[16]);

    /**
     * @brief Check if Wi-Fi SSID is configured.
     * @return True if wifiSsid is non-empty.
     */
    static bool hasWifi();

    /**
     * @brief Convert raw byte array to uppercase hexadecimal string.
     * @param in Input byte array.
     * @param len Number of bytes.
     * @param out Output char buffer (must be at least len*2 + 1 bytes).
     */
    static void bytesToHex(const uint8_t* in, int len, char* out);

    /**
     * @brief Parse hexadecimal string into raw byte array.
     * @param in Input hex string.
     * @param out Output byte buffer.
     * @param len Number of bytes expected (hex string must have 2*len chars).
     * @return True on valid hex input.
     */
    static bool hexToBytes(const char* in, uint8_t* out, int len);

    /** @brief True if the active master key is the public example key of secrets.h.example. */
    static bool masterKeyIsExample();

    /**
     * Web portal password. Only a salted SHA-256 of it is stored in NVS ("portal_hash"); it is
     * set in the setup access point, with the serial command "portalpass", or by
     * DEFAULT_PORTAL_PASS in secrets.h (used while NVS has none).
     */
    static bool hasPortalPassword();

    /**
     * Password of the setup access point 'Verisure-Gateway': DEFAULT_AP_PASS from secrets.h
     * (8-63 characters) or else 10 characters derived from this chip's factory MAC, so every
     * gateway has its own. Shown on the serial console (boot, every minute in AP mode, and the
     * command "appass"), never on the page.
     * @param out at least 64 bytes
     */
    static void apPassword(char* out);
    /** @return false if the password is shorter than 8 or longer than 64 characters. */
    static bool setPortalPassword(const char* pw);
    /** @brief Constant-time check of a password against the stored hash. */
    static bool checkPortalPassword(const char* pw);
    static void clearPortalPassword();
};
