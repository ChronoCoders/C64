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

all: $(BIN)

$(BIN): $(SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -o $(BIN)

clean:
	rm -rf build

.PHONY: all clean
