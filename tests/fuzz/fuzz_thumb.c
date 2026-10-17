/* libFuzzer harness for the terminal thumbnails.
 *
 * The first two bytes pick the box to scale into, and the rest is taken as a
 * PNG. A thumbnail that comes back must decode again at the size it claims.
 * Built by `make fuzz` into bin/fuzz/fuzz_thumb; seed the corpus with small
 * PNG files and run it with `-max_total_time=60`.
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
#include "thumb.c"

#include <stdint.h>
#include <stdlib.h>

static alignas(64) u8 g_fuzz_mem[1u << 26];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    Arena a;
    arena_init(&a, g_fuzz_mem, sizeof g_fuzz_mem);
    u32 max_w = (u32)data[0] + 1, max_h = (u32)data[1] + 1;
    Str png = {(const char *)data + 2, size - 2};
    Str out = {0};
    u32 w = 0, h = 0;
    if (thumb_png(&a, png, max_w, max_h, &out, &w, &h) != THUMB_SCALED)
        return 0;
    if (!w || !h || w > max_w || h > max_h) abort();
    AgentPng *dec = arena_alloc(&a, agent_png_size(), 16);
    u32 dw = 0, dh = 0;
    u64 work_n = 0;
    if (!dec || agent_png_begin(dec, out.p, out.n, &dw, &dh, &work_n) || dw != w
        || dh != h)
        abort();
    u8 *work = arena_alloc(&a, (size_t)work_n + 1, 16);
    u8 *px = arena_alloc_array(&a, (size_t)w * h, 4, 16);
    if (!work || !px
        || agent_png_finish(dec, work, (size_t)work_n, px, (size_t)w * h * 4))
        abort();
    return 0;
}
