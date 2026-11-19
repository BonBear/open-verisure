/**
 * @file host_stubs.cpp
 * @brief Implementation of the host test stubs: millis(), Serial, Preferences, factory MAC.
 */

#include <Arduino.h>
#include <Preferences.h>
#include <esp_mac.h>

/* ---- millis() -------------------------------------------------------------------------------- */
static unsigned long s_millis = 0;
unsigned long millis() { return s_millis; }
void hostSetMillis(unsigned long ms) { s_millis = ms; }
void hostAdvanceMillis(unsigned long ms) { s_millis += ms; }

/* ---- Serial ---------------------------------------------------------------------------------- */
HostSerial Serial;

static bool verbose() {
    static int v = -1;
    if (v < 0) v = getenv("HOST_TEST_VERBOSE") != nullptr;
    return v != 0;
}

size_t HostSerial::printf(const char* fmt, ...) {
    if (!verbose()) return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n > 0 ? (size_t)n : 0;
}

size_t HostSerial::println(const char* s) {
    if (!verbose()) return 0;
    return (size_t)::printf("%s\n", s);
}

size_t HostSerial::print(const char* s) {
    if (!verbose()) return 0;
    return (size_t)::printf("%s", s);
}

/* ---- Preferences ----------------------------------------------------------------------------- */
std::map<std::string, HostNvsNamespace>& hostNvs() {
    static std::map<std::string, HostNvsNamespace> s_flash;
    return s_flash;
}

void hostNvsWipe() { hostNvs().clear(); }

static bool validKey(const char* key) {
    return key && key[0] && strlen(key) <= 15;     // NVS_KEY_NAME_MAX_SIZE - 1
}

bool Preferences::begin(const char* name, bool readOnly, const char*) {
    if (started_) return false;                     // as the ESP32 core: a second begin() fails
    if (!name || !name[0] || strlen(name) > 15) return false;
    ns_ = name;
    readOnly_ = readOnly;
    started_ = true;
    return true;
}

void Preferences::end() { started_ = false; }

bool Preferences::clear() {
    if (!started_ || readOnly_) return false;
    hostNvs()[ns_].clear();
    return true;
}

bool Preferences::remove(const char* key) {
    if (!started_ || readOnly_ || !validKey(key)) return false;
    return hostNvs()[ns_].erase(key) > 0;
}

bool Preferences::isKey(const char* key) {
    if (!started_ || !validKey(key)) return false;
    return hostNvs()[ns_].count(key) > 0;
}

bool Preferences::put(const char* key, char type, const void* data, size_t len) {
    if (!started_ || readOnly_ || !validKey(key)) return false;
    HostNvsEntry e;
    e.type = type;
    e.data.assign((const uint8_t*)data, (const uint8_t*)data + len);
    hostNvs()[ns_][key] = e;
    return true;
}

const std::vector<uint8_t>* Preferences::get(const char* key, char type) {
    if (!started_ || !validKey(key)) return nullptr;
    auto& n = hostNvs()[ns_];
    auto it = n.find(key);
    if (it == n.end() || it->second.type != type) return nullptr;
    return &it->second.data;
}

size_t Preferences::putChar(const char* key, int8_t v) { return put(key, 'c', &v, 1) ? 1 : 0; }
size_t Preferences::putBool(const char* key, bool v) { uint8_t b = v ? 1 : 0; return put(key, 'b', &b, 1) ? 1 : 0; }
bool Preferences::getBool(const char* key, bool d) {
    const std::vector<uint8_t>* e = get(key, 'b');
    return e ? (*e)[0] != 0 : d;
}
size_t Preferences::putUShort(const char* key, uint16_t v) { return put(key, 'S', &v, 2) ? 2 : 0; }
size_t Preferences::putULong(const char* key, uint32_t v) { return put(key, 'L', &v, 4) ? 4 : 0; }

size_t Preferences::putString(const char* key, const char* v) {
    if (!v) return 0;
    size_t n = strlen(v);
    return put(key, 's', v, n) ? n : 0;
}

size_t Preferences::putBytes(const char* key, const void* v, size_t len) {
    if (!v || !len) return 0;
    return put(key, 'b', v, len) ? len : 0;
}

int8_t Preferences::getChar(const char* key, int8_t d) {
    auto* v = get(key, 'c');
    return v ? (int8_t)(*v)[0] : d;
}

uint16_t Preferences::getUShort(const char* key, uint16_t d) {
    auto* v = get(key, 'S');
    if (!v) return d;
    uint16_t r;
    memcpy(&r, v->data(), 2);
    return r;
}

uint32_t Preferences::getULong(const char* key, uint32_t d) {
    auto* v = get(key, 'L');
    if (!v) return d;
    uint32_t r;
    memcpy(&r, v->data(), 4);
    return r;
}

String Preferences::getString(const char* key, String d) {
    auto* v = get(key, 's');
    if (!v) return d;
    return String(std::string(v->begin(), v->end()).c_str());
}

size_t Preferences::getBytesLength(const char* key) {
    auto* v = get(key, 'b');
    return v ? v->size() : 0;
}

size_t Preferences::getBytes(const char* key, void* buf, size_t maxLen) {
    auto* v = get(key, 'b');
    if (!v || !buf || v->size() > maxLen) return 0;  // as the ESP32 core: too small a buffer reads nothing
    memcpy(buf, v->data(), v->size());
    return v->size();
}

/* ---- Factory MAC ----------------------------------------------------------------------------- */
static uint8_t s_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

esp_err_t esp_efuse_mac_get_default(uint8_t* mac) {
    memcpy(mac, s_mac, 6);
    return ESP_OK;
}

void hostSetFactoryMac(const uint8_t mac[6]) { memcpy(s_mac, mac, 6); }
