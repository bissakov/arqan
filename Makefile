CC      ?= cc
# Everything the provider streams is untrusted input, so the cheap runtime
# checks stay on: they cost nothing measurable next to an SSE round trip.
CFLAGS  ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Wconversion \
           -fno-strict-aliasing -pipe -flto \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS ?= -flto
LIBS    ?= -lcurl

# A warning is a failure, so CI builds with EXTRA_CFLAGS=-Werror. It appends
# to whichever CFLAGS are in force and never reaches the vendored sources,
# which carry their own flags and are not ours to fix.
CFLAGS += $(EXTRA_CFLAGS)

# libcurl is opened at the first request rather than at exec, which keeps its
# dependency tree off the startup path. A static build has no dynamic loader,
# so CURL_MODE=link restores the ordinary link; that build is also where
# curl.h's type-checking macros still apply, since the table displaces them.
#
# The define stays out of CFLAGS: a variant recipe passes its own CFLAGS down,
# and a copy of this one folded into it would outlive the CURL_MODE the
# sub-make was given. Only the unity source needs it.
CURL_MODE ?= dlopen
CURL_CFLAGS :=
ifeq ($(CURL_MODE),dlopen)
CURL_CFLAGS := -DAGENT_CURL_DLOPEN=1
# Nothing links libcurl in this mode; dlopen lives in libc on glibc 2.34 and
# later, and -ldl remains an empty archive there for the older ones.
LIBS := -ldl
endif

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

# One relocatable executable: `make static`, from inside the musl builder image
# `scripts/build-musl.sh` provides. The link is static-pie, so the binary keeps
# ASLR and needs no interpreter, and it resolves its CA trust store at run time
# rather than inheriting the builder's.
STATIC_BUILD := build/musl
STATIC_BIN := bin/musl
# libcurl.a does not describe what it needs, and curl-config --static-libs
# names only libcurl's own dependencies: it omits their dependencies in turn,
# so libidn2 arrives without libunistring and libbrotlidec without
# libbrotlicommon. pkg-config walks Requires.private to a fixed point, so the
# list follows whatever the distribution's curl was built against instead of a
# hand-kept tail that the next curl rebuild invalidates at release time.
# Deferred, so no other target pays for the lookup.
STATIC_LIBS ?= $(shell pkg-config --static --libs libcurl)

# The rpm's pair: the shipped build, linked against the libcurl the rpm
# distributions ship rather than Debian's, whose versioned symbols no rpm host
# provides. Built from inside the EL9 image `scripts/build-el9.sh` provides.
EL9_BUILD := build/el9
EL9_BIN := bin/el9

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

# The suite as CI runs it. The waits are quiet-window based, so the number of
# workers is part of what is being tested: a workstation's default spread
# clears cases that starve on the runner's four shared cores, and the failure
# is then discovered a push later. Reproduce it here instead.
CI_JOBS ?= 4
test-ci: all $(TEST_BIN)
	ARQAN_TEST_BIN=$(TEST_BIN) ARQAN_TEST_JOBS=$(CI_JOBS) \
	    $(PYTHON) tests/run.py $(T)

# The instrumented tree is built and run entirely under build/asan and
# bin/asan, so bin/arqan stays the shipped binary, bin/arqan-test the one the
# suite drives, and `make test` keeps testing them.
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

# Same variant layout as asan and fil, under build/musl and bin/musl. Run this
# where a musl toolchain and the static archives are, which is what the builder
# image is for; on a glibc host the link fails for want of libcurl.a.
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

# The table displaces curl.h's type-checking macros, so a wrong setopt
# argument is caught in the linked build and nowhere else. Build it here, as
# every static release does for real.
check-curl-types:
	$(MAKE) bin/link/arqan CURL_MODE=link BUILDDIR=build/link BINDIR=bin/link

# Same variant layout again, under build/el9 and bin/el9. Nothing but the
# toolchain and the libraries differ from the shipped build, so this target
# only redirects the output; run it where an rpm distribution's libcurl is.
el9:
	$(MAKE) all $(EL9_BIN)/arqan-test \
	    BUILDDIR='$(EL9_BUILD)' BINDIR='$(EL9_BIN)'

test-el9: el9
	ARQAN_TEST_BIN=$(EL9_BIN)/arqan-test $(PYTHON) tests/run.py $(T)

mock:
	$(PYTHON) -m tests.mockprovider.server $(MOCK_ARGS)

# Benchmarks measure the shipped binary, never the instrumented one, and run
# one case at a time: a benchmark sharing the machine measures its neighbours.
bench: all
	$(PYTHON) -m bench.run $(B)

bench-slow: all
	$(PYTHON) -m bench.run --slow $(B)

# Iteration only. The instrumented binary announces every park on its input,
# so the harness stops waiting quiet windows out and a run costs seconds
# instead of minutes. Its figures describe the test build, which is why the
# guard never reads them.
bench-fast: all $(TEST_BIN)
	ARQAN_TEST_BIN=$(TEST_BIN) $(PYTHON) -m bench.run $(B)

# Record a baseline, then `make bench B="--baseline bench-baseline.json"`.
bench-baseline: all
	$(PYTHON) -m bench.run --slow --json bench-baseline.json $(B)

# The regression gate: build and measure REF, then this tree, on one machine.
# REF defaults to HEAD, so an uncommitted change is judged against the commit
# it sits on. CI passes the base of the branch instead.
REF ?= HEAD
bench-guard:
	./scripts/bench-guard.sh $(REF) $(B)

# The native packages take the glibc binaries from bin/; the portable archive
# takes the relocatable ones from bin/musl, which scripts/build-musl.sh
# produces. Packaging fails rather than shipping a dynamic archive.
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

# Drop only the static tree; the shipped build survives.
clean-static:
	rm -rf $(STATIC_BUILD) $(STATIC_BIN)

# Drop only the rpm tree; the shipped build survives.
clean-el9:
	rm -rf $(EL9_BUILD) $(EL9_BIN)
