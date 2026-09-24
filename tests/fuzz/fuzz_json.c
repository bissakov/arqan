/* libFuzzer harness for the JSON parser and serializer.
 *
 * Parses the input into a fixed arena, then writes what it parsed back out.
 * Built by `make fuzz` into bin/fuzz/fuzz_json; run it with a corpus directory and
 * `-max_total_time=60`.
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
#include "width.c"
#include "json.c"

#include <stdint.h>

static alignas(64) u8 g_fuzz_mem[1u << 24];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    Arena a;
    arena_init(&a, g_fuzz_mem, sizeof g_fuzz_mem);
    JVal *v = json_parse(&a, (Str){(const char *)data, size});
    if (!v) return 0;
    Buf b;
    buf_init(&b, &a, 4096);
    json_write(&b, v);
    return 0;
}
