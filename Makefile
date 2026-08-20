CC      ?= cc
SIZE_CFLAGS := -ffunction-sections -fdata-sections \
               -fno-asynchronous-unwind-tables -fno-unwind-tables
SIZE_LDFLAGS := -Wl,-O1,--gc-sections -s
CFLAGS  ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Wconversion \
           -fno-strict-aliasing -pipe -flto=auto $(SIZE_CFLAGS) \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS ?= -flto=auto $(SIZE_LDFLAGS)
LIBS    ?= -lcurl

CFLAGS += $(EXTRA_CFLAGS)

CURL_MODE ?= dlopen
CURL_CFLAGS :=
ifeq ($(CURL_MODE),dlopen)
CURL_CFLAGS := -DAGENT_CURL_DLOPEN=1
LIBS := -ldl
endif

SANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all \
            -fno-omit-frame-pointer -g
ASAN_BUILD := build/asan
ASAN_BIN := bin/asan

FILCC ?= /opt/fil/bin/filcc
FIL_BUILD := build/fil
FIL_BIN := bin/fil
FIL_DROP := -flto=auto -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
            $(SIZE_CFLAGS) $(SIZE_LDFLAGS)

STATIC_BUILD := build/musl
STATIC_BIN := bin/musl
STATIC_LIBS ?= $(shell pkg-config --static --libs libcurl)

EL9_BUILD := build/el9
EL9_BIN := bin/el9

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
VENDOR_CFLAGS ?= -std=c17 -Os -fno-strict-aliasing -pipe -flto=auto -w \
                 $(SIZE_CFLAGS) -D_DEFAULT_SOURCE \
                 -D_POSIX_C_SOURCE=200809L

PYTHON  ?= python3

.PHONY: all minimal clean clean-asan run test test-update asan test-asan mock \
        test-ci \
        clean-fil fil test-fil clean-static static test-static \
        clean-el9 el9 test-el9 \
        bench bench-slow bench-baseline \
        bench-guard check-curl-types \
        package-linux test-package-linux release-linux

all: $(BIN) $(HL_BIN)

minimal: $(BIN)

$(BIN): $(OBJ) $(LEXBOR_OBJ)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(OBJ) $(LEXBOR_OBJ) -o $@ $(LDFLAGS) $(LIBS)

$(OBJ): $(SRC) $(wildcard src/*.c) $(wildcard src/*.h)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) $(CURL_CFLAGS) -c $(SRC) -o $@

$(LEXBOR_OBJ): vendor/lexbor/bridge.c vendor/lexbor/bridge.h \
               vendor/lexbor/lexbor.c
	@mkdir -p $(BUILDDIR)/vendor
	$(CC) $(VENDOR_CFLAGS) -Ivendor/lexbor -c $< -o $@

$(TEST_OBJ): $(SRC) $(wildcard src/*.c) $(wildcard src/*.h)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) $(CURL_CFLAGS) -DAGENT_TESTING -c $(SRC) -o $@

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

CI_JOBS ?= 4
test-ci: all $(TEST_BIN)
	ARQAN_TEST_BIN=$(TEST_BIN) ARQAN_TEST_JOBS=$(CI_JOBS) \
	    $(PYTHON) tests/run.py $(T)

asan:
	$(MAKE) all $(ASAN_BIN)/arqan-test \
	    BUILDDIR='$(ASAN_BUILD)' BINDIR='$(ASAN_BIN)' \
	    CFLAGS='$(filter-out -flto=auto -D_FORTIFY_SOURCE=2 $(SIZE_CFLAGS),$(CFLAGS)) $(SANFLAGS)' \
	    LDFLAGS='$(filter-out -flto=auto $(SIZE_LDFLAGS),$(LDFLAGS)) $(SANFLAGS)' \
	    VENDOR_CFLAGS='$(filter-out -flto=auto $(SIZE_CFLAGS),$(VENDOR_CFLAGS)) $(SANFLAGS)'

test-asan: asan
	ASAN_OPTIONS=detect_leaks=0 ARQAN_TEST_BIN=$(ASAN_BIN)/arqan-test \
	    $(PYTHON) tests/run.py $(T)

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

static:
	@case '$(shell $(CC) -dumpmachine)' in *musl*) ;; *) \
	    echo "$(CC) targets $(shell $(CC) -dumpmachine), not musl. Run"; \
	    echo "scripts/build-musl.sh, which builds and enters the builder"; \
	    echo "image this target expects."; \
	    exit 1 ;; esac
	@pkg-config --exists libcurl 2>/dev/null || { \
	    echo "pkg-config cannot describe libcurl. Install its development"; \
	    echo "package, or pass the full archive list as STATIC_LIBS."; \
	    exit 1; }
	$(MAKE) all $(STATIC_BIN)/arqan-test \
	    BUILDDIR='$(STATIC_BUILD)' BINDIR='$(STATIC_BIN)' \
	    CFLAGS='$(CFLAGS) -static-pie' LDFLAGS='$(LDFLAGS) -static-pie' \
	    CURL_MODE=link LIBS='$(STATIC_LIBS)'

test-static: static
	ARQAN_TEST_BIN=$(STATIC_BIN)/arqan-test $(PYTHON) tests/run.py $(T)

check-curl-types:
	$(MAKE) bin/link/arqan CURL_MODE=link BUILDDIR=build/link BINDIR=bin/link

el9:
	$(MAKE) all $(EL9_BIN)/arqan-test \
	    BUILDDIR='$(EL9_BUILD)' BINDIR='$(EL9_BIN)'

test-el9: el9
	ARQAN_TEST_BIN=$(EL9_BIN)/arqan-test $(PYTHON) tests/run.py $(T)

mock:
	$(PYTHON) -m tests.mockprovider.server $(MOCK_ARGS)

bench: all
	$(PYTHON) -m bench.run $(B)

bench-slow: all
	$(PYTHON) -m bench.run --slow $(B)

bench-fast: all $(TEST_BIN)
	ARQAN_TEST_BIN=$(TEST_BIN) $(PYTHON) -m bench.run $(B)

bench-baseline: all
	$(PYTHON) -m bench.run --slow --json bench-baseline.json $(B)

REF ?= HEAD
bench-guard:
	./scripts/bench-guard.sh $(REF) $(B)

package-linux: all
	./scripts/package-linux.sh

test-package-linux: package-linux
	$(PYTHON) tests/package_linux.py

release-linux:
	./scripts/release-linux.sh

clean:
	rm -rf build bin dist
	rm -rf tests/__pycache__ tests/*/__pycache__

clean-asan:
	rm -rf $(ASAN_BUILD) $(ASAN_BIN)

clean-fil:
	rm -rf $(FIL_BUILD) $(FIL_BIN)

clean-static:
	rm -rf $(STATIC_BUILD) $(STATIC_BIN)

clean-el9:
	rm -rf $(EL9_BUILD) $(EL9_BIN)
