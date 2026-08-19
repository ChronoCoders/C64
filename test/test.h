//! Minimal test harness for the durable suite: assertion macros, pass/fail/skip
//! counters, and a summary that names any failure and sets the process exit code.
//! Each subsystem test is a standalone binary that ends with TEST_SUMMARY.
#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

static int test_pass;
static int test_fail;
static int test_skip;

// CHECK: assert a boolean. NAME states the behavior under test, not the function.
#define CHECK(cond, name)                                                      \
    do {                                                                       \
        if (cond) {                                                            \
            test_pass++;                                                       \
        } else {                                                               \
            test_fail++;                                                       \
            printf("  FAIL %s (%s:%d)\n", (name), __FILE__, __LINE__);         \
        }                                                                      \
    } while (0)

// CHECK_EQ: assert got == want, reporting both (decimal and hex) on failure.
#define CHECK_EQ(got, want, name)                                              \
    do {                                                                       \
        long long g_ = (long long)(got);                                       \
        long long w_ = (long long)(want);                                      \
        if (g_ == w_) {                                                        \
            test_pass++;                                                       \
        } else {                                                               \
            test_fail++;                                                       \
            printf("  FAIL %s: got %lld ($%llX) want %lld ($%llX) (%s:%d)\n",  \
                   (name), g_, (unsigned long long)g_, w_,                     \
                   (unsigned long long)w_, __FILE__, __LINE__);                \
        }                                                                      \
    } while (0)

// SKIP: record a test that could not run (e.g. a copyrighted ROM is absent).
// A skip is neither a pass nor a failure; it is reported so the gap is visible.
#define SKIP(name, reason)                                                     \
    do {                                                                       \
        test_skip++;                                                           \
        printf("  SKIP %s: %s\n", (name), (reason));                           \
    } while (0)

// Line-buffer stdout so a suite killed or crashed mid-run leaves every completed
// line on disk (block buffering would discard the whole trail), then announce it.
#define TEST_BEGIN(suite)                                                      \
    do {                                                                       \
        setvbuf(stdout, NULL, _IOLBF, 0);                                      \
        printf("== %s ==\n", (suite));                                         \
    } while (0)

// Print the per-suite line the Makefile aggregates, and return the exit code. A run
// that executed no checks (everything skipped) verified nothing, which is a failure
// distinct from a check failing: it returns 2, not 1. Set C64_ALLOW_NO_CHECKS=1 where
// an empty run is expected and acceptable.
static int test_summary(const char *suite) {
    printf("%s: %d passed, %d failed, %d skipped\n", suite, test_pass, test_fail,
           test_skip);
    if (test_pass + test_fail == 0) {
        const char *allow = getenv("C64_ALLOW_NO_CHECKS");
        if (!(allow && allow[0] == '1' && allow[1] == '\0')) {
            printf("RESULT: no checks ran in %s, nothing was verified "
                   "(set C64_ALLOW_NO_CHECKS=1 to allow)\n", suite);
            return 2;
        }
    }
    return test_fail == 0 ? 0 : 1;
}
#define TEST_SUMMARY(suite) test_summary(suite)

#endif // TEST_H
