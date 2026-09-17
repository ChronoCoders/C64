#include "lorenz_gate.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: lorenz-gate <baseline-file>\n");
        return 2;
    }
    char in[LG_BUF_CAP];
    size_t n = fread(in, 1u, sizeof in - 1u, stdin);
    int extra = fgetc(stdin);
    in[n] = '\0';
    if (extra != EOF) {
        fprintf(stderr, "lorenz-gate: runner output exceeds %u bytes\n",
                (unsigned)(LG_BUF_CAP - 1u));
        return 1;
    }
    switch (lg_decide(in, argv[1])) {
        case LG_OK:
            return 0;
        case LG_BAD_RESULT:
            fprintf(stderr, "lorenz-gate: runner output has no single well-formed "
                            "canonical result\n");
            return 1;
        case LG_BAD_BASELINE:
            fprintf(stderr, "lorenz-gate: baseline missing, unreadable, or malformed: "
                            "%s\n", argv[1]);
            return 1;
        case LG_MISMATCH:
            fprintf(stderr, "lorenz-gate: frontier does not match baseline %s\n", argv[1]);
            return 1;
    }
    return 1;
}
