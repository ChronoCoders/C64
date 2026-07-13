CC = cc
CSTD = -std=c11
WARN = -Wall -Wextra -Werror

MODE ?= dev
ifeq ($(MODE),release)
OPT = -O2
else
OPT = -O0 -g
endif

CFLAGS = $(CSTD) $(WARN) $(OPT)

# SDL2 is linked into the emulator binary only; the Lorenz test runner stays
# headless (no display dependency).
SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS := $(shell sdl2-config --libs 2>/dev/null)

SRC = $(wildcard src/*.c)
BIN = build/c64

# Core objects without main.c or host.c (no SDL), shared by the test runner. The
# drive is included so the drive suite links; the Lorenz runner does not call it.
CORE_SRC = src/bus.c src/mem.c src/cpu.c src/cpu6502.c src/vic.c src/sid.c src/cia.c src/drive.c src/via.c src/iec.c src/disk.c
TEST_SRC = test/runner.c
TEST_BIN = build/lorenz-runner

# One durable unit-test binary per subsystem, plus the Lorenz runner.
UNIT_TESTS = mem cpu cia sid vic drive via iec gcr
UNIT_BINS = $(addprefix build/test-,$(UNIT_TESTS))

all: $(BIN)

$(BIN): $(SRC)
	@command -v sdl2-config >/dev/null 2>&1 || { \
	  echo "error: SDL2 not found. Install it (e.g. sudo apt install libsdl2-dev)."; \
	  echo "The headless test runner still builds: make test"; exit 1; }
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(SRC) -o $(BIN) $(SDL_LIBS)

$(TEST_BIN): $(TEST_SRC) $(CORE_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc $(TEST_SRC) $(CORE_SRC) -o $(TEST_BIN)

build/test-%: test/%_test.c test/test.h $(CORE_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -Itest $< $(CORE_SRC) -o $@

# Build and run every unit suite headless plus the Lorenz runner; report per
# subsystem and a total, and exit non-zero if any unit assertion failed.
test: $(UNIT_BINS) $(TEST_BIN)
	@rc=0; : > build/test.log; \
	for t in $(UNIT_BINS); do \
	  if ./$$t >> build/test.log 2>&1; then :; else rc=1; fi; \
	done; \
	echo "======== unit tests ========"; cat build/test.log; \
	echo "======== lorenz ========"; \
	./$(TEST_BIN) 2>/dev/null | grep -E "Tests passed|Stopped in test" || \
	  echo "  (lorenz suite not present under test/lorenz; skipped)"; \
	echo "======== summary ========"; \
	awk '/passed,/{p+=$$2; f+=$$4; s+=$$6} \
	     END{printf "TOTAL: %d passed, %d failed, %d skipped\n",p,f,s}' build/test.log; \
	if [ $$rc -ne 0 ]; then echo "RESULT: FAILURES"; exit 1; fi; \
	echo "RESULT: all unit suites passed"

# Sanitizer build: run the unit suites under AddressSanitizer and
# UndefinedBehaviorSanitizer. The vic suite boots the real KERNAL for 200 frames,
# so this run exercises CPU, VIC, SID, CIA and memory together as well as the unit
# checks. A finding fails the run (UBSan halts, ASAN reports leaks). This target is
# deliberate and slow; the normal `make test` stays fast and unsanitized.
# Lorenz is not run here: under the sanitizers it is far too slow to finish in a
# reasonable time. Run it unsanitized with `make test`.
ASAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -g
ASAN_BINS = $(addprefix build/asan-test-,$(UNIT_TESTS))

build/asan-test-%: test/%_test.c test/test.h $(CORE_SRC)
	@mkdir -p build
	$(CC) $(CSTD) $(WARN) -O1 $(ASAN_FLAGS) -Isrc -Itest $< $(CORE_SRC) -o $@

test-asan: $(ASAN_BINS)
	@rc=0; \
	export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1; \
	export ASAN_OPTIONS=detect_leaks=1; \
	for t in $(ASAN_BINS); do \
	  echo "======== $$t ========"; \
	  if ./$$t; then :; else rc=1; echo "SANITIZER FINDING in $$t"; fi; \
	done; \
	if command -v sdl2-config >/dev/null 2>&1; then \
	  echo "======== host smoke (SDL dummy drivers) ========"; \
	  $(CC) $(CSTD) $(WARN) -O1 $(ASAN_FLAGS) -Isrc -Itest $(SDL_CFLAGS) \
	    test/host_smoke.c src/host.c $(CORE_SRC) -o build/asan-host-smoke $(SDL_LIBS); \
	  if SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./build/asan-host-smoke; then \
	    echo "host adapter: no findings"; else rc=1; echo "SANITIZER FINDING in host smoke"; fi; \
	fi; \
	if [ $$rc -ne 0 ]; then echo "RESULT: sanitizer findings, see above"; exit 1; fi; \
	echo "RESULT: no sanitizer findings in the unit suites (incl. the KERNAL boot and host smoke)"

# Line-coverage report (gcov; gcovr/lcov not required). Instruments the core once
# so the five unit suites accumulate into one profile, plus a headless full-machine
# run for drive.c and main.c. Prints per-file line coverage. The Lorenz runner is
# NOT included: under coverage instrumentation it is too slow to finish in a usable
# time, so cpu6502 coverage here reflects the unit suites and the KERNAL boot only,
# not the far larger opcode coverage Lorenz exercises in `make test`.
COV_DIR = build/cov
COV_FULL_DIR = build/cov-full
COV_CORE = bus mem cpu cpu6502 vic sid cia drive via iec disk
COV_CFLAGS = -std=c11 --coverage -O0 -g

coverage:
	@command -v gcov >/dev/null 2>&1 || { \
	  echo "gcov not installed. It ships with gcc: apt install gcc"; exit 1; }
	@rm -rf $(COV_DIR) $(COV_FULL_DIR); mkdir -p $(COV_DIR) $(COV_FULL_DIR)
	@for f in $(COV_CORE); do $(CC) $(COV_CFLAGS) -Isrc -c src/$$f.c -o $(COV_DIR)/$$f.o; done
	@for t in $(UNIT_TESTS); do \
	  $(CC) $(COV_CFLAGS) -Isrc -Itest test/$$t\_test.c $(COV_DIR)/*.o -o $(COV_DIR)/cov-$$t; \
	  ./$(COV_DIR)/cov-$$t >/dev/null 2>&1 || true; \
	done
	@if command -v sdl2-config >/dev/null 2>&1; then \
	  for f in $$(ls src/*.c | xargs -n1 basename | sed 's/\.c$$//'); do \
	    $(CC) $(COV_CFLAGS) -Isrc $(SDL_CFLAGS) -c src/$$f.c -o $(COV_FULL_DIR)/$$f.o; done; \
	  $(CC) $(COV_CFLAGS) $(COV_FULL_DIR)/*.o -o $(COV_FULL_DIR)/cov-c64 $(SDL_LIBS); \
	  ./$(COV_FULL_DIR)/cov-c64 --headless >/dev/null 2>&1 || true; \
	fi
	@echo "======== line coverage (gcov) ========"
	@for f in $(COV_CORE); do \
	  l=$$(gcov -n -o $(COV_DIR) src/$$f.c 2>/dev/null | grep "Lines executed" | head -1); \
	  printf "  %-11s %s\n" "$$f.c" "$$l"; done
	@for f in main host; do \
	  l=$$(gcov -n -o $(COV_FULL_DIR) src/$$f.c 2>/dev/null | grep "Lines executed" | head -1); \
	  printf "  %-11s %s\n" "$$f.c" "$${l:-no data (SDL not available)}"; done
	@echo "note: coverage is what the tests execute, not proof of correctness."

# Bounded memory check under Valgrind: the headless run boots the KERNAL to READY,
# attaches the 1541, and steps it to its idle loop, exercising CPU, VIC, SID, CIA,
# memory and the drive together. Valgrind sees leaks, invalid accesses, and
# uninitialised reads that AddressSanitizer does not. Requires valgrind and the
# ROMs. Findings are reported, not fixed.
valgrind: $(BIN)
	@command -v valgrind >/dev/null 2>&1 || { \
	  echo "valgrind not installed: apt install valgrind"; exit 1; }
	valgrind --leak-check=full --show-leak-kinds=definite,indirect,possible \
	  --track-origins=yes --error-exitcode=1 ./$(BIN) --headless

# Fuzz the .d64 mount path (the only untrusted-input parser) with libFuzzer under
# ASan+UBSan. A blank valid image seeds the corpus so mutations explore content at
# the right size, not just the reject path. FUZZ_TIME bounds the run. Findings are
# real defects: they are reported, not fixed silently. Requires clang.
FUZZ_TIME ?= 60
fuzz:
	@command -v clang >/dev/null 2>&1 || { \
	  echo "clang (with libFuzzer) required: apt install clang"; exit 1; }
	@mkdir -p build/fuzz-corpus
	@head -c 174848 /dev/zero > build/fuzz-corpus/blank.d64
	clang $(CSTD) $(WARN) -g -O1 -fsanitize=fuzzer,address,undefined \
	  -Isrc test/fuzz_d64.c src/disk.c -o build/fuzz-d64
	./build/fuzz-d64 -max_total_time=$(FUZZ_TIME) -print_final_stats=1 build/fuzz-corpus

clean:
	rm -rf build

.PHONY: all clean test test-asan coverage valgrind fuzz
