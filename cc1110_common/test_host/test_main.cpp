/**
 * @file test_main.cpp
 * @brief Runner of the counter log host tests. Usage: ctr_log_tests [name-substring]
 */

#include "test.h"

#include <string.h>
#include <vector>

namespace {
struct Entry {
    const char* name;
    TestFn fn;
    const char* xfail;
};
std::vector<Entry>& registry() {
    static std::vector<Entry> r;
    return r;
}
int s_failures = 0;   // failed checks in the current test
}

TestReg::TestReg(const char* name, TestFn fn, const char* xfail) {
    registry().push_back({name, fn, xfail});
}

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

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failed = 0, xfailed = 0;
    for (const Entry& e : registry()) {
        if (filter && !strstr(e.name, filter))
            continue;
        s_failures = 0;
        printf("%s%s\n", e.xfail ? "[ XFAIL? ] " : "[ RUN    ] ", e.name);
        fflush(stdout);
        e.fn();
        run++;
        if (e.xfail) {
            if (s_failures) {
                xfailed++;
                printf("[ XFAIL  ] %s (expected: %s)\n", e.name, e.xfail);
            } else {
                failed++;
                printf("[ XPASS  ] %s passes now: remove its XFAIL mark\n", e.name);
            }
        } else if (s_failures) {
            failed++;
            printf("[ FAILED ] %s\n", e.name);
        } else {
            printf("[     OK ] %s\n", e.name);
        }
    }
    printf("\n%d tests, %d failed, %d expected failures (known firmware problems)\n", run, failed,
           xfailed);
    if (run == 0) {
        printf("no test matches \"%s\"\n", filter ? filter : "");
        return 1;
    }
    return failed ? 1 : 0;
}
