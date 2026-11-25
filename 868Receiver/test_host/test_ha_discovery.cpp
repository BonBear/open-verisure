/**
 * @file test_ha_discovery.cpp
 * @brief Host tests of ha_discovery.cpp and gateway_diag.cpp: the gateway discovery payloads and
 *        the gateway state are valid JSON, the diagnostics entities are complete and the state
 *        carries every value their templates read.
 */

#include "test.h"
#include "ha_discovery.h"
#include "gateway_diag.h"
#include "image_receiver.h"
#include "mqtt_manager.h"
#include <stdint.h>
#include <map>
#include <string>

/* ---- A small strict JSON parser: validates and collects the top-level members ---------------- */
namespace {
struct Json {
    const char* p;
    bool ok = true;
    std::map<std::string, std::string> top;    // strings unquoted, anything else as raw text

    explicit Json(const char* s) : p(s) {
        ws();
        value(0, nullptr);
        ws();
        if (*p) ok = false;                     // trailing garbage
    }
    void ws() { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; }
    bool lit(const char* w) {
        size_t n = strlen(w);
        if (strncmp(p, w, n) != 0) return false;
        p += n;
        return true;
    }
    bool str(std::string* out) {
        if (*p != '"') return false;
        p++;
        while (*p && *p != '"') {
            unsigned char c = (unsigned char)*p;
            if (c < 0x20) return false;
            if (c == '\\') {
                p++;
                if (!strchr("\"\\/bfnrtu", *p) || !*p) return false;
                if (*p == 'u') {
                    for (int i = 1; i <= 4; i++) if (!isxdigit((unsigned char)p[i])) return false;
                    p += 4;
                }
            }
            if (out) *out += *p;
            p++;
        }
        if (*p != '"') return false;
        p++;
        return true;
    }
    bool num() {
        const char* s = p;
        if (*p == '-') p++;
        if (!isdigit((unsigned char)*p)) return false;
        if (*p == '0' && isdigit((unsigned char)p[1])) return false;
        while (isdigit((unsigned char)*p)) p++;
        if (*p == '.') { p++; if (!isdigit((unsigned char)*p)) return false; while (isdigit((unsigned char)*p)) p++; }
        if (*p == 'e' || *p == 'E') {
            p++;
            if (*p == '+' || *p == '-') p++;
            if (!isdigit((unsigned char)*p)) return false;
            while (isdigit((unsigned char)*p)) p++;
        }
        return p > s;
    }
    void value(int depth, std::string* out) {
        if (!ok || depth > 16) { ok = false; return; }
        const char* start = p;
        if (*p == '{') {
            p++;
            ws();
            if (*p == '}') { p++; }
            else for (;;) {
                std::string key, val;
                ws();
                if (!str(&key)) { ok = false; return; }
                ws();
                if (*p != ':') { ok = false; return; }
                p++;
                ws();
                value(depth + 1, depth == 0 ? &val : nullptr);
                if (!ok) return;
                if (depth == 0) {
                    if (top.count(key)) { ok = false; return; }   // duplicate member
                    top[key] = val;
                }
                ws();
                if (*p == ',') { p++; continue; }
                if (*p == '}') { p++; break; }
                ok = false;
                return;
            }
        } else if (*p == '[') {
            p++;
            ws();
            if (*p == ']') { p++; }
            else for (;;) {
                ws();
                value(depth + 1, nullptr);
                if (!ok) return;
                ws();
                if (*p == ',') { p++; continue; }
                if (*p == ']') { p++; break; }
                ok = false;
                return;
            }
        } else if (*p == '"') {
            std::string s;
            if (!str(&s)) { ok = false; return; }
            if (out) *out = s;
            return;
        } else if (!(lit("true") || lit("false") || lit("null") || num())) {
            ok = false;
            return;
        }
        if (out) out->assign(start, p - start);
    }
    bool has(const char* k) const { return top.count(k) != 0; }
    std::string get(const char* k) const { auto it = top.find(k); return it == top.end() ? "" : it->second; }
};

GatewayDiagSample sample() {
    GatewayDiagSample s;
    s.uptimeS = 4000000000UL;      // above 2^31: printed unsigned
    s.resetReason = "brownout";
    s.freeHeap = 123456;
    s.minFreeHeap = 98765;
    return s;
}

const char* const DIAG_KEYS[] = {
    "uptime", "reset_reason", "free_heap", "min_free_heap", "rx_crc_ok", "auth_ok", "auth_fail",
    "replay_drop", "pic_stale", "session_full", "last_rejected_id",
};
}  // namespace

/* MQTT_MAX_PACKET_SIZE / setBufferSize(1024): topic, payload and ~10 header bytes must fit */
#define MQTT_BUFFER 1024

TEST(ha_gateway_discovery_payloads_are_valid_json) {
    int n = haDiscoveryGetGatewayEntityCount();
    CHECK_EQ(n, 3 + 11);
    for (int i = 0; i < n; i++) {
        const HaEntityDef* def = haDiscoveryGetGatewayEntityDef(i);
        REQUIRE(def);
        String topic = haDiscoveryBuildGatewayTopic(def);
        String payload = haDiscoveryBuildGatewayPayload(def, "verisure");
        Json j(payload.c_str());
        if (!j.ok) printf("    invalid JSON: %s\n", payload.c_str());
        CHECK(j.ok);
        CHECK(topic.length() + payload.length() + 10 < MQTT_BUFFER);
        CHECK_STR(j.get("unique_id"), std::string("verisure_gateway_") + def->key);
        CHECK_STR(j.get("state_topic"), "verisure/gateway/state");
        CHECK_STR(j.get("entity_category"), "diagnostic");
        CHECK(j.has("device"));
        CHECK(!j.has("expire_after"));
    }
    CHECK(!haDiscoveryGetGatewayEntityDef(n));
    CHECK(!haDiscoveryGetGatewayEntityDef(-1));
}

TEST(ha_gateway_diag_entities_classes) {
    struct Want { const char* key; const char* stateClass; const char* unit; const char* deviceClass; };
    const Want want[] = {
        {"uptime", "total_increasing", "s", "duration"},
        {"reset_reason", "", "", ""},
        {"free_heap", "measurement", "B", "data_size"},
        {"min_free_heap", "measurement", "B", "data_size"},
        {"rx_crc_ok", "total_increasing", "pkts", ""},
        {"auth_ok", "total_increasing", "pkts", ""},
        {"auth_fail", "total_increasing", "pkts", ""},
        {"replay_drop", "total_increasing", "pkts", ""},
        {"pic_stale", "total_increasing", "chunks", ""},
        {"session_full", "total_increasing", "events", ""},
        {"last_rejected_id", "", "", ""},
    };
    for (const Want& w : want) {
        const HaEntityDef* def = nullptr;
        for (int i = 0; i < haDiscoveryGetGatewayEntityCount(); i++) {
            if (strcmp(haDiscoveryGetGatewayEntityDef(i)->key, w.key) == 0) def = haDiscoveryGetGatewayEntityDef(i);
        }
        if (!def) printf("    missing entity %s\n", w.key);
        REQUIRE(def);
        Json j(haDiscoveryBuildGatewayPayload(def, "verisure").c_str());
        REQUIRE(j.ok);
        CHECK_STR(j.get("state_class"), w.stateClass);
        CHECK_STR(j.get("unit_of_measurement"), w.unit);
        CHECK_STR(j.get("device_class"), w.deviceClass);
        CHECK_STR(j.get("value_template"), std::string("{{ value_json.") + w.key + " }}");
    }
}

TEST(ha_gateway_state_has_every_template_value) {
    g_gatewayDiag.rxCrcOk = 4294967295UL;
    g_gatewayDiag.authOk = 7;
    g_gatewayDiag.authFail = 2;
    gatewayDiagNoteRejected("2K79A65A");
    String state = haStateBuildGatewayPayload(-98, 0x0D, 42, 3, sample());
    Json j(state.c_str());
    if (!j.ok) printf("    invalid JSON: %s\n", state.c_str());
    REQUIRE(j.ok);
    CHECK(state.length() < MQTT_BUFFER - 64);
    // every gateway entity reads a member of this state
    for (int i = 0; i < haDiscoveryGetGatewayEntityCount(); i++) {
        const char* key = haDiscoveryGetGatewayEntityDef(i)->key;
        if (!j.has(key)) printf("    state lacks %s\n", key);
        CHECK(j.has(key));
    }
    CHECK_STR(j.get("uptime"), "4000000000");
    CHECK_STR(j.get("reset_reason"), "brownout");
    CHECK_STR(j.get("free_heap"), "123456");
    CHECK_STR(j.get("min_free_heap"), "98765");
    CHECK_STR(j.get("rx_crc_ok"), "4294967295");
    CHECK_STR(j.get("auth_ok"), "7");
    CHECK_STR(j.get("auth_fail"), "2");
    CHECK_STR(j.get("replay_drop"), "0");
    CHECK_STR(j.get("last_rejected_id"), "2K79A65A");
    CHECK_STR(j.get("packets"), "42");
}

TEST(diag_json_without_rejected_id_is_null) {
    String s = "{";
    gatewayDiagAppendJson(s, sample());
    s += "}";
    Json j(s.c_str());
    REQUIRE(j.ok);
    CHECK_STR(j.get("last_rejected_id"), "null");
    for (const char* k : DIAG_KEYS) CHECK(j.has(k));
    CHECK_EQ(j.top.size(), sizeof(DIAG_KEYS) / sizeof(DIAG_KEYS[0]));
}

TEST(diag_rejected_id_only_alphanumeric) {
    gatewayDiagNoteRejected("ABCD1234");
    CHECK_STR(g_gatewayDiag.lastRejectedId, "ABCD1234");
    gatewayDiagNoteRejected("AB\"D1234");     // would break the JSON: not taken
    gatewayDiagNoteRejected("ABC");           // short
    gatewayDiagNoteRejected(nullptr);
    CHECK_STR(g_gatewayDiag.lastRejectedId, "ABCD1234");
    gatewayDiagNoteRejected("ZZZZ9999XYZ");   // only the first 8 characters
    CHECK_STR(g_gatewayDiag.lastRejectedId, "ZZZZ9999");
}

TEST(json_checker_rejects_bad_json) {
    CHECK(!Json("{\"a\":1,}").ok);
    CHECK(!Json("{\"a\":1 \"b\":2}").ok);
    CHECK(!Json("{\"a\":\"x\"\"}").ok);
    CHECK(!Json("{\"a\":01}").ok);
    CHECK(!Json("{\"a\":1,\"a\":2}").ok);
    CHECK(Json("{\"a\":[1,{\"b\":null}],\"c\":\"\\\"\"}").ok);
}

TEST(mqtt_camera_image_length_guard) {
    CHECK(!mqttCameraImageLengthOk(23, 0));
    CHECK(!mqttCameraImageLengthOk(0, 0));

    // "verisure/2NCEKS3T/image" (23 B): 2 + 23 + 65510 = 65535 (max uint16_t)
    CHECK(mqttCameraImageLengthOk(23, 65510));
    CHECK(!mqttCameraImageLengthOk(23, 65511));
    CHECK(!mqttCameraImageLengthOk(23, MAX_IMAGE_SIZE));

    // Max 32-char base topic (".../2NCEKS3T/image" = 47 B): 2 + 47 + 65486 = 65535
    CHECK(mqttCameraImageLengthOk(47, 65486));
    CHECK(!mqttCameraImageLengthOk(47, 65487));

    // Topic/payload extremes and wrap-around safety
    CHECK(!mqttCameraImageLengthOk(65534, 1));
    CHECK(!mqttCameraImageLengthOk(65533, 1));
    CHECK(mqttCameraImageLengthOk(65532, 1));
    CHECK(!mqttCameraImageLengthOk(23, SIZE_MAX));
    CHECK(!mqttCameraImageLengthOk(0, SIZE_MAX));
}

TEST(ha_siren_discovery_and_state_payload) {
    CHECK(haIsSirenSensor("2S5F554X"));
    CHECK(!haIsSirenSensor("2JHL2MVN"));
    CHECK(!haIsSirenSensor("2NCEKS3T"));
    CHECK(!haIsSirenSensor(nullptr));

    int count = haDiscoveryGetSirenEntityCount();
    CHECK_EQ(count, 5);
    CHECK(!haDiscoveryGetSirenEntityDef(-1));
    CHECK(!haDiscoveryGetSirenEntityDef(count));

    bool sawTempEntity = false;
    for (int i = 0; i < count; i++) {
        const HaEntityDef* def = haDiscoveryGetSirenEntityDef(i);
        REQUIRE(def);
        CHECK(strcmp(def->key, "contact") != 0);
        CHECK(strcmp(def->key, "vibration") != 0);
        String topic = haDiscoveryBuildTopic("2S5F554X", def);
        String payload = haDiscoveryBuildPayload("2S5F554X", def, "verisure");
        Json j(payload.c_str());
        REQUIRE(j.ok);
        CHECK(topic.length() + payload.length() + 10 < MQTT_BUFFER);
        CHECK(strstr(payload.c_str(), "Verisure Siren (GW-SI)") != nullptr);
        if (strcmp(def->key, "temperature") == 0) {
            sawTempEntity = true;
            CHECK_STR(j.get("device_class"), "temperature");
            CHECK_STR(j.get("state_class"), "measurement");
            CHECK_STR(j.get("unit_of_measurement"), "°C");
            CHECK_STR(j.get("value_template"),
                      "{{ value_json.temperature_c if value_json.temperature_c is defined else None }}");
        }
    }
    CHECK(sawTempEntity);

    String sirenTopic = haDiscoveryBuildSirenTopic("2S5F554X");
    CHECK_STR(sirenTopic.c_str(), "homeassistant/siren/verisure_2S5F554X_siren/config");

    String sirenPayload = haDiscoveryBuildSirenPayload("2S5F554X", "verisure");
    Json sj(sirenPayload.c_str());
    REQUIRE(sj.ok);
    CHECK(sirenTopic.length() + sirenPayload.length() + 10 < MQTT_BUFFER);
    CHECK_STR(sj.get("command_topic"), "verisure/2S5F554X/siren/set");
    CHECK_STR(sj.get("state_topic"), "verisure/2S5F554X/state");
    CHECK_STR(sj.get("value_template"), "{{ value_json.siren }}");
    CHECK_STR(sj.get("payload_on"), "ON");
    CHECK_STR(sj.get("payload_off"), "OFF");
    CHECK_STR(sj.get("support_duration"), "true");
    CHECK(sj.has("available_tones"));
    CHECK_STR(sj.get("available_tones"), "[\"alarm\",\"ding_dong\",\"chime\",\"double_beep\",\"beep\",\"tone\"]");
    CHECK(strstr(sirenPayload.c_str(), "Verisure Siren (GW-SI)") != nullptr);

    VerisureTelemetry t = {};
    strcpy(t.deviceId, "2S5F554X");
    t.batteryPct = 95;
    t.batteryMv = 3010;
    t.rssi = -52;
    t.lqi = 40;
    t.counter = 0x105;
    t.isOpen = true;
    t.hasTemp = true;
    t.tempC = 22.5f;
    t.valid = true;
    Json stOn(haStateBuildPayload(t).c_str());
    REQUIRE(stOn.ok);
    CHECK_STR(stOn.get("siren"), "ON");
    CHECK_STR(stOn.get("temperature_c"), "22.5");

    t.isOpen = false;
    t.hasTemp = false;
    Json stOff(haStateBuildPayload(t).c_str());
    REQUIRE(stOff.ok);
    CHECK_STR(stOff.get("siren"), "OFF");
    CHECK(!stOff.has("temperature_c"));

    strcpy(t.deviceId, "2JHL2MVN");
    Json stDoor(haStateBuildPayload(t).c_str());
    REQUIRE(stDoor.ok);
    CHECK(!stDoor.has("siren"));

    strcpy(t.deviceId, "2NCEKS3T");
    Json stCam(haStateBuildPayload(t).c_str());
    REQUIRE(stCam.ok);
    CHECK(!stCam.has("siren"));
}
