/* libFuzzer harness for the settings file parser.
 *
 * Parses the input the way settings_load does, from a writable arena copy,
 * then reads every entry back. Built by `make fuzz` into bin/fuzz/fuzz_settings.
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
#include "paths.c"
#include "settings.c"

#include <stdint.h>

static alignas(64) u8 g_fuzz_mem[1u << 22];
static Settings g_fuzz_settings;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > AGENT_MAX_SETTINGS_BYTES) return 0;
    Arena a;
    arena_init(&a, g_fuzz_mem, sizeof g_fuzz_mem);
    Str src = str_dup(&a, (Str){(const char *)data, size});
    if (size && !src.p) return 0;
    Settings *s = &g_fuzz_settings;
    settings_parse(s, src);
    Str sections[16];
    settings_sections(s, STR(""), sections, 16);
    for (size_t i = 0; i < s->n; i++)
        (void)settings_get(s, s->section[i], s->key[i]);
    return 0;
}
