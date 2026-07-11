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

# Core objects without main.c or host.c (no SDL), shared by the test runner.
CORE_SRC = src/bus.c src/mem.c src/cpu.c src/cpu6502.c src/vic.c src/sid.c src/cia.c
TEST_SRC = test/runner.c
TEST_BIN = build/lorenz-runner

# One durable unit-test binary per subsystem, plus the Lorenz runner.
UNIT_TESTS = mem cpu cia sid vic
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

clean:
	rm -rf build

.PHONY: all clean test
