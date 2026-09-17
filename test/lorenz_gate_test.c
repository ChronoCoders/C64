// Fixtures for the strict Lorenz CPU gate (test/lorenz_gate.h), in three layers:
// parser integrity, baseline integrity, and semantic comparison. These exercise the
// same parser and decision the gate binary uses, so a fixture that passes here pins the
// behavior the gate ships.
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "lorenz_gate.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#define LG_POSIX 1
#else
#define LG_POSIX 0
#endif

#define GOOD_LINE \
    "LORENZ_RESULT passed=236 last=TRAP15 stop_reason=NO_PROGRESS stop_test=TRAP16\n"

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        fputs(content, f);
        fclose(f);
    }
}

static void test_parser_integrity(void) {
    LgResult r;
    CHECK_EQ(lg_parse(GOOD_LINE, &r), LG_PARSE_OK, "well-formed canonical line parses");
    CHECK_EQ((long long)r.passed, 236, "parsed passed value");
    CHECK(strcmp(r.last, "TRAP15") == 0, "parsed last value");
    CHECK(strcmp(r.stop_reason, "NO_PROGRESS") == 0, "parsed stop_reason value");
    CHECK(strcmp(r.stop_test, "TRAP16") == 0, "parsed stop_test value");

    CHECK_EQ(lg_parse("Tests passed: 236\nStopped in test x\n", &r), LG_PARSE_NONE,
             "no canonical line is rejected");
    CHECK_EQ(lg_parse("LORENZ_RESULT passed=1 last=A stop_reason=JAM stop_test=B\n"
                      "LORENZ_RESULT passed=2 last=A stop_reason=JAM stop_test=B\n", &r),
             LG_PARSE_DUP_LINE, "duplicate canonical line is rejected");
    CHECK_EQ(lg_parse("LORENZ_RESULT passed 236 last=A stop_reason=JAM stop_test=B\n", &r),
             LG_PARSE_MALFORMED, "token without = is rejected");
    CHECK_EQ(lg_parse("LORENZ_RESULT  passed=236 last=A stop_reason=JAM stop_test=B\n", &r),
             LG_PARSE_MALFORMED, "double space is rejected");
    CHECK_EQ(lg_parse("LORENZ_RESULT passed=236 last=A stop_reason=JAM\n", &r),
             LG_PARSE_MISSING_FIELD, "missing field is rejected");
    CHECK_EQ(lg_parse("LORENZ_RESULT passed=236 last=A stop_reason=JAM stop_test=B extra=Z\n", &r),
             LG_PARSE_UNKNOWN_FIELD, "unknown extra field is rejected");
    CHECK_EQ(lg_parse("LORENZ_RESULT passed=236 passed=5 last=A stop_reason=JAM stop_test=B\n", &r),
             LG_PARSE_DUP_FIELD, "duplicate field is rejected");
    CHECK_EQ(lg_parse("LORENZ_RESULT passed=twelve last=A stop_reason=JAM stop_test=B\n", &r),
             LG_PARSE_BAD_PASSED, "non-numeric passed is rejected");
}

static void test_baseline_integrity(void) {
    CHECK_EQ(lg_decide(GOOD_LINE, "/tmp/lg_no_such_baseline_qa001.txt"), LG_BAD_BASELINE,
             "missing baseline fails");

    const char *malformed = "/tmp/lg_malformed_baseline_qa001.txt";
    write_file(malformed, "this is not a canonical result line\n");
    CHECK_EQ(lg_decide(GOOD_LINE, malformed), LG_BAD_BASELINE, "malformed baseline fails");
    remove(malformed);

#if LG_POSIX
    const char *unreadable = "/tmp/lg_unreadable_baseline_qa001.txt";
    write_file(unreadable, GOOD_LINE);
    chmod(unreadable, 0);
    CHECK_EQ(lg_decide(GOOD_LINE, unreadable), LG_BAD_BASELINE, "unreadable baseline fails");
    chmod(unreadable, 0600);
    remove(unreadable);
#else
    SKIP("unreadable baseline fails", "needs POSIX chmod to remove read permission");
#endif
}

static void test_semantic(void) {
    const char *baseline = "/tmp/lg_baseline_qa001.txt";
    write_file(baseline, GOOD_LINE);

    CHECK_EQ(lg_decide(GOOD_LINE, baseline), LG_OK, "exact four-field match passes");
    CHECK_EQ(lg_decide("LORENZ_RESULT passed=235 last=TRAP15 stop_reason=NO_PROGRESS stop_test=TRAP16\n",
                       baseline), LG_MISMATCH, "wrong passed fails");
    CHECK_EQ(lg_decide("LORENZ_RESULT passed=236 last=TRAP14 stop_reason=NO_PROGRESS stop_test=TRAP16\n",
                       baseline), LG_MISMATCH, "wrong last fails");
    CHECK_EQ(lg_decide("LORENZ_RESULT passed=236 last=TRAP15 stop_reason=JAM stop_test=TRAP16\n",
                       baseline), LG_MISMATCH, "wrong stop_reason fails");
    CHECK_EQ(lg_decide("LORENZ_RESULT passed=236 last=TRAP15 stop_reason=NO_PROGRESS stop_test=TRAP17\n",
                       baseline), LG_MISMATCH, "wrong stop_test fails");
    CHECK_EQ(lg_decide("LORENZ_RESULT passed=300 last=TRAP20 stop_reason=END_MARKER stop_test=TRAP20\n",
                       baseline), LG_MISMATCH, "advanced frontier fails until baseline is updated");
    CHECK_EQ(lg_decide("Tests passed: 236\nno canonical line here\n", baseline), LG_BAD_RESULT,
             "runner output with no canonical line fails");
    remove(baseline);
}

// QA-001 strict-parser gaps: falsification controls. Each anomalous line is measured
// against the documented canonical contract (leading/trailing space is malformed; passed
// is plain decimal). Parse outcome and gate decision are asserted separately so a
// malformed-but-accepted line is distinguished from one rejected by semantic mismatch.
static void test_parser_gaps(void) {
    const char *baseline = "/tmp/lg_gaps_baseline_qa001.txt";
    write_file(baseline, GOOD_LINE);
    LgResult r;

    CHECK_EQ(lg_parse(GOOD_LINE, &r), LG_PARSE_OK, "positive control: clean line parses");
    CHECK_EQ(lg_decide(GOOD_LINE, baseline), LG_OK, "positive control: clean line accepted");

    // Gap 1: one trailing space after the last token. Contract: trailing space malformed.
    const char *trail =
        "LORENZ_RESULT passed=236 last=TRAP15 stop_reason=NO_PROGRESS stop_test=TRAP16 \n";
    CHECK_EQ(lg_parse(trail, &r), LG_PARSE_MALFORMED, "trailing space is malformed (parse)");
    CHECK_EQ(lg_decide(trail, baseline), LG_BAD_RESULT, "trailing space is rejected (decide)");

    // Leading space before LORENZ_RESULT. Contract: leading space malformed.
    const char *lead =
        " LORENZ_RESULT passed=236 last=TRAP15 stop_reason=NO_PROGRESS stop_test=TRAP16\n";
    CHECK_EQ(lg_parse(lead, &r), LG_PARSE_NONE, "leading space not recognized as canonical (parse)");
    CHECK_EQ(lg_decide(lead, baseline), LG_BAD_RESULT, "leading space is rejected (decide)");

    // Gap 2a: passed=+236. Contract: passed is plain decimal, so a leading sign is malformed.
    const char *plus =
        "LORENZ_RESULT passed=+236 last=TRAP15 stop_reason=NO_PROGRESS stop_test=TRAP16\n";
    CHECK_EQ(lg_parse(plus, &r), LG_PARSE_BAD_PASSED, "passed=+236 is not plain decimal (parse)");
    CHECK_EQ(lg_decide(plus, baseline), LG_BAD_RESULT, "passed=+236 is rejected (decide)");

    // Gap 2b: passed=-1. A leading sign is not plain decimal, so it is rejected as a bad
    // passed value at parse and BAD_RESULT at the gate, not admitted and then caught only by
    // value mismatch as it was before the digit-only fix.
    const char *neg =
        "LORENZ_RESULT passed=-1 last=TRAP15 stop_reason=NO_PROGRESS stop_test=TRAP16\n";
    CHECK_EQ(lg_parse(neg, &r), LG_PARSE_BAD_PASSED, "passed=-1 is not plain decimal (parse)");
    CHECK_EQ(lg_decide(neg, baseline), LG_BAD_RESULT, "passed=-1 rejected as malformed passed (decide)");

    // Gap 3: leading whitespace in the passed value. The tokenizer splits only on 0x20, so a
    // value may begin with a tab; strtoul then skips it. Contract: plain decimal.
    const char *tab =
        "LORENZ_RESULT passed=\t236 last=TRAP15 stop_reason=NO_PROGRESS stop_test=TRAP16\n";
    CHECK_EQ(lg_parse(tab, &r), LG_PARSE_BAD_PASSED, "passed=<tab>236 is not plain decimal (parse)");
    CHECK_EQ(lg_decide(tab, baseline), LG_BAD_RESULT, "passed=<tab>236 is rejected (decide)");

    remove(baseline);
}

int main(void) {
    TEST_BEGIN("lorenz_gate");
    test_parser_integrity();
    test_baseline_integrity();
    test_semantic();
    test_parser_gaps();
    return TEST_SUMMARY("lorenz_gate");
}
