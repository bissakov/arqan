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

.PHONY: all clean run

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(OBJ) -o $@ $(LDFLAGS) $(LIBS)

$(OBJ): $(SRC) $(wildcard src/*.c) $(wildcard src/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $(SRC) -o $@

run: $(BIN)
	./$(BIN)

clean:
	rm -rf build bin
