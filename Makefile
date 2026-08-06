CC      ?= cc
CFLAGS  ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Wconversion \
           -fno-strict-aliasing -pipe -flto
LDFLAGS ?= -flto
LIBS    ?= -lcurl

SRC     := src/main.c
OBJ     := build/yoke.o
BIN     := bin/yoke

PYTHON  ?= python3

.PHONY: all clean run test test-update mock

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

mock:
	$(PYTHON) -m tests.mockprovider.server $(MOCK_ARGS)

clean:
	rm -rf build bin
	rm -rf tests/__pycache__ tests/*/__pycache__
