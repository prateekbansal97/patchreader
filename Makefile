.DEFAULT_GOAL := all
CC      ?= cc
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
BASE    := -std=c11 -D_POSIX_C_SOURCE=200809L -Iinclude $(WARN)
CFLAGS  ?= -O2 -g
SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

LIB_SRC  := $(filter-out src/main.c,$(wildcard src/*.c))
HDRS     := $(wildcard include/*.h tests/*.h)
TESTS    := $(wildcard tests/test_*.c)
TEST_BIN := $(patsubst tests/%.c,build/tests/%,$(TESTS))

.PHONY: all asan test clean

.PHONY: ide
ide: patchreader patchreader-asan $(TEST_BIN)

.PHONY: pty
pty: patchreader-asan
	python3 tests/pty_ui.py ./patchreader-asan

all: patchreader

patchreader: src/main.c $(LIB_SRC) $(HDRS)
	$(CC) $(BASE) $(CFLAGS) -o $@ src/main.c $(LIB_SRC)

asan: patchreader-asan

patchreader-asan: src/main.c $(LIB_SRC) $(HDRS)
	$(CC) $(BASE) -O1 -g $(SAN) -o $@ src/main.c $(LIB_SRC)

.PHONY: debug
debug: patchreader-debug

patchreader-debug: src/main.c $(LIB_SRC) $(HDRS)
	$(CC) $(BASE) -O0 -g -fno-omit-frame-pointer -o $@ src/main.c $(LIB_SRC)

test: $(TEST_BIN)
	@set -e; for t in $(TEST_BIN); do echo "== $$t"; $$t; done

build/tests/%: tests/%.c $(LIB_SRC) $(HDRS)
	@mkdir -p build/tests
	$(CC) $(BASE) -O1 -g $(SAN) -Itests -o $@ $< $(LIB_SRC)

clean:
	rm -rf build patchreader patchreader-asan patchreader-debug *.dSYM

FUZZ_SECONDS ?= 30

.PHONY: fuzz bench audit check

fuzz: build/fuzz_pipeline
	./build/fuzz_pipeline $(FUZZ_SECONDS)

build/fuzz_pipeline: tests/fuzz_pipeline.c $(LIB_SRC) $(HDRS)
	@mkdir -p build
	$(CC) $(BASE) -O1 -g $(SAN) -Itests -o $@ $< $(LIB_SRC)

bench: build/bench
	./build/bench

build/bench: tests/bench.c $(LIB_SRC) $(HDRS)
	@mkdir -p build
	$(CC) $(BASE) -O2 -Itests -o $@ $< $(LIB_SRC)

audit: patchreader
	sh tests/audit.sh

check: test pty audit bench
