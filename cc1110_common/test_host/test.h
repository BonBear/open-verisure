/**
 * @file test.h
 * @brief A minimal test framework (as in 868Receiver/test_host): TEST() registers a test,
 *        CHECK*() records a failure and goes on, REQUIRE() stops the test. XFAIL_TEST() marks a
 *        test that shows a known firmware problem: its failure is expected and reported as
 *        XFAIL; if it passes, the run fails (XPASS), so the mark is removed once the firmware
 *        is fixed.
 */

#pragma once

#include <stdio.h>

typedef void (*TestFn)();

struct TestReg {
    TestReg(const char* name, TestFn fn, const char* xfail = nullptr);
};

void testFail(const char* file, int line, const char* what);
void testFailNum(const char* file, int line, const char* expr, long long got, long long want);
bool testCurrentFailed();

#define TEST(name)                                         \
    static void name();                                    \
    static TestReg name##_reg(#name, name);                \
    static void name()

#define XFAIL_TEST(name, why)                              \
    static void name();                                    \
    static TestReg name##_reg(#name, name, why);           \
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
