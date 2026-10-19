/* libFuzzer harness for passage ranking.
 *
 * The first input byte sets the query length, then the query and the page
 * body follow. Checks that passages cover ordered, in-range lines and bytes,
 * and that the written passages and outline stay within their budgets.
 * Built by `make fuzz` into bin/fuzz/fuzz_passage; run it with a corpus
 * directory and `-max_total_time=60`.
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
#include "passage.c"

#include <stdint.h>
#include <stdlib.h>

static alignas(64) u8 g_fuzz_mem[1u << 24];
static PassageQuery g_fuzz_query;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    size_t qn = data[0] % 64u;
    if (qn > size - 2) qn = size - 2;
    size_t budget = (size_t)data[1] * 24u;
    Str query = {(const char *)data + 2, qn};
    Str body = {(const char *)data + 2 + qn, size - 2 - qn};

    Arena a;
    arena_init(&a, g_fuzz_mem, sizeof g_fuzz_mem);
    passage_query(query, &g_fuzz_query);
    if (g_fuzz_query.n > AGENT_PASSAGE_TERMS) abort();

    size_t lines = str_lines(body);
    size_t n = passage_split(body, NULL);
    Passage *ps = arena_new(&a, Passage, n ? n : 1);
    if (!ps) return 0;
    if (passage_split(body, ps) != n) abort();
    for (size_t i = 0; i < n; i++) {
        if (ps[i].first < 1 || ps[i].first > ps[i].last || ps[i].last > lines)
            abort();
        if (ps[i].start > ps[i].end || ps[i].end > body.n) abort();
        if (i && (ps[i].first <= ps[i - 1].last || ps[i].start < ps[i - 1].end))
            abort();
    }

    Buf out;
    buf_init(&out, &a, 256);
    PassageReport report;
    if (passage_write(body, &g_fuzz_query, budget, &a, &out, &report)) {
        if (out.n > budget || report.shown > report.matched) abort();
    }
    Buf outline;
    buf_init(&outline, &a, 256);
    passage_outline(body, budget, &outline);
    if (outline.n > budget) abort();
    return 0;
}
