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
HL_BIN  := bin/yoke-highlight
HL_OWN  := build/highlight/yoke-highlight.o build/highlight/queries.o
HL_LANG := c cpp rust go python javascript typescript tsx bash json toml yaml
HL_PARSE := $(addprefix build/highlight/,$(addsuffix -parser.o,$(HL_LANG)))
HL_SCAN_LANG := cpp rust python javascript typescript tsx bash toml yaml
HL_SCAN := $(addprefix build/highlight/,$(addsuffix -scanner.o,$(HL_SCAN_LANG)))
HL_OBJ  := $(HL_OWN) build/highlight/tree-sitter.o $(HL_PARSE) $(HL_SCAN)
HL_CPPFLAGS := -Isrc -Ihighlight -Ivendor/tree-sitter/include
VENDOR_CFLAGS ?= -std=c17 -O2 -fno-strict-aliasing -pipe -w \
                 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L

PYTHON  ?= python3

.PHONY: all minimal clean run test test-update test-asan mock

all: $(BIN) $(HL_BIN)

minimal: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(OBJ) -o $@ $(LDFLAGS) $(LIBS)

$(OBJ): $(SRC) $(wildcard src/*.c) $(wildcard src/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $(SRC) -o $@

$(HL_BIN): $(HL_OBJ)
	@mkdir -p bin
	$(CC) $(HL_OBJ) -o $@ $(LDFLAGS)
	./$@ --self-test

build/highlight/yoke-highlight.o: highlight/yoke-highlight.c \
                                  highlight/queries.h src/highlight_protocol.h
	@mkdir -p build/highlight
	$(CC) $(CFLAGS) $(HL_CPPFLAGS) -c $< -o $@

build/highlight/queries.o: highlight/queries.c highlight/queries.h
	@mkdir -p build/highlight
	$(CC) $(CFLAGS) $(HL_CPPFLAGS) -c $< -o $@

build/highlight/tree-sitter.o: vendor/tree-sitter/runtime/lib.c \
                              $(wildcard vendor/tree-sitter/runtime/*.c) \
                              $(wildcard vendor/tree-sitter/runtime/*.h)
	@mkdir -p build/highlight
	$(CC) $(VENDOR_CFLAGS) $(HL_CPPFLAGS) -c $< -o $@

define HL_PARSER_RULE
build/highlight/$(1)-parser.o: vendor/tree-sitter/grammars/$(1)/parser.c \
                              $$(wildcard vendor/tree-sitter/include/tree_sitter/*.h)
	@mkdir -p build/highlight
	$$(CC) $$(VENDOR_CFLAGS) $$(HL_CPPFLAGS) -c $$< -o $$@
endef
$(foreach lang,$(HL_LANG),$(eval $(call HL_PARSER_RULE,$(lang))))

define HL_SCANNER_RULE
build/highlight/$(1)-scanner.o: vendor/tree-sitter/grammars/$(1)/scanner.c \
                               $$(wildcard vendor/tree-sitter/include/tree_sitter/*.h) \
                               $$(wildcard vendor/tree-sitter/common/*.h) \
                               $$(wildcard vendor/tree-sitter/grammars/$(1)/schema.*.c)
	@mkdir -p build/highlight
	$$(CC) $$(VENDOR_CFLAGS) $$(HL_CPPFLAGS) -c $$< -o $$@
endef
$(foreach lang,$(HL_SCAN_LANG),$(eval $(call HL_SCANNER_RULE,$(lang))))

run: $(BIN)
	./$(BIN)

test: all
	$(PYTHON) tests/run.py $(T)

test-update: all
	$(PYTHON) tests/run.py --update $(T)

# Rebuilds bin/yoke instrumented, runs the suite, then leaves it instrumented:
# `make` puts the normal binary back.
test-asan:
	$(MAKE) clean
	$(MAKE) all \
	    CFLAGS='$(filter-out -flto -D_FORTIFY_SOURCE=2,$(CFLAGS)) $(SANFLAGS)' \
	    LDFLAGS='$(filter-out -flto,$(LDFLAGS)) $(SANFLAGS)' \
	    VENDOR_CFLAGS='$(VENDOR_CFLAGS) $(SANFLAGS)'
	ASAN_OPTIONS=detect_leaks=0 $(PYTHON) tests/run.py $(T)

mock:
	$(PYTHON) -m tests.mockprovider.server $(MOCK_ARGS)

clean:
	rm -rf build bin
	rm -rf tests/__pycache__ tests/*/__pycache__
