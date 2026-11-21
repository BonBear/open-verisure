/**
 * @file test_main.cpp
 * @brief Runner of the host unit tests. Usage: gateway_tests|fuzz_tests [name-substring]
 *        Exit code: 0 all passed, 1 a test failed, 77 no test matches the filter.
 */

#include "test.h"
#include <Arduino.h>
#include <Preferences.h>
#include "gateway_diag.h"
#include "picture_token.h"
#include "duty_cycle.h"
#include <string>
#include <vector>

void cryptoHostTestRestart();       // crypto.cpp, HOST_TEST only

namespace {
struct Entry {
    const char* name;
    TestFn fn;
};
std::vector<Entry>& registry() {
    static std::vector<Entry> r;
    return r;
}
int s_failures = 0;       // failed checks in the current test
}

TestReg::TestReg(const char* name, TestFn fn) { registry().push_back({name, fn}); }

bool testCurrentFailed() { return s_failures > 0; }

void testFail(const char* file, int line, const char* what) {
    s_failures++;
    printf("    %s:%d: CHECK failed: %s\n", file, line, what);
}

void testFailNum(const char* file, int line, const char* expr, long long got, long long want) {
    s_failures++;
    printf("    %s:%d: CHECK failed: %s (got %lld / 0x%llX, want %lld / 0x%llX)\n", file, line, expr,
           got, (unsigned long long)got, want, (unsigned long long)want);
}

static std::string printable(const char* s) {
    std::string r;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x20 && c < 0x7F && c != '\\') {
            r += (char)c;
        } else {
            char b[8];
            snprintf(b, sizeof(b), "\\x%02X", c);
            r += b;
        }
    }
    return r;
}

void testFailStr(const char* file, int line, const char* expr, const char* got, const char* want) {
    s_failures++;
    printf("    %s:%d: CHECK failed: %s is \"%s\", want \"%s\"\n", file, line, expr,
           printable(got).c_str(), printable(want).c_str());
}

void testResetWorld() {
    hostNvsWipe();
    hostSetMillis(100000);
    cryptoHostTestRestart();
    gatewayDiagClear();
    pictureTokenInit();
    dutyCycleInit();
}

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failed = 0;
    for (const Entry& e : registry()) {
        if (filter && !strstr(e.name, filter)) continue;
        testResetWorld();
        s_failures = 0;
        e.fn();
        run++;
        if (s_failures) {
            failed++;
            printf("FAIL %s\n", e.name);
        } else {
            printf("ok   %s\n", e.name);
        }
    }
    printf("\n%d tests, %d failed\n", run, failed);
    if (failed) return 1;
    return run ? 0 : 77;        // no test matches the filter (the Makefile runs two binaries)
}
