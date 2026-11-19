/**
 * @file test_portal_util.cpp
 * @brief Host tests of portal_util.h: the /api/save JSON readers and hostOnly().
 */

#include "test.h"
#include "portal_util.h"

static String S(const char* s) { return String(s); }

/* ---- jsonGetString --------------------------------------------------------------------------- */
TEST(json_string_plain_and_absent) {
    String j = S("{\"wifi_ssid\":\"home\",\"mqtt_host\" :  \"192.168.1.5\",\"empty\":\"\"}");
    CHECK_STR(jsonGetString(j, "wifi_ssid").c_str(), "home");
    CHECK_STR(jsonGetString(j, "mqtt_host").c_str(), "192.168.1.5");   // spaces around ':'
    CHECK_STR(jsonGetString(j, "empty").c_str(), "");
    CHECK_STR(jsonGetString(j, "wifi_pass").c_str(), "");              // absent
    CHECK_STR(jsonGetString(S(""), "wifi_ssid").c_str(), "");
}

TEST(json_string_key_must_match_whole_name) {
    String j = S("{\"wifi_ssid2\":\"x\",\"my_wifi_ssid\":\"y\",\"wifi_ssid\":\"z\"}");
    CHECK_STR(jsonGetString(j, "wifi_ssid").c_str(), "z");
}

TEST(json_string_not_a_string) {
    String j = S("{\"a\":1883,\"b\":null,\"c\":true,\"d\":");
    CHECK_STR(jsonGetString(j, "a").c_str(), "");
    CHECK_STR(jsonGetString(j, "b").c_str(), "");
    CHECK_STR(jsonGetString(j, "c").c_str(), "");
    CHECK_STR(jsonGetString(j, "d").c_str(), "");                      // nothing after ':'
    CHECK_STR(jsonGetString(S("{\"e\""), "e").c_str(), "");            // no ':'
}

TEST(json_string_escapes) {
    String j = S("{\"k\":\"a\\\"b\\\\c\\/d\\ne\\tf\\rg\\bh\\fi\"}");
    CHECK_STR(jsonGetString(j, "k").c_str(), "a\"b\\c/d\ne\tf\rg\bh\fi");
    // a quote inside the value does not end it
    CHECK_STR(jsonGetString(S("{\"p\":\"pa\\\"ss\",\"q\":\"x\"}"), "p").c_str(), "pa\"ss");
}

TEST(json_string_unicode_escapes) {
    CHECK_STR(jsonGetString(S("{\"k\":\"\\u0041\\u0062c\"}"), "k").c_str(), "Abc");
    CHECK_STR(jsonGetString(S("{\"k\":\"\\u0022\"}"), "k").c_str(), "\"");
    CHECK_STR(jsonGetString(S("{\"k\":\"a\\u00e9b\"}"), "k").c_str(), "ab");     // non-ASCII dropped
    CHECK_STR(jsonGetString(S("{\"k\":\"a\\u20ACb\"}"), "k").c_str(), "ab");
    CHECK_STR(jsonGetString(S("{\"k\":\"a\\u0000b\"}"), "k").c_str(), "ab");     // NUL dropped
    CHECK_STR(jsonGetString(S("{\"k\":\"a\\u007Fb\"}"), "k").c_str(), "a\x7F" "b");
}

TEST(json_string_unterminated) {
    // no closing quote: what was read so far
    CHECK_STR(jsonGetString(S("{\"k\":\"abc"), "k").c_str(), "abc");
    CHECK_STR(jsonGetString(S("{\"k\":\"abc\\"), "k").c_str(), "abc");          // ends in '\'
    CHECK_STR(jsonGetString(S("{\"k\":\"ab\\u00"), "k").c_str(), "");           // cut \u: nothing
    CHECK_STR(jsonGetString(S("{\"k\":\"ab\\u004"), "k").c_str(), "");
    CHECK_STR(jsonGetString(S("{\"k\":\"ab\\u0041"), "k").c_str(), "abA");
    CHECK_STR(jsonGetString(S("{\"k\":\""), "k").c_str(), "");
}

TEST(json_string_long_value_is_kept) {
    String v;
    for (int i = 0; i < 300; i++) v += (char)('a' + i % 26);
    String j = S("{\"k\":\"") + v + "\"}";
    CHECK_STR(jsonGetString(j, "k").c_str(), v.c_str());
}

/* ---- jsonGetInt ------------------------------------------------------------------------------ */
TEST(json_int_numbers) {
    CHECK_EQ(jsonGetInt(S("{\"mqtt_port\":1883}"), "mqtt_port", -1), 1883);
    CHECK_EQ(jsonGetInt(S("{\"mqtt_port\": 8883 }"), "mqtt_port", -1), 8883);
    CHECK_EQ(jsonGetInt(S("{\"mqtt_port\":\"1884\"}"), "mqtt_port", -1), 1884);   // quoted
    CHECK_EQ(jsonGetInt(S("{\"o\":-5}"), "o", 0), -5);
    CHECK_EQ(jsonGetInt(S("{\"o\":\"-12\"}"), "o", 0), -12);
    CHECK_EQ(jsonGetInt(S("{\"o\":0}"), "o", 7), 0);
}

TEST(json_int_garbage_gives_default) {
    CHECK_EQ(jsonGetInt(S("{}"), "p", 1883), 1883);
    CHECK_EQ(jsonGetInt(S("{\"p\":}"), "p", 1883), 1883);
    CHECK_EQ(jsonGetInt(S("{\"p\":"), "p", 1883), 1883);
    CHECK_EQ(jsonGetInt(S("{\"p\":\"abc\"}"), "p", 1883), 1883);
    CHECK_EQ(jsonGetInt(S("{\"p\":abc}"), "p", 1883), 1883);
    CHECK_EQ(jsonGetInt(S("{\"p\":-}"), "p", 1883), 1883);
    CHECK_EQ(jsonGetInt(S("{\"p\":\"-\"}"), "p", 1883), 1883);
    CHECK_EQ(jsonGetInt(S("{\"p\":+5}"), "p", 1883), 1883);
    CHECK_EQ(jsonGetInt(S("{\"p\":null}"), "p", 1883), 1883);
    CHECK_EQ(jsonGetInt(S("{\"p\":\"\"}"), "p", 1883), 1883);
    CHECK_EQ(jsonGetInt(S("{\"p\":--5}"), "p", 1883), 1883);
}

TEST(json_int_leading_digits_only) {
    CHECK_EQ(jsonGetInt(S("{\"p\":12abc}"), "p", 0), 12);
    CHECK_EQ(jsonGetInt(S("{\"p\":1e3}"), "p", 0), 1);
    CHECK_EQ(jsonGetInt(S("{\"p\":\"80 \"}"), "p", 0), 80);
    CHECK_EQ(jsonGetInt(S("{\"p\":3.9}"), "p", 0), 3);
}

/* ---- hostOnly -------------------------------------------------------------------------------- */
TEST(host_only) {
    CHECK_STR(hostOnly(S("192.168.4.1")).c_str(), "192.168.4.1");
    CHECK_STR(hostOnly(S("192.168.4.1:80")).c_str(), "192.168.4.1");
    CHECK_STR(hostOnly(S("Verisure-Gateway.LOCAL:8080")).c_str(), "verisure-gateway.local");
    CHECK_STR(hostOnly(S("VERISURE-GATEWAY")).c_str(), "verisure-gateway");
    CHECK_STR(hostOnly(S("")).c_str(), "");
    CHECK_STR(hostOnly(S(":80")).c_str(), "");
    CHECK_STR(hostOnly(S("[::1]:80")).c_str(), "[::1]:80");    // IPv6 literal: left as it is
    CHECK_STR(hostOnly(S("[FE80::1]")).c_str(), "[FE80::1]");
}

TEST(host_only_origin_comparison) {
    // originAllowed(): hostOnly(Origin after "://") == hostOnly(Host)
    String origin = S("http://Verisure-Gateway.local:80");
    String host = S("verisure-gateway.local");
    int p = origin.indexOf("://");
    REQUIRE(p >= 0);
    CHECK(hostOnly(origin.substring(p + 3)) == hostOnly(host));
    String evil = S("http://verisure-gateway.local.evil.example");
    p = evil.indexOf("://");
    CHECK(!(hostOnly(evil.substring(p + 3)) == hostOnly(host)));
}
