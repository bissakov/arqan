CC      ?= cc
# Everything the provider streams is untrusted input, so the cheap runtime
# checks stay on: they cost nothing measurable next to an SSE round trip.
CFLAGS  ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Wconversion \
           -fno-strict-aliasing -pipe -flto \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS ?= -flto
LIBS    ?= -lcurl

# The same suite against an instrumented binary: `make test-asan`.
SANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all \
            -fno-omit-frame-pointer -g

SRC     := src/main.c
OBJ     := build/yoke.o
BIN     := bin/yoke

PYTHON  ?= python3

.PHONY: all clean run test test-update test-asan mock

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(OBJ) -o $@ $(LDFLAGS) $(LIBS)

$(OBJ): $(SRC) $(wildcard src/*.c) $(wildcard src/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $(SRC) -o $@

run: $(BIN)
	./$(BIN)

test: $(BIN)
	$(PYTHON) tests/run.py $(T)

test-update: $(BIN)
	$(PYTHON) tests/run.py --update $(T)

# Rebuilds bin/yoke instrumented, runs the suite, then leaves it instrumented:
# `make` puts the normal binary back.
test-asan:
	@mkdir -p bin
	$(CC) $(filter-out -flto -D_FORTIFY_SOURCE=2,$(CFLAGS)) $(SANFLAGS) \
	    $(SRC) -o $(BIN) $(LIBS)
	ASAN_OPTIONS=detect_leaks=0 $(PYTHON) tests/run.py $(T)

mock:
	$(PYTHON) -m tests.mockprovider.server $(MOCK_ARGS)

clean:
	rm -rf build bin
	rm -rf tests/__pycache__ tests/*/__pycache__
