# ah — C17 AI coding harness
# Unity build: main.c #includes every other .c, so we compile one TU.

CC      ?= cc
CFLAGS  ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Wconversion \
           -fno-strict-aliasing -pipe -flto
LDFLAGS ?= -flto
LIBS    ?= -lcurl

SRC     := src/main.c
OBJ     := build/ah.o
BIN     := bin/ah

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

# End-to-end TUI tests: ah is driven inside a pty against a dummy provider.
# Python 3 only, no third-party packages.
test: $(BIN)
	$(PYTHON) tests/run.py $(T)

test-update: $(BIN)
	$(PYTHON) tests/run.py --update $(T)

# Dummy OpenAI-compatible provider, for driving the UI by hand.
mock:
	$(PYTHON) -m tests.mockprovider.server $(MOCK_ARGS)

clean:
	rm -rf build bin
	rm -rf tests/__pycache__ tests/*/__pycache__
