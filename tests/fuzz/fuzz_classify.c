/* libFuzzer harness for the shell command classifier.
 *
 * Classifies every input with and without a sandbox to contain git, under
 * ASan and UBSan. A sandbox may only add commands to the read-only set, never
 * take one away. A command the classifier calls read-only must hold none of
 * what it promises to refuse: a substitution, an input redirect, a group, a
 * background job, or an output redirect other than the harmless ones. This
 * harness finds those with its own quote-aware scan, not the classifier's.
 * Built by `make fuzz` into bin/fuzz/fuzz_classify.
 */

#define _XOPEN_SOURCE   700
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "agent.h"

void telemetry_log(i32 level, Str msg) {
    (void)level;
    (void)msg;
}

#include "core.c"
#include "classify.c"

#include <stdint.h>

static const char *const k_fuzz_harmless[] = {
    "2>&1", "2>/dev/null", ">/dev/null", "1>/dev/null", "&>/dev/null",
};

static b8 fuzz_delimiter(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '|';
}

static b8 fuzz_harmless(const char *p, size_t n) {
    for (size_t i = 0; i < sizeof k_fuzz_harmless / sizeof *k_fuzz_harmless;
         i++)
        if (strlen(k_fuzz_harmless[i]) == n
            && !memcmp(p, k_fuzz_harmless[i], n))
            return true;
    return false;
}

static b8 fuzz_substitutes(const char *p, size_t n, size_t i) {
    return p[i] == '`' || (p[i] == '$' && i + 1 < n && p[i + 1] == '(');
}

static b8 fuzz_shell_syntax(const char *p, size_t n) {
    char quote = 0;
    size_t word = SIZE_MAX;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (quote == '\'') {
            if (c == '\'') quote = 0;
            continue;
        }
        if (quote == '"') {
            if (c == '\\')
                i++;
            else if (c == '"')
                quote = 0;
            else if (fuzz_substitutes(p, n, i))
                return true;
            continue;
        }
        if (fuzz_substitutes(p, n, i) || c == '<' || c == '(' || c == '{')
            return true;
        if (c == '&' && i + 1 < n && p[i + 1] == '&') {
            i++;
            word = SIZE_MAX;
            continue;
        }
        if (c == '>' || c == '&') {
            size_t from = word == SIZE_MAX ? i : word;
            size_t end = i;
            while (end < n && !fuzz_delimiter(p[end])) end++;
            if (!fuzz_harmless(p + from, end - from)) return true;
            i = end - 1;
            word = SIZE_MAX;
            continue;
        }
        if (fuzz_delimiter(c)) {
            word = SIZE_MAX;
            continue;
        }
        if (word == SIZE_MAX) word = i;
        if (c == '\\')
            i++;
        else if (c == '\'' || c == '"')
            quote = c;
    }
    return false;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > AGENT_MAX_COMMAND) return 0;
    Str cmd = {(const char *)data, size};
    ShellClass bare = shell_classify(cmd, false);
    ShellClass contained = shell_classify(cmd, true);
    if (bare != SHELL_WRITES && contained != bare) abort();
    if (!memchr(cmd.p, '\0', size) && contained != SHELL_WRITES
        && fuzz_shell_syntax(cmd.p, size))
        abort();
    return 0;
}
