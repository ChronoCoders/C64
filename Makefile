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

SRC = $(wildcard src/*.c)
BIN = build/c64

# Core objects without main.c, shared by the test runner (two mains would clash).
CORE_SRC = src/bus.c src/mem.c src/cpu.c
TEST_SRC = test/runner.c
TEST_BIN = build/lorenz-runner

all: $(BIN)

$(BIN): $(SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -o $(BIN)

test: $(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(CORE_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc $(TEST_SRC) $(CORE_SRC) -o $(TEST_BIN)

clean:
	rm -rf build

.PHONY: all clean test
