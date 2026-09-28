/* libFuzzer harness for the highlight helper's grammars and queries.
 *
 * The first input byte picks a bundled language and the rest is the source,
 * run through the same parse, query and run-building path as a request.
 * Built by `make fuzz` into bin/fuzz/fuzz_highlight; run it with a corpus
 * directory, `-max_total_time=60` and `-timeout=5`.
 */

/* NOTE: the helper's own main would clash with the one libFuzzer provides. */
#define main arqan_highlight_main
#include "arqan-highlight.c"
#undef main

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!size) return 0;
    Language *lang = &languages[data[0] % YHL_LANG_COUNT];
    size_t source_n = size - 1 < YHL_SOURCE_MAX ? size - 1 : YHL_SOURCE_MAX;
    memcpy(source, data + 1, source_n);
    TSQuery *query = language_query(lang);
    if (!query) __builtin_trap();
    uint32_t run_count = 0;
    uint8_t status = make_runs(lang, query, (uint32_t)source_n, &run_count);
    if (status == YHL_STATUS_INTERNAL) __builtin_trap();
    if (status == YHL_STATUS_OK && run_count > YHL_RUN_MAX) __builtin_trap();
    return 0;
}
