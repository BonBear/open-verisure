/**
 * @file test.h
 * @brief A minimal test framework for the host unit tests: TEST() registers a test, CHECK*()
 *        records a failure and goes on, REQUIRE() stops the test.
 */

#pragma once

#include <stdio.h>
#include <string.h>
#include <string>

typedef void (*TestFn)();

struct TestReg {
    TestReg(const char* name, TestFn fn);
};

void testFail(const char* file, int line, const char* what);
void testFailNum(const char* file, int line, const char* expr, long long got, long long want);
void testFailStr(const char* file, int line, const char* expr, const char* got, const char* want);
bool testCurrentFailed();

/** Reset everything between tests: flash, millis(), the crypto module's RAM state. */
void testResetWorld();

#define TEST(name)                                         \
    static void name();                                    \
    static TestReg name##_reg(#name, name);                \
    static void name()

#define CHECK(cond)                                                     \
    do { if (!(cond)) testFail(__FILE__, __LINE__, #cond); } while (0)

#define REQUIRE(cond)                                                   \
    do { if (!(cond)) { testFail(__FILE__, __LINE__, #cond); return; } } while (0)

#define CHECK_EQ(got, want)                                                          \
    do {                                                                             \
        long long g_ = (long long)(got), w_ = (long long)(want);                    \
        if (g_ != w_) testFailNum(__FILE__, __LINE__, #got " == " #want, g_, w_);   \
    } while (0)

#define CHECK_STR(got, want)                                                         \
    do {                                                                             \
        std::string g_ = (got);     /* a copy: (got) may point into a temporary */   \
        std::string w_ = (want);                                                     \
        if (g_ != w_) testFailStr(__FILE__, __LINE__, #got, g_.c_str(), w_.c_str()); \
    } while (0)
