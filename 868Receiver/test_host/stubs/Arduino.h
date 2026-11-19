/**
 * @file Arduino.h (host test stub)
 * @brief The small part of the Arduino core that the gateway sources under test use, for the
 *        host unit tests (868Receiver/test_host). Not used by the firmware build.
 */

#pragma once

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#define PROGMEM

/* ---- Time: millis() only moves when a test moves it ------------------------------------------ */
unsigned long millis();
void hostSetMillis(unsigned long ms);
void hostAdvanceMillis(unsigned long ms);

/* ---- String: the subset of Arduino's String the code under test uses ------------------------- */
class String {
public:
    String() {}
    String(const char* c) : s_(c ? c : "") {}
    String(const String&) = default;
    String& operator=(const String&) = default;
    explicit String(char c) : s_(1, c) {}
    explicit String(int v) : s_(std::to_string(v)) {}
    explicit String(long v) : s_(std::to_string(v)) {}
    explicit String(unsigned int v) : s_(std::to_string(v)) {}
    explicit String(unsigned long v) : s_(std::to_string(v)) {}

    unsigned int length() const { return (unsigned int)s_.size(); }
    const char* c_str() const { return s_.c_str(); }
    bool reserve(unsigned int n) { s_.reserve(n); return true; }

    // Arduino returns 0 for an index past the end instead of failing
    char operator[](unsigned int i) const { return i < s_.size() ? s_[i] : 0; }
    char& operator[](unsigned int i) { static char dummy; dummy = 0; return i < s_.size() ? s_[i] : dummy; }

    String& operator+=(const String& o) { s_ += o.s_; return *this; }
    String& operator+=(const char* c) { if (c) s_ += c; return *this; }
    String& operator+=(char c) { s_ += c; return *this; }

    int indexOf(char ch, unsigned int from = 0) const {
        size_t p = s_.find(ch, from);
        return p == std::string::npos ? -1 : (int)p;
    }
    int indexOf(const String& str, unsigned int from = 0) const {
        if (from > s_.size()) return -1;
        size_t p = s_.find(str.s_, from);
        return p == std::string::npos ? -1 : (int)p;
    }

    // Arduino semantics: indexes are swapped if reversed and clipped to the length
    String substring(unsigned int from) const { return substring(from, length()); }
    String substring(unsigned int from, unsigned int to) const {
        if (from > to) { unsigned int t = from; from = to; to = t; }
        if (from >= s_.size()) return String();
        if (to > s_.size()) to = (unsigned int)s_.size();
        return String(s_.substr(from, to - from).c_str());
    }

    long toInt() const { return atol(s_.c_str()); }
    bool startsWith(const String& p) const { return s_.compare(0, p.s_.size(), p.s_) == 0; }
    void toLowerCase() { for (auto& c : s_) c = (char)tolower((unsigned char)c); }
    void trim() {
        size_t b = 0, e = s_.size();
        while (b < e && isspace((unsigned char)s_[b])) b++;
        while (e > b && isspace((unsigned char)s_[e - 1])) e--;
        s_ = s_.substr(b, e - b);
    }

    bool operator==(const String& o) const { return s_ == o.s_; }
    bool operator==(const char* c) const { return s_ == (c ? c : ""); }
    bool operator!=(const String& o) const { return !(*this == o); }
    bool operator!=(const char* c) const { return !(*this == c); }

private:
    std::string s_;
};

inline String operator+(const String& a, const String& b) { String r(a); r += b; return r; }
inline String operator+(const String& a, const char* b) { String r(a); r += b; return r; }
inline String operator+(const char* a, const String& b) { String r(a); r += b; return r; }
inline String operator+(const String& a, char b) { String r(a); r += b; return r; }

/* ---- Serial: output is dropped unless HOST_TEST_VERBOSE is set in the environment ------------ */
class HostSerial {
public:
    size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    size_t println(const char* s = "");
    size_t println(const String& s) { return println(s.c_str()); }
    size_t print(const char* s);
    size_t print(const String& s) { return print(s.c_str()); }
};
extern HostSerial Serial;
