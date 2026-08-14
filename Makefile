CC      ?= cc
# Everything the provider streams is untrusted input, so the cheap runtime
# checks stay on: they cost nothing measurable next to an SSE round trip.
CFLAGS  ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Wconversion \
           -fno-strict-aliasing -pipe -flto \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS ?= -flto
LIBS    ?= -lcurl

# The same suite against an instrumented binary: `make test-asan`. The
# sanitizer build lives in its own object and binary directories, so it never
# overwrites the shipped one and the two can coexist without a rebuild.
SANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all \
            -fno-omit-frame-pointer -g
ASAN_BUILD := build/asan
ASAN_BIN := bin/asan

# The same suite against a Fil-C binary: `make test-fil`. Fil-C shares no ABI
# with the system, so it cannot link the system libcurl; the compiler and every
# library it links must come from one Fil-C slice. Only the /opt/fil
# distribution ships a Fil-C libcurl, so that is what this target expects.
# Install it from https://fil-c.org/install_optfil.
FILCC ?= /opt/fil/bin/filcc
FIL_BUILD := build/fil
FIL_BIN := bin/fil
# Fil-C proves what these flags approximate, and LTO is not supported: keep the
# warnings and drop the hardening. -g gives Fil-C panics a symbolized trace.
FIL_DROP := -flto -fstack-protector-strong -D_FORTIFY_SOURCE=2

# BUILDDIR and BINDIR select a build variant; the sanitizer recipe overrides
# both. Everything below derives from them, so no rule writes a fixed path.
BUILDDIR ?= build
BINDIR  ?= bin

SRC     := src/main.c
OBJ     := $(BUILDDIR)/arqan.o
LEXBOR_OBJ := $(BUILDDIR)/vendor/lexbor.o
BIN     := $(BINDIR)/arqan
TEST_OBJ := $(BUILDDIR)/arqan-test.o
TEST_BIN := $(BINDIR)/arqan-test
HL_BIN  := $(BINDIR)/arqan-highlight
HL_OWN  := $(BUILDDIR)/highlight/arqan-highlight.o $(BUILDDIR)/highlight/queries.o
HL_LANG := c cpp rust go python javascript typescript tsx bash json toml yaml
HL_PARSE := $(addprefix $(BUILDDIR)/highlight/,$(addsuffix -parser.o,$(HL_LANG)))
HL_SCAN_LANG := cpp rust python javascript typescript tsx bash toml yaml
HL_SCAN := $(addprefix $(BUILDDIR)/highlight/,$(addsuffix -scanner.o,$(HL_SCAN_LANG)))
HL_OBJ  := $(HL_OWN) $(BUILDDIR)/highlight/tree-sitter.o $(HL_PARSE) $(HL_SCAN)
HL_CPPFLAGS := -Isrc -Ihighlight -Ivendor/tree-sitter/include \
               -Ivendor/tree-sitter/runtime
VENDOR_CFLAGS ?= -std=c17 -O2 -fno-strict-aliasing -pipe -w \
                 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L

PYTHON  ?= python3

.PHONY: all minimal clean clean-asan run test test-update asan test-asan mock \
        clean-fil fil test-fil bench bench-slow bench-baseline \
        package-linux test-package-linux release-linux

all: $(BIN) $(HL_BIN)

minimal: $(BIN)

$(BIN): $(OBJ) $(LEXBOR_OBJ)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(OBJ) $(LEXBOR_OBJ) -o $@ $(LDFLAGS) $(LIBS)

$(OBJ): $(SRC) $(wildcard src/*.c) $(wildcard src/*.h)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -c $(SRC) -o $@

$(LEXBOR_OBJ): vendor/lexbor/bridge.c vendor/lexbor/bridge.h \
               vendor/lexbor/lexbor.c
	@mkdir -p $(BUILDDIR)/vendor
	$(CC) $(VENDOR_CFLAGS) -Ivendor/lexbor -c $< -o $@

$(TEST_OBJ): $(SRC) $(wildcard src/*.c) $(wildcard src/*.h)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -DAGENT_TESTING -c $(SRC) -o $@

$(TEST_BIN): $(TEST_OBJ) $(LEXBOR_OBJ)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(TEST_OBJ) $(LEXBOR_OBJ) -o $@ $(LDFLAGS) $(LIBS)

$(HL_BIN): $(HL_OBJ)
	@mkdir -p $(BINDIR)
	$(CC) $(HL_OBJ) -o $@ $(LDFLAGS)
	./$@ --self-test

$(BUILDDIR)/highlight/arqan-highlight.o: highlight/arqan-highlight.c \
                                  highlight/queries.h src/highlight_protocol.h
	@mkdir -p $(BUILDDIR)/highlight
	$(CC) $(CFLAGS) $(HL_CPPFLAGS) -c $< -o $@

$(BUILDDIR)/highlight/queries.o: highlight/queries.c highlight/queries.h
	@mkdir -p $(BUILDDIR)/highlight
	$(CC) $(CFLAGS) $(HL_CPPFLAGS) -c $< -o $@

$(BUILDDIR)/highlight/tree-sitter.o: vendor/tree-sitter/runtime/lib.c \
                              $(wildcard vendor/tree-sitter/runtime/*.c) \
                              $(wildcard vendor/tree-sitter/runtime/*.h)
	@mkdir -p $(BUILDDIR)/highlight
	$(CC) $(VENDOR_CFLAGS) $(HL_CPPFLAGS) -c $< -o $@

define HL_PARSER_RULE
$$(BUILDDIR)/highlight/$(1)-parser.o: vendor/tree-sitter/grammars/$(1)/parser.c \
                              $$(wildcard vendor/tree-sitter/include/tree_sitter/*.h)
	@mkdir -p $$(BUILDDIR)/highlight
	$$(CC) $$(VENDOR_CFLAGS) $$(HL_CPPFLAGS) -c $$< -o $$@
endef
$(foreach lang,$(HL_LANG),$(eval $(call HL_PARSER_RULE,$(lang))))

define HL_SCANNER_RULE
$$(BUILDDIR)/highlight/$(1)-scanner.o: vendor/tree-sitter/grammars/$(1)/scanner.c \
                               $$(wildcard vendor/tree-sitter/include/tree_sitter/*.h) \
                               $$(wildcard vendor/tree-sitter/common/*.h) \
                               $$(wildcard vendor/tree-sitter/grammars/$(1)/schema.*.c)
	@mkdir -p $$(BUILDDIR)/highlight
	$$(CC) $$(VENDOR_CFLAGS) $$(HL_CPPFLAGS) -c $$< -o $$@
endef
$(foreach lang,$(HL_SCAN_LANG),$(eval $(call HL_SCANNER_RULE,$(lang))))

run: $(BIN)
	./$(BIN)

test: all $(TEST_BIN)
	ARQAN_TEST_BIN=$(TEST_BIN) $(PYTHON) tests/run.py $(T)

test-update: all $(TEST_BIN)
	ARQAN_TEST_BIN=$(TEST_BIN) $(PYTHON) tests/run.py --update $(T)

# The instrumented tree is built and run entirely under build/asan and
# bin/asan, so bin/arqan stays the shipped binary and a bare
# `python3 tests/run.py` keeps testing it.
asan:
	$(MAKE) all $(ASAN_BIN)/arqan-test \
	    BUILDDIR='$(ASAN_BUILD)' BINDIR='$(ASAN_BIN)' \
	    CFLAGS='$(filter-out -flto -D_FORTIFY_SOURCE=2,$(CFLAGS)) $(SANFLAGS)' \
	    LDFLAGS='$(filter-out -flto,$(LDFLAGS)) $(SANFLAGS)' \
	    VENDOR_CFLAGS='$(VENDOR_CFLAGS) $(SANFLAGS)'

test-asan: asan
	ASAN_OPTIONS=detect_leaks=0 ARQAN_TEST_BIN=$(ASAN_BIN)/arqan-test \
	    $(PYTHON) tests/run.py $(T)

# Fil-C catches what the sanitizers can only sample, and it needs no leak
# suppression: it collects. Same variant layout as asan, under build/fil and
# bin/fil.
fil:
	@command -v $(FILCC) >/dev/null 2>&1 || { \
	    echo "$(FILCC) not found. Install the Fil-C /opt/fil distribution"; \
	    echo "from https://fil-c.org/install_optfil, or set FILCC=<path>."; \
	    exit 1; }
	$(MAKE) all $(FIL_BIN)/arqan-test \
	    CC='$(FILCC)' BUILDDIR='$(FIL_BUILD)' BINDIR='$(FIL_BIN)' \
	    CFLAGS='$(filter-out $(FIL_DROP),$(CFLAGS)) -g' \
	    LDFLAGS='$(filter-out $(FIL_DROP),$(LDFLAGS))' \
	    VENDOR_CFLAGS='$(filter-out $(FIL_DROP),$(VENDOR_CFLAGS)) -g'

test-fil: fil
	ARQAN_TEST_BIN=$(FIL_BIN)/arqan-test $(PYTHON) tests/run.py $(T)

mock:
	$(PYTHON) -m tests.mockprovider.server $(MOCK_ARGS)

# Benchmarks measure the shipped binary, never the instrumented one, and run
# one case at a time: a benchmark sharing the machine measures its neighbours.
bench: all
	$(PYTHON) -m bench.run $(B)

bench-slow: all
	$(PYTHON) -m bench.run --slow $(B)

# Record a baseline, then `make bench B="--baseline bench-baseline.json"`.
bench-baseline: all
	$(PYTHON) -m bench.run --slow --json bench-baseline.json $(B)

package-linux: all
	./scripts/package-linux.sh

test-package-linux: package-linux
	$(PYTHON) tests/package_linux.py

release-linux:
	./scripts/release-linux.sh

clean:
	rm -rf build bin dist
	rm -rf tests/__pycache__ tests/*/__pycache__

# Drop only the instrumented tree; the shipped build survives.
clean-asan:
	rm -rf $(ASAN_BUILD) $(ASAN_BIN)

# Drop only the Fil-C tree; the shipped build survives.
clean-fil:
	rm -rf $(FIL_BUILD) $(FIL_BIN)
