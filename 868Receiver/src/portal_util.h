/**
 * @file portal_util.h
 * @brief Pure helpers of the web portal: the minimal JSON field readers for /api/save and the
 *        Host/Origin normalisation of the access check.
 *
 * They only use Arduino's String (no WebServer, no Wi-Fi), so the host unit tests in
 * 868Receiver/test_host include this header directly. Included by web_portal.cpp only.
 */

#pragma once

#include <Arduino.h>
#include <ctype.h>
#include <stdlib.h>

/** Host header or Origin authority without the port, lower case ("" stays ""). */
static inline String hostOnly(String h) {
    if (h.startsWith("[")) return h;          // IPv6 literal: not used here
    int c = h.indexOf(':');
    if (c >= 0) h = h.substring(0, c);
    h.toLowerCase();
    return h;
}

/* Minimal JSON field readers for /api/save (flat object of strings and numbers). */
static inline int jsonValueStart(const String& json, const char* key) {
    String k = String("\"") + key + "\"";
    int p = json.indexOf(k);
    if (p < 0) return -1;
    p = json.indexOf(':', p + k.length());
    if (p < 0) return -1;
    p++;
    while (p < (int)json.length() && isspace((unsigned char)json[p])) p++;
    return p;
}

/** A string value with its escapes (\" \\ \/ \b \f \n \r \t \uXXXX for ASCII) decoded; "" if absent. */
static inline String jsonGetString(const String& json, const char* key) {
    int p = jsonValueStart(json, key);
    String out;
    if (p < 0 || p >= (int)json.length() || json[p] != '"') return out;
    for (p++; p < (int)json.length() && json[p] != '"'; p++) {
        char c = json[p];
        if (c != '\\') { out += c; continue; }
        if (++p >= (int)json.length()) break;
        switch (json[p]) {
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (p + 4 >= (int)json.length()) return String();
                long v = strtol(json.substring(p + 1, p + 5).c_str(), nullptr, 16);
                p += 4;
                if (v > 0 && v < 0x80) out += (char)v;   // settings are ASCII
                break;
            }
            default: out += json[p];         // \" \\ \/
        }
    }
    return out;
}

/** A number, also when sent as a quoted string; defaultVal if absent or not a number. */
static inline long jsonGetInt(const String& json, const char* key, long defaultVal) {
    int p = jsonValueStart(json, key);
    if (p < 0 || p >= (int)json.length()) return defaultVal;
    if (json[p] == '"') p++;
    int e = p;
    if (e < (int)json.length() && json[e] == '-') e++;
    while (e < (int)json.length() && isdigit((unsigned char)json[e])) e++;
    if (e == p || (e == p + 1 && json[p] == '-')) return defaultVal;
    return json.substring(p, e).toInt();
}
