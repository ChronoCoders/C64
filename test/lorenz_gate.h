//! Strict parser and comparator for the Lorenz CPU-conformance gate. Header-only so
//! the gate binary (lorenz_gate.c) and its fixtures (lorenz_gate_test.c) share one
//! implementation. The gate accepts a run only when its single canonical line
//! (LORENZ_RESULT passed=<uint> last=<id> stop_reason=<id> stop_test=<id>) parses
//! strictly and matches the checked-in baseline on all four fields.
#ifndef LORENZ_GATE_H
#define LORENZ_GATE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LG_ID_CAP 64u
#define LG_BUF_CAP 65536u

typedef struct {
    unsigned long passed;
    char last[LG_ID_CAP];
    char stop_reason[LG_ID_CAP];
    char stop_test[LG_ID_CAP];
} LgResult;

typedef enum {
    LG_PARSE_OK = 0,
    LG_PARSE_NONE,          // no canonical line
    LG_PARSE_DUP_LINE,      // more than one canonical line
    LG_PARSE_MALFORMED,     // token without a single key=value, or stray whitespace
    LG_PARSE_MISSING_FIELD, // one of the four required keys absent
    LG_PARSE_UNKNOWN_FIELD, // a key outside the four
    LG_PARSE_DUP_FIELD,     // a required key repeated
    LG_PARSE_BAD_PASSED,    // passed is not a plain decimal
} LgParse;

typedef enum {
    LG_OK = 0,
    LG_BAD_RESULT,   // runner output did not parse strictly
    LG_BAD_BASELINE, // baseline missing, unreadable, or malformed
    LG_MISMATCH,     // parsed cleanly but the frontier differs from the baseline
} LgStatus;

static int lg_key_index(const char *key) {
    if (strcmp(key, "passed") == 0) { return 0; }
    if (strcmp(key, "last") == 0) { return 1; }
    if (strcmp(key, "stop_reason") == 0) { return 2; }
    if (strcmp(key, "stop_test") == 0) { return 3; }
    return -1;
}

// Parse one canonical line (no trailing newline). Splits on single spaces so a double
// space or leading/trailing space is malformed, not silently collapsed.
static LgParse lg_parse_line(const char *line, LgResult *out) {
    char tok[LG_ID_CAP + 16u];
    size_t pos = 0;
    size_t len = strlen(line);
    size_t tlen = 0;
    if (len < 13u || strncmp(line, "LORENZ_RESULT", 13u) != 0 || line[13] != ' ') {
        return LG_PARSE_MALFORMED;
    }
    pos = 14u;  // past "LORENZ_RESULT "
    int seen[4] = {0, 0, 0, 0};
    unsigned long passed = 0;
    char vals[4][LG_ID_CAP] = {{0}, {0}, {0}, {0}};
    while (pos <= len) {
        if (pos == len) { break; }  // last token ended at end of line
        tlen = 0;
        while (pos < len && line[pos] != ' ') {
            if (tlen + 1u >= sizeof tok) { return LG_PARSE_MALFORMED; }
            tok[tlen++] = line[pos++];
        }
        tok[tlen] = '\0';
        if (pos < len && line[pos] == ' ') {
            pos++;
            if (pos == len) { return LG_PARSE_MALFORMED; }  // trailing separator space
        }
        if (tlen == 0u) { return LG_PARSE_MALFORMED; }  // stray/double space
        char *eq = strchr(tok, '=');
        if (eq == NULL || eq == tok || eq[1] == '\0') { return LG_PARSE_MALFORMED; }
        *eq = '\0';
        const char *key = tok;
        const char *val = eq + 1;
        int ki = lg_key_index(key);
        if (ki < 0) { return LG_PARSE_UNKNOWN_FIELD; }
        if (seen[ki]) { return LG_PARSE_DUP_FIELD; }
        seen[ki] = 1;
        if (ki == 0) {
            for (const char *d = val; *d != '\0'; d++) {
                if (*d < '0' || *d > '9') { return LG_PARSE_BAD_PASSED; }  // plain decimal only
            }
            char *end = NULL;
            passed = strtoul(val, &end, 10);
            if (end == val || *end != '\0') { return LG_PARSE_BAD_PASSED; }
        } else {
            if (strlen(val) + 1u > LG_ID_CAP) { return LG_PARSE_MALFORMED; }
            snprintf(vals[ki], LG_ID_CAP, "%s", val);
        }
    }
    for (int i = 0; i < 4; i++) {
        if (!seen[i]) { return LG_PARSE_MISSING_FIELD; }
    }
    out->passed = passed;
    snprintf(out->last, LG_ID_CAP, "%s", vals[1]);
    snprintf(out->stop_reason, LG_ID_CAP, "%s", vals[2]);
    snprintf(out->stop_test, LG_ID_CAP, "%s", vals[3]);
    return LG_PARSE_OK;
}

// Parse text that may hold human-readable lines plus exactly one canonical line.
static LgParse lg_parse(const char *text, LgResult *out) {
    const char *p = text;
    int found = 0;
    char line[LG_ID_CAP * 6u];
    const char *canonical = NULL;
    char saved[LG_ID_CAP * 6u];
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t n = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
        if (n > 0u && p[n - 1u] == '\r') { n--; }
        if (n >= 13u && strncmp(p, "LORENZ_RESULT", 13u) == 0) {
            found++;
            if (found > 1) { return LG_PARSE_DUP_LINE; }
            if (n + 1u > sizeof line) { return LG_PARSE_MALFORMED; }
            memcpy(line, p, n);
            line[n] = '\0';
            memcpy(saved, line, n + 1u);
            canonical = saved;
        }
        if (nl == NULL) { break; }
        p = nl + 1u;
    }
    if (found == 0) { return LG_PARSE_NONE; }
    return lg_parse_line(canonical, out);
}

static long lg_read_file(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) { return -1; }
    size_t n = fread(buf, 1u, cap - 1u, f);
    int extra = fgetc(f);
    fclose(f);
    if (extra != EOF) { return -1; }  // larger than the buffer: treat as unusable
    buf[n] = '\0';
    return (long)n;
}

static int lg_result_eq(const LgResult *a, const LgResult *b) {
    return a->passed == b->passed && strcmp(a->last, b->last) == 0 &&
           strcmp(a->stop_reason, b->stop_reason) == 0 &&
           strcmp(a->stop_test, b->stop_test) == 0;
}

// The whole gate decision: strict-parse the runner text, load and strict-parse the
// baseline, and require an exact four-field match.
static LgStatus lg_decide(const char *runner_text, const char *baseline_path) {
    LgResult r;
    if (lg_parse(runner_text, &r) != LG_PARSE_OK) { return LG_BAD_RESULT; }
    char bbuf[LG_BUF_CAP];
    if (lg_read_file(baseline_path, bbuf, sizeof bbuf) < 0) { return LG_BAD_BASELINE; }
    LgResult b;
    if (lg_parse(bbuf, &b) != LG_PARSE_OK) { return LG_BAD_BASELINE; }
    return lg_result_eq(&r, &b) ? LG_OK : LG_MISMATCH;
}

#endif // LORENZ_GATE_H
