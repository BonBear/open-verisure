/**
 * @file Preferences.h (host test stub)
 * @brief ESP32 Preferences (NVS) kept in a process-wide map, for the host unit tests. The map
 *        outlives a simulated gateway restart (the code's RAM state is reset, NVS is not), as the
 *        flash does. Like real NVS, keys are at most 15 characters and a value read with another
 *        type than it was written with is not found.
 */

#pragma once

#include <Arduino.h>
#include <map>
#include <string>
#include <vector>

class Preferences {
public:
    bool begin(const char* name, bool readOnly = false, const char* partitionLabel = nullptr);
    void end();

    bool clear();
    bool remove(const char* key);
    bool isKey(const char* key);

    size_t putChar(const char* key, int8_t value);
    size_t putUShort(const char* key, uint16_t value);
    size_t putULong(const char* key, uint32_t value);
    size_t putString(const char* key, const char* value);
    size_t putString(const char* key, const String& value) { return putString(key, value.c_str()); }
    size_t putBytes(const char* key, const void* value, size_t len);

    int8_t getChar(const char* key, int8_t defaultValue = 0);
    uint16_t getUShort(const char* key, uint16_t defaultValue = 0);
    uint32_t getULong(const char* key, uint32_t defaultValue = 0);
    String getString(const char* key, String defaultValue = String());
    size_t getBytesLength(const char* key);
    size_t getBytes(const char* key, void* buf, size_t maxLen);

private:
    bool put(const char* key, char type, const void* data, size_t len);
    const std::vector<uint8_t>* get(const char* key, char type);

    std::string ns_;
    bool started_ = false;
    bool readOnly_ = false;
};

/* ---- Test access to the simulated flash ------------------------------------------------------ */
struct HostNvsEntry {
    char type;                    // 'c' char, 'S' ushort, 'L' ulong, 's' string, 'b' bytes
    std::vector<uint8_t> data;
};
using HostNvsNamespace = std::map<std::string, HostNvsEntry>;

/** All namespaces; tests may read and change it directly. */
std::map<std::string, HostNvsNamespace>& hostNvs();
/** Erase the whole flash (a new device). */
void hostNvsWipe();
