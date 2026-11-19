/**
 * @file test_config.cpp
 * @brief Host tests of config_manager.cpp: the web portal password (salted SHA-256 in NVS,
 *        constant-time check) and a few setters with security relevance.
 *
 * config_manager.cpp is built with HOST_TEST, so a local secrets.h is never included: the
 * compile-time defaults are the public fallbacks.
 */

#include "test.h"
#include "config_manager.h"
#include <Preferences.h>
#include <esp_mac.h>
#include <mbedtls/sha256.h>
#include <string>

static const uint8_t MAC_A[6] = {0x24, 0x6F, 0x28, 0x01, 0x02, 0x03};
static const uint8_t MAC_B[6] = {0x24, 0x6F, 0x28, 0x01, 0x02, 0x04};

/** A fresh gateway (testResetWorld() has erased the simulated flash) on chip A. */
static void startGateway() {
    hostSetFactoryMac(MAC_A);
    ConfigManager::begin();
}

static void refPortalHash(const uint8_t mac[6], const char* pw, uint8_t out[32]) {
    std::string in = "verisure-gateway-portal:";
    in.append((const char*)mac, 6);
    in.append(pw);
    mbedtls_sha256((const unsigned char*)in.data(), in.size(), out, 0);
}

static bool storedHash(uint8_t out[32]) {
    auto& ns = hostNvs()["gateway"];
    auto it = ns.find("portal_hash");
    if (it == ns.end() || it->second.type != 'b' || it->second.data.size() != 32) return false;
    memcpy(out, it->second.data.data(), 32);
    return true;
}

static bool flashContains(const char* text) {
    std::string needle(text);
    for (auto& ns : hostNvs())
        for (auto& kv : ns.second) {
            std::string v(kv.second.data.begin(), kv.second.data.end());
            if (v.find(needle) != std::string::npos) return true;
        }
    return false;
}

/* ---- Portal password ------------------------------------------------------------------------- */
TEST(portal_password_none_by_default) {
    startGateway();
    CHECK(!ConfigManager::hasPortalPassword());
    CHECK(!ConfigManager::checkPortalPassword(""));
    CHECK(!ConfigManager::checkPortalPassword("anything-at-all"));
    CHECK(!ConfigManager::checkPortalPassword(nullptr));
}

TEST(portal_password_length_limits) {
    startGateway();
    CHECK(!ConfigManager::setPortalPassword(nullptr));
    CHECK(!ConfigManager::setPortalPassword(""));
    CHECK(!ConfigManager::setPortalPassword("1234567"));              // 7
    CHECK(!ConfigManager::hasPortalPassword());
    CHECK(ConfigManager::setPortalPassword("12345678"));              // 8
    std::string p64(64, 'x'), p65(65, 'y');
    CHECK(ConfigManager::setPortalPassword(p64.c_str()));
    CHECK(!ConfigManager::setPortalPassword(p65.c_str()));
    CHECK(ConfigManager::checkPortalPassword(p64.c_str()));           // a rejected one changes nothing
    CHECK(!ConfigManager::checkPortalPassword(p65.c_str()));
}

TEST(portal_password_check) {
    startGateway();
    REQUIRE(ConfigManager::setPortalPassword("test-pass-1"));
    CHECK(ConfigManager::hasPortalPassword());
    CHECK(ConfigManager::checkPortalPassword("test-pass-1"));
    CHECK(!ConfigManager::checkPortalPassword("test-pass-2"));
    CHECK(!ConfigManager::checkPortalPassword("test-pass-"));         // prefix
    CHECK(!ConfigManager::checkPortalPassword("test-pass-11"));       // longer
    CHECK(!ConfigManager::checkPortalPassword("TEST-PASS-1"));
    CHECK(!ConfigManager::checkPortalPassword(""));
    CHECK(!ConfigManager::checkPortalPassword(nullptr));
}

TEST(portal_password_stored_as_salted_sha256_only) {
    startGateway();
    REQUIRE(ConfigManager::setPortalPassword("test-pass-1"));
    uint8_t got[32], want[32];
    REQUIRE(storedHash(got));
    refPortalHash(MAC_A, "test-pass-1", want);
    CHECK(memcmp(got, want, 32) == 0);
    CHECK(!flashContains("test-pass-1"));                             // no plaintext in NVS
}

TEST(portal_password_salt_is_the_chip_mac) {
    startGateway();
    REQUIRE(ConfigManager::setPortalPassword("test-pass-1"));
    uint8_t a[32], b[32];
    REQUIRE(storedHash(a));
    hostSetFactoryMac(MAC_B);                     // the same NVS on another chip
    CHECK(!ConfigManager::checkPortalPassword("test-pass-1"));
    REQUIRE(ConfigManager::setPortalPassword("test-pass-1"));
    REQUIRE(storedHash(b));
    CHECK(memcmp(a, b, 32) != 0);                 // same password, other hash
    CHECK(ConfigManager::checkPortalPassword("test-pass-1"));
}

TEST(portal_password_survives_restart_and_clear) {
    startGateway();
    REQUIRE(ConfigManager::setPortalPassword("test-pass-1"));
    ConfigManager::begin();                       // gateway restart: hash read back from NVS
    CHECK(ConfigManager::hasPortalPassword());
    CHECK(ConfigManager::checkPortalPassword("test-pass-1"));
    ConfigManager::clearPortalPassword();
    CHECK(!ConfigManager::hasPortalPassword());
    CHECK(!ConfigManager::checkPortalPassword("test-pass-1"));
    uint8_t h[32];
    CHECK(!storedHash(h));
    ConfigManager::begin();
    CHECK(!ConfigManager::hasPortalPassword());
}

TEST(portal_password_cleared_by_factory_reset) {
    startGateway();
    REQUIRE(ConfigManager::setPortalPassword("test-pass-1"));
    ConfigManager::resetToDefaults();
    CHECK(!ConfigManager::hasPortalPassword());
    CHECK(!ConfigManager::checkPortalPassword("test-pass-1"));
}

/* ---- MQTT settings --------------------------------------------------------------------------- */
TEST(mqtt_password_not_kept_for_another_broker) {
    startGateway();
    REQUIRE(ConfigManager::setMqtt("broker-a.lan", 1883, "user", "test-mqtt-pw", "verisure"));
    CHECK_STR(ConfigManager::get().mqttPass, "test-mqtt-pw");
    REQUIRE(ConfigManager::setMqtt("broker-a.lan", 1883, "user", nullptr, nullptr));   // same host
    CHECK_STR(ConfigManager::get().mqttPass, "test-mqtt-pw");
    REQUIRE(ConfigManager::setMqtt("broker-b.lan", 1883, "user", nullptr, nullptr));   // new host
    CHECK_STR(ConfigManager::get().mqttPass, "");
}

TEST(mqtt_topic_validation_changes_nothing_on_reject) {
    startGateway();
    REQUIRE(ConfigManager::setMqtt("broker-a.lan", 1883, "user", "test-mqtt-pw", "home/verisure"));
    const char* bad[] = {"a/#", "+", "a//b", "/a", "a/", "a\"b", "a b",
                         "0123456789012345678901234567890123"};   // 34 characters: too long
    for (const char* t : bad) {
        CHECK(!ConfigManager::setMqtt("broker-b.lan", 1884, "other", nullptr, t));
        CHECK_STR(ConfigManager::get().mqttTopic, "home/verisure");
        CHECK_STR(ConfigManager::get().mqttHost, "broker-a.lan");
        CHECK_STR(ConfigManager::get().mqttPass, "test-mqtt-pw");
        CHECK_EQ(ConfigManager::get().mqttPort, 1883);
    }
    CHECK(ConfigManager::setMqtt(nullptr, 0, nullptr, nullptr, "a-b_c/D9"));
    CHECK_STR(ConfigManager::get().mqttTopic, "a-b_c/D9");
}

/* ---- Master key ------------------------------------------------------------------------------ */
TEST(master_key_fallback_is_flagged_as_example) {
    startGateway();
    CHECK(ConfigManager::masterKeyIsExample());
    CHECK(ConfigManager::setMasterKeyHex("000102030405060708090a0b0c0d0e0f"));   // CI test key
    CHECK(!ConfigManager::masterKeyIsExample());
    CHECK_STR(ConfigManager::get().masterKeyHex, "000102030405060708090A0B0C0D0E0F");
}

TEST(master_key_hex_rejects_bad_input) {
    startGateway();
    CHECK(!ConfigManager::setMasterKeyHex(nullptr));
    CHECK(!ConfigManager::setMasterKeyHex(""));
    CHECK(!ConfigManager::setMasterKeyHex("000102030405060708090a0b0c0d0e0"));    // 31 digits
    CHECK(!ConfigManager::setMasterKeyHex("000102030405060708090a0b0c0d0e0f0"));  // 33 digits
    CHECK(!ConfigManager::setMasterKeyHex("000102030405060708090a0b0c0d0e0g"));   // not hex
    CHECK(ConfigManager::masterKeyIsExample());                                   // unchanged
}

/* ---- Setup access point password ------------------------------------------------------------ */
TEST(ap_password_unique_per_chip_and_typeable) {
    char a[64], a2[64], b[64];
    hostSetFactoryMac(MAC_A);
    ConfigManager::apPassword(a);
    ConfigManager::apPassword(a2);
    hostSetFactoryMac(MAC_B);
    ConfigManager::apPassword(b);
    hostSetFactoryMac(MAC_A);
    CHECK_EQ(strlen(a), 10);                       // WPA2 needs 8..63
    CHECK_STR(a, a2);                              // stable for one chip
    CHECK(strcmp(a, b) != 0);                      // differs between chips
    for (const char* p = a; *p; p++)               // no 0/O/1/l/i: typed from the serial log
        CHECK(strchr("abcdefghjkmnpqrstuvwxyz23456789", *p) != nullptr);
}
