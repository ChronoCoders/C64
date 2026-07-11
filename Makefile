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

all: $(BIN)

$(BIN): $(SRC)
	@command -v sdl2-config >/dev/null 2>&1 || { \
	  echo "error: SDL2 not found. Install it (e.g. sudo apt install libsdl2-dev)."; \
	  echo "The headless test runner still builds: make test"; exit 1; }
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(SRC) -o $(BIN) $(SDL_LIBS)

test: $(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(CORE_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc $(TEST_SRC) $(CORE_SRC) -o $(TEST_BIN)

clean:
	rm -rf build

.PHONY: all clean test
