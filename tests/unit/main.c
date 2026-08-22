/* Unit tests for the modules that carry no terminal or network dependency.
 *
 * The end-to-end suite drives the built binary, so it cannot exhaust an arena
 * or force a short allocation on purpose. Those paths are the ones AGENTS.md
 * calls normal, and they are what this binary exists to cover. Keep cases here
 * to modules that compile standalone; anything needing a terminal belongs in
 * tests/cases instead.
 *
 * Built by `make test-unit` into bin/arqan-unit. No release target reads it.
 */

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "agent.h"

/* core.c reports through telemetry, which the unity build supplies from
 * telemetry.c. Stub it rather than pull that module and its dependencies in:
 * these cases assert on return values, not on what was reported. */
void telemetry_log(i32 level, Str msg) { (void)level; (void)msg; }

#include "core.c"
#include "width.c"
#include "json.c"

static int g_fail;
static int g_ran;
static const char *g_case;

#define CHECK(cond) do {                                                      \
    if (!(cond)) {                                                            \
        printf("  %s:%d: %s\n", g_case, __LINE__, #cond);                     \
        g_fail++;                                                             \
    }                                                                         \
} while (0)

#define RUN(fn) do { g_case = #fn; g_ran++; fn(); } while (0)

/* Arenas are the allocator, so a case gets its own rather than sharing one.
 * alignas mirrors what main.c does for the real arenas: arena_alloc aligns an
 * offset within the arena, so an absolute alignment holds only when the base
 * is at least as aligned. */
#define WITH_ARENA(name, bytes)                                               \
    static alignas(64) u8 name##_mem[bytes];                                  \
    Arena name;                                                               \
    arena_init(&name, name##_mem, sizeof name##_mem)

/* ---- arenas ------------------------------------------------------------ */

static void arena_reports_exhaustion(void) {
    WITH_ARENA(a, 128);
    CHECK(arena_alloc(&a, 64, 1) != NULL);
    size_t used = arena_used(&a);
    CHECK(arena_alloc(&a, 1024, 1) == NULL);
    /* A refused request must not consume the arena it could not satisfy. */
    CHECK(arena_used(&a) == used);
    CHECK(arena_alloc(&a, 8, 1) != NULL);
}

static void arena_honours_alignment(void) {
    WITH_ARENA(a, 512);
    CHECK(arena_alloc(&a, 1, 1) != NULL);
    void *p = arena_alloc(&a, 8, 64);
    CHECK(p != NULL);
    CHECK(((uintptr_t)p % 64) == 0);
    /* Rejected rather than rounded, so a caller cannot get less than it asked. */
    CHECK(arena_alloc(&a, 8, 0) == NULL);
    CHECK(arena_alloc(&a, 8, 3) == NULL);
}

static void arena_array_refuses_overflow(void) {
    WITH_ARENA(a, 4096);
    /* count * size wraps to a small satisfiable request if unchecked. */
    size_t huge = SIZE_MAX / 2 + 2;
    CHECK(arena_alloc_array(&a, huge, 4, 1) == NULL);
    CHECK(arena_alloc_array(&a, 4, huge, 1) == NULL);
    CHECK(arena_used(&a) == 0);
    CHECK(arena_alloc_array(&a, 4, 8, 1) != NULL);
}

static void arena_reset_reclaims(void) {
    WITH_ARENA(a, 256);
    CHECK(arena_alloc(&a, 200, 1) != NULL);
    CHECK(arena_used(&a) >= 200);
    arena_reset(&a);
    CHECK(arena_used(&a) == 0);
    CHECK(arena_alloc(&a, 200, 1) != NULL);
}

static void arena_mark_restores(void) {
    WITH_ARENA(a, 512);
    CHECK(arena_alloc(&a, 32, 1) != NULL);
    size_t mark = a.off;
    CHECK(arena_alloc(&a, 128, 1) != NULL);
    a.off = mark;
    CHECK(arena_used(&a) == mark);
}

/* ---- buffers ----------------------------------------------------------- */

static void buf_latches_on_exhaustion(void) {
    WITH_ARENA(a, 512);
    Buf b;
    buf_init(&b, &a, 16);
    buf_puts(&b, STR("short"));
    CHECK(buf_ok(&b));
    for (int i = 0; i < 1000; i++) buf_puts(&b, STR("padding padding padding"));
    CHECK(!buf_ok(&b));
    /* The latch is the contract: once a write fails, buf_ok stays false and
     * the content is not to be read. A later short write can still land in
     * spare capacity, so b.n is not stable and is not asserted here. */
    buf_puts(&b, STR("more"));
    CHECK(!buf_ok(&b));
    buf_putc(&b, 'x');
    CHECK(!buf_ok(&b));
}

static void buf_reserve_reports_failure(void) {
    WITH_ARENA(a, 128);
    Buf b;
    buf_init(&b, &a, 8);
    CHECK(!buf_reserve(&b, SIZE_MAX / 2));
    CHECK(!buf_ok(&b));
}

static void buf_json_str_escapes(void) {
    WITH_ARENA(a, 4096);
    Buf b;
    buf_init(&b, &a, 64);
    buf_json_str(&b, STR("a\"b\\c\nd\te"));
    CHECK(buf_ok(&b));
    CHECK(str_eq((Str){ b.p, b.n }, STR("\"a\\\"b\\\\c\\nd\\te\"")));
}

static void buf_json_str_escapes_control(void) {
    WITH_ARENA(a, 4096);
    Buf b;
    buf_init(&b, &a, 64);
    buf_json_str(&b, (Str){ "\x01", 1 });
    CHECK(buf_ok(&b));
    CHECK(str_eq((Str){ b.p, b.n }, STR("\"\\u0001\"")));
}

/* ---- json -------------------------------------------------------------- */

static void json_rejects_malformed(void) {
    WITH_ARENA(a, 8192);
    static const char *const bad[] = {
        "", "{", "}", "[", "[,]", "{\"a\"}", "{\"a\":}", "{a:1}",
        "\"unterminated", "tru",
        /* A number the scanner accepts but strtod would not consume whole.
         * Each of these silently parsed as a wrong value before. */
        "--1", "1e", "1-2-3", "1.2.3", "1e+", "{\"v\":--1}", "[1.2.3]",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        arena_reset(&a);
        Str s = (Str){ bad[i], strlen(bad[i]) };
        if (json_parse(&a, s) != NULL) {
            printf("  %s:%d: parsed malformed input: %s\n",
                   g_case, __LINE__, bad[i]);
            g_fail++;
        }
    }
}

static void json_accepts_wellformed(void) {
    WITH_ARENA(a, 8192);
    static const char *const good[] = {
        "{}", "[]", "null", "true", "false", "0", "-1.5e3", "\"\"",
        "{\"a\":[1,2,{\"b\":null}]}", " \t\r\n{} \t\r\n",
        "1e5", "-0", "0.0", "1E+5", "1e-5", "123456789012345678901234567890",
    };
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) {
        arena_reset(&a);
        Str s = (Str){ good[i], strlen(good[i]) };
        if (json_parse(&a, s) == NULL) {
            printf("  %s:%d: rejected valid input: %s\n",
                   g_case, __LINE__, good[i]);
            g_fail++;
        }
    }
}

/* Trailing commas are not valid JSON, but the parser accepts them and no
 * provider depends on their rejection. Pinned so a later change to the parser
 * is a decision rather than an accident. */
static void json_tolerates_trailing_commas(void) {
    WITH_ARENA(a, 8192);
    CHECK(json_parse(&a, STR("[1,]")) != NULL);
    CHECK(json_parse(&a, STR("{\"a\":1,}")) != NULL);
}

static void json_survives_a_short_arena(void) {
    /* Walk the arena from far too small to comfortably large. Every size must
     * either parse completely or return NULL; none may crash or hand back a
     * partial tree. */
    static u8 mem[4096];
    Str doc = STR("{\"a\":[1,2,3],\"b\":{\"c\":\"dddddddddddddddddddd\"}}");
    for (size_t cap = 8; cap <= sizeof mem; cap += 8) {
        Arena a;
        arena_init(&a, mem, cap);
        JVal *v = json_parse(&a, doc);
        if (!v) continue;
        CHECK(v->type == J_OBJ);
        CHECK(str_eq(json_str(json_get(v, STR("b")), STR("c")),
                     STR("dddddddddddddddddddd")));
    }
}

static void json_reads_missing_members_as_absent(void) {
    WITH_ARENA(a, 4096);
    JVal *v = json_parse(&a, STR("{\"s\":\"x\",\"n\":1,\"b\":true}"));
    CHECK(v != NULL);
    CHECK(json_str(v, STR("nope")).n == 0);
    /* A member of the wrong type is the same answer as an absent one. */
    CHECK(json_str(v, STR("n")).n == 0);
    CHECK(json_get(v, STR("nope")) == NULL);
    CHECK(json_bool(v, STR("b")));
    CHECK(!json_bool(v, STR("nope")));
    CHECK(json_str(NULL, STR("s")).n == 0);
    CHECK(json_get(NULL, STR("s")) == NULL);
}

static void json_decodes_escapes(void) {
    WITH_ARENA(a, 4096);
    JVal *v = json_parse(&a, STR("{\"k\":\"a\\\"b\\\\c\\nd\\u00e9\"}"));
    CHECK(v != NULL);
    CHECK(str_eq(json_str(v, STR("k")), STR("a\"b\\c\nd\xc3\xa9")));
}

static void json_decodes_surrogate_pairs(void) {
    WITH_ARENA(a, 4096);
    JVal *v = json_parse(&a, STR("{\"k\":\"\\ud83d\\ude00\"}"));
    CHECK(v != NULL);
    CHECK(str_eq(json_str(v, STR("k")), STR("\xf0\x9f\x98\x80")));
}

static void json_bounds_nesting(void) {
    /* Deep nesting must be refused rather than recursed into the C stack. */
    WITH_ARENA(a, 1 << 20);
    Buf b;
    buf_init(&b, &a, 4096);
    for (int i = 0; i < 4096; i++) buf_putc(&b, '[');
    for (int i = 0; i < 4096; i++) buf_putc(&b, ']');
    CHECK(buf_ok(&b));
    CHECK(json_parse(&a, (Str){ b.p, b.n }) == NULL);
}

static void json_round_trips(void) {
    WITH_ARENA(a, 1 << 16);
    JVal *v = json_parse(&a, STR("{\"a\":[1,true,null,\"x\"],\"b\":{\"c\":-2.5}}"));
    CHECK(v != NULL);
    Buf b;
    buf_init(&b, &a, 256);
    json_write(&b, v);
    CHECK(buf_ok(&b));
    JVal *again = json_parse(&a, (Str){ b.p, b.n });
    CHECK(again != NULL);
    const JVal *arr = json_get(again, STR("a"));
    CHECK(arr && arr->type == J_ARR);
    CHECK(json_at(arr, 3) != NULL);
    CHECK(json_at(arr, 99) == NULL);
    CHECK(json_at(NULL, 0) == NULL);
}

static void json_error_names_a_position(void) {
    WITH_ARENA(a, 4096);
    char err[128] = {0};
    CHECK(json_parse_error(&a, STR("{\"a\": }"), err, sizeof err) == NULL);
    CHECK(err[0] != '\0');
    char ok[128] = "untouched";
    CHECK(json_parse_error(&a, STR("{}"), ok, sizeof ok) != NULL);
    CHECK(strcmp(ok, "untouched") == 0);
}

/* ---- strings ----------------------------------------------------------- */

static void str_handles_empty(void) {
    Str e = (Str){0};
    CHECK(str_eq(e, e));
    CHECK(!str_eq(e, STR("x")));
    CHECK(str_trim(e).n == 0);
    CHECK(str_trim(STR("   \t\r\n  ")).n == 0);
    CHECK(str_eq(str_trim(STR("  ab  ")), STR("ab")));
    CHECK(str_drop(STR("abc"), 99).n == 0);
    CHECK(str_eq(str_drop(STR("abc"), 1), STR("bc")));
}

/* ---- display width ----------------------------------------------------- */

static void width_classifies_glyphs(void) {
    CHECK(agent_width('a') == 1);
    CHECK(agent_width(0x4E00) == 2);      /* CJK */
    CHECK(agent_width(0x1F600) == 2);     /* emoji */
    CHECK(agent_width(0x0301) == 0);      /* combining acute */
}

int main(void) {
    /* These cases exhaust arenas on purpose, and each refusal logs. Raise the
     * level past ERROR so a passing run says nothing but its result. */
    agent_log_set_level(AGENT_LOG_ERROR + 1);

    RUN(arena_reports_exhaustion);
    RUN(arena_honours_alignment);
    RUN(arena_array_refuses_overflow);
    RUN(arena_reset_reclaims);
    RUN(arena_mark_restores);

    RUN(buf_latches_on_exhaustion);
    RUN(buf_reserve_reports_failure);
    RUN(buf_json_str_escapes);
    RUN(buf_json_str_escapes_control);

    RUN(json_rejects_malformed);
    RUN(json_accepts_wellformed);
    RUN(json_tolerates_trailing_commas);
    RUN(json_survives_a_short_arena);
    RUN(json_reads_missing_members_as_absent);
    RUN(json_decodes_escapes);
    RUN(json_decodes_surrogate_pairs);
    RUN(json_bounds_nesting);
    RUN(json_round_trips);
    RUN(json_error_names_a_position);

    RUN(str_handles_empty);
    RUN(width_classifies_glyphs);

    if (g_fail) {
        printf("%d failure(s) in %d cases\n", g_fail, g_ran);
        return 1;
    }
    printf("%d/%d unit cases passed\n", g_ran, g_ran);
    return 0;
}
