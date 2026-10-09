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

#define _XOPEN_SOURCE   700
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "agent.h"

void telemetry_log(i32 level, Str msg) {
    (void)level;
    (void)msg;
}

void tel_int(TelEvent *e, const char *key, i64 v) {
    (void)e;
    (void)key;
    (void)v;
}

void tel_open(TelEvent *e, const char *ev) {
    (void)e;
    (void)ev;
}

void tel_bool(TelEvent *e, const char *key, b8 v) {
    (void)e;
    (void)key;
    (void)v;
}

void tel_str(TelEvent *e, const char *key, Str v) {
    (void)e;
    (void)key;
    (void)v;
}

void tel_shape(TelEvent *e, const char *key, Str text) {
    (void)e;
    (void)key;
    (void)text;
}

void tel_send(TelEvent *e) {
    (void)e;
}

void tools_write_schemas(Buf *b, const ToolRegistry *r, ApiKind api,
                         ToolAudience audience) {
    (void)b;
    (void)r;
    (void)api;
    (void)audience;
}

i32 http_post(const HttpReq *r) {
    (void)r;
    return -1;
}

i32 http_get(const char *base_url, const char *path, const char *api_key,
             ApiKind api, Buf *out, char *fail_out, size_t fail_cap) {
    (void)base_url;
    (void)path;
    (void)api_key;
    (void)api;
    (void)out;
    (void)fail_out;
    (void)fail_cap;
    return -1;
}

#include "core.c"
#include "width.c"
#include "json.c"
#include "tasklog.c"
#include "spill.c"
#include "media.c"
#include "paths.c"
#include "settings.c"
#include "theme.c"
#include "progress.c"
#include "provider.c"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail;
static int g_ran;
static const char *g_case;

#define CHECK(cond)                                           \
    do {                                                      \
        if (!(cond)) {                                        \
            printf("  %s:%d: %s\n", g_case, __LINE__, #cond); \
            g_fail++;                                         \
        }                                                     \
    } while (0)

#define RUN(fn)       \
    do {              \
        g_case = #fn; \
        g_ran++;      \
        fn();         \
    } while (0)

#define WITH_ARENA(name, bytes)              \
    static alignas(64) u8 name##_mem[bytes]; \
    Arena name;                              \
    arena_init(&name, name##_mem, sizeof name##_mem)

/* ---- arenas ------------------------------------------------------------ */

static void arena_reports_exhaustion(void) {
    WITH_ARENA(a, 128);
    CHECK(arena_alloc(&a, 64, 1) != NULL);
    size_t used = arena_used(&a);
    CHECK(arena_alloc(&a, 1024, 1) == NULL);
    CHECK(arena_used(&a) == used);
    CHECK(arena_alloc(&a, 8, 1) != NULL);
}

static void arena_honours_alignment(void) {
    WITH_ARENA(a, 512);
    CHECK(arena_alloc(&a, 1, 1) != NULL);
    void *p = arena_alloc(&a, 8, 64);
    CHECK(p != NULL);
    CHECK(((uintptr_t)p % 64) == 0);
    CHECK(arena_alloc(&a, 8, 0) == NULL);
    CHECK(arena_alloc(&a, 8, 3) == NULL);
}

static void arena_array_refuses_overflow(void) {
    WITH_ARENA(a, 4096);
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
    CHECK(str_eq((Str){b.p, b.n}, STR("\"a\\\"b\\\\c\\nd\\te\"")));
}

static void buf_json_str_escapes_control(void) {
    WITH_ARENA(a, 4096);
    Buf b;
    buf_init(&b, &a, 64);
    buf_json_str(&b, (Str){"\x01", 1});
    CHECK(buf_ok(&b));
    CHECK(str_eq((Str){b.p, b.n}, STR("\"\\u0001\"")));
}

static void base64_round_trips(void) {
    WITH_ARENA(a, 8192);
    u8 raw[256];
    for (size_t i = 0; i < sizeof raw; i++) raw[i] = (u8)i;
    for (size_t n = 0; n <= sizeof raw; n += n < 8 ? 1 : 37) {
        size_t mark = a.off;
        Buf b;
        buf_init(&b, &a, 16);
        buf_base64(&b, raw, n);
        Str text = buf_finish(&b);
        Str back = {0};
        CHECK(base64_decode(&a, text, sizeof raw, &back) == B64_OK);
        CHECK(back.n == n);
        CHECK(n == 0 || memcmp(back.p, raw, n) == 0);
        a.off = mark;
    }
}

static void base64_accepts_missing_padding(void) {
    WITH_ARENA(a, 1024);
    Str out = {0};
    CHECK(base64_decode(&a, STR("aGk"), 16, &out) == B64_OK);
    CHECK(str_eq(out, STR("hi")));
    CHECK(base64_decode(&a, STR("aA"), 16, &out) == B64_OK);
    CHECK(str_eq(out, STR("h")));
}

static void base64_rejects_malformed(void) {
    WITH_ARENA(a, 1024);
    static const char *const bad[] = {
        "a",    "aGk=a",  "aG=k", "aGk==", "a===", "aGk!",
        "aG k", "aGk=\n", "=",    "====",  "aGl-", "aGl_",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Str out = {0};
        CHECK(base64_decode(&a, str_c(bad[i]), 64, &out) == B64_MALFORMED);
    }
}

static void base64_refuses_before_allocating(void) {
    WITH_ARENA(a, 64);
    Str out = {0};
    size_t used = arena_used(&a);
    CHECK(base64_decode(&a, STR("aGVsbG8gd29ybGQ="), 10, &out)
          == B64_TOO_LARGE);
    CHECK(arena_used(&a) == used);
    CHECK(base64_decode(&a, STR("aGVsbG8gd29ybGQ="), 11, &out) == B64_OK);
    CHECK(str_eq(out, STR("hello world")));

    WITH_ARENA(tiny, 8);
    CHECK(base64_decode(&tiny, STR("aGVsbG8gd29ybGQ="), 64, &out)
          == B64_NO_MEMORY);
}

/* ---- json -------------------------------------------------------------- */

static void json_rejects_malformed(void) {
    WITH_ARENA(a, 8192);
    static const char *const bad[] = {
        "",
        "{",
        "}",
        "[",
        "[,]",
        "{\"a\"}",
        "{\"a\":}",
        "{a:1}",
        "\"unterminated",
        "tru",
        "--1",
        "1e",
        "1-2-3",
        "1.2.3",
        "1e+",
        "{\"v\":--1}",
        "[1.2.3]",
        "[1",
        "{\"a\":1",
        "{\"a\":[1}",
        "[{\"a\":1 x]",
        "{\"a\":{\"b\":1}",
        "[[1] 2]",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        arena_reset(&a);
        Str s = (Str){bad[i], strlen(bad[i])};
        if (json_parse(&a, s) != NULL) {
            printf("  %s:%d: parsed malformed input: %s\n", g_case, __LINE__,
                   bad[i]);
            g_fail++;
        }
    }
}

static void json_accepts_wellformed(void) {
    WITH_ARENA(a, 8192);
    static const char *const good[] = {
        "{}",
        "[]",
        "null",
        "true",
        "false",
        "0",
        "-1.5e3",
        "\"\"",
        "{\"a\":[1,2,{\"b\":null}]}",
        " \t\r\n{} \t\r\n",
        "1e5",
        "-0",
        "0.0",
        "1E+5",
        "1e-5",
        "123456789012345678901234567890",
    };
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) {
        arena_reset(&a);
        Str s = (Str){good[i], strlen(good[i])};
        if (json_parse(&a, s) == NULL) {
            printf("  %s:%d: rejected valid input: %s\n", g_case, __LINE__,
                   good[i]);
            g_fail++;
        }
    }
}

static void json_tolerates_trailing_commas(void) {
    WITH_ARENA(a, 8192);
    CHECK(json_parse(&a, STR("[1,]")) != NULL);
    CHECK(json_parse(&a, STR("{\"a\":1,}")) != NULL);
}

static void json_strict_refuses_trailing_commas(void) {
    WITH_ARENA(a, 8192);
    CHECK(json_parse_strict(&a, STR("[1,]")) == NULL);
    CHECK(json_parse_strict(&a, STR("{\"a\":1,}")) == NULL);
    CHECK(json_parse_strict(&a, STR("{\"a\":[1,2,],\"b\":1}")) == NULL);
    CHECK(json_parse_strict(&a, STR("{\"a\":[1,2],\"b\":{}}")) != NULL);
}

static void json_survives_a_short_arena(void) {
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
    WITH_ARENA(a, 1 << 20);
    Buf b;
    buf_init(&b, &a, 4096);
    for (int i = 0; i < 4096; i++) buf_putc(&b, '[');
    for (int i = 0; i < 4096; i++) buf_putc(&b, ']');
    CHECK(buf_ok(&b));
    CHECK(json_parse(&a, (Str){b.p, b.n}) == NULL);
}

static void json_round_trips(void) {
    WITH_ARENA(a, 1 << 16);
    JVal *v =
        json_parse(&a, STR("{\"a\":[1,true,null,\"x\"],\"b\":{\"c\":-2.5}}"));
    CHECK(v != NULL);
    Buf b;
    buf_init(&b, &a, 256);
    json_write(&b, v);
    CHECK(buf_ok(&b));
    JVal *again = json_parse(&a, (Str){b.p, b.n});
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

static void str_compares_ascii_without_case(void) {
    CHECK(str_eq_ci(STR("Content-Type"), STR("content-type")));
    CHECK(str_eq_ci(STR("MCP"), STR("mcp")));
    CHECK(!str_eq_ci(STR("mcp"), STR("mcpx")));
    CHECK(!str_eq_ci(STR("mcp"), STR("map")));
}

/* ---- display width ----------------------------------------------------- */

static void width_classifies_glyphs(void) {
    CHECK(agent_width('a') == 1);
    CHECK(agent_width(0x4E00) == 2);
    CHECK(agent_width(0x1F600) == 2);
    CHECK(agent_width(0x0301) == 0);
}

/* ---- the task worker log ----------------------------------------------- */

typedef struct {
    i32 w, r;
    char path[64];
} LogPair;

static b8 log_pair(LogPair *p) {
    snprintf(p->path, sizeof p->path, "/tmp/arqan-unit-log-%d", (i32)getpid());
    p->w = open(p->path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    p->r = open(p->path, O_RDONLY);
    unlink(p->path);
    return p->w >= 0 && p->r >= 0;
}

static void log_pair_close(LogPair *p) {
    if (p->w >= 0) close(p->w);
    if (p->r >= 0) close(p->r);
}

static struct {
    size_t n;
    TaskEventKind kind[16];
    char text[16][64];
    u32 slot[16];
} g_seen;

static void seen(const TaskEvent *e, void *ud) {
    (void)ud;
    if (g_seen.n >= 16) return;
    g_seen.kind[g_seen.n] = e->kind;
    g_seen.slot[g_seen.n] = e->slot;
    Str t = str_clip_utf8(e->text, sizeof g_seen.text[0] - 1);
    if (t.n) memcpy(g_seen.text[g_seen.n], t.p, t.n);
    g_seen.text[g_seen.n][t.n] = '\0';
    g_seen.n++;
}

static void raw_line(i32 fd, const char *s) {
    ssize_t rc = write(fd, s, strlen(s));
    (void)rc;
}

static void tasklog_round_trips(void) {
    WITH_ARENA(a, 1 << 20);
    LogPair p;
    if (!log_pair(&p)) {
        CHECK(false);
        return;
    }
    TaskLog l;
    tasklog_init(&l, p.w);
    TaskEvent call = {.kind = TASK_EV_CALL,
                      .slot = 3,
                      .name = STR("grep"),
                      .args = STR("{\"pattern\":\"a\\\"b\"}")};
    tasklog_write(&l, &call, &a);
    TaskEvent end = {.kind = TASK_EV_END,
                     .outcome = STR("reported"),
                     .rounds = 2,
                     .text = STR("line one\nline two")};
    tasklog_write(&l, &end, &a);

    TaskReader r;
    tasklog_reader_init(&r, p.r);
    memset(&g_seen, 0, sizeof g_seen);
    CHECK(tasklog_read(&r, &a, seen, NULL) == 2);
    CHECK(g_seen.n == 2);
    CHECK(g_seen.kind[0] == TASK_EV_CALL);
    CHECK(g_seen.slot[0] == 3);
    CHECK(g_seen.kind[1] == TASK_EV_END);
    CHECK(strcmp(g_seen.text[1], "line one\nline two") == 0);
    log_pair_close(&p);
}

static void tasklog_keeps_a_partial_tail(void) {
    WITH_ARENA(a, 1 << 20);
    LogPair p;
    if (!log_pair(&p)) {
        CHECK(false);
        return;
    }
    TaskReader r;
    tasklog_reader_init(&r, p.r);
    memset(&g_seen, 0, sizeof g_seen);

    raw_line(p.w, "{\"e\":\"msg\",\"text\":\"half");
    CHECK(tasklog_read(&r, &a, seen, NULL) == 0);
    CHECK(g_seen.n == 0);

    raw_line(p.w, " and half\"}\n");
    CHECK(tasklog_read(&r, &a, seen, NULL) == 1);
    CHECK(g_seen.n == 1);
    CHECK(strcmp(g_seen.text[0], "half and half") == 0);
    log_pair_close(&p);
}

static void tasklog_skips_what_it_cannot_read(void) {
    WITH_ARENA(a, 1 << 20);
    LogPair p;
    if (!log_pair(&p)) {
        CHECK(false);
        return;
    }
    raw_line(p.w, "not json at all\n");
    raw_line(p.w, "{\"e\":\"nonsense\"}\n");
    raw_line(p.w, "{\"e\":\"msg\",\"text\":\"kept\"}\n");

    TaskReader r;
    tasklog_reader_init(&r, p.r);
    memset(&g_seen, 0, sizeof g_seen);
    CHECK(tasklog_read(&r, &a, seen, NULL) == 1);
    CHECK(g_seen.n == 1);
    CHECK(strcmp(g_seen.text[0], "kept") == 0);
    log_pair_close(&p);
}

static void tasklog_resynchronizes_after_a_huge_line(void) {
    WITH_ARENA(a, 1 << 20);
    LogPair p;
    if (!log_pair(&p)) {
        CHECK(false);
        return;
    }
    size_t huge = AGENT_TASK_LINE_MAX + 4096;
    char *fat = malloc(huge + 2);
    CHECK(fat != NULL);
    if (!fat) return;
    memset(fat, 'x', huge);
    fat[huge] = '\n';
    fat[huge + 1] = '\0';
    raw_line(p.w, fat);
    free(fat);
    raw_line(p.w, "{\"e\":\"msg\",\"text\":\"after\"}\n");

    TaskReader r;
    tasklog_reader_init(&r, p.r);
    memset(&g_seen, 0, sizeof g_seen);
    CHECK(tasklog_read(&r, &a, seen, NULL) == 1);
    CHECK(g_seen.n == 1);
    CHECK(strcmp(g_seen.text[0], "after") == 0);
    log_pair_close(&p);
}

static void tasklog_writes_the_end_past_the_cap(void) {
    WITH_ARENA(a, 1 << 20);
    LogPair p;
    if (!log_pair(&p)) {
        CHECK(false);
        return;
    }
    TaskLog l;
    tasklog_init(&l, p.w);
    l.written = AGENT_TASK_LOG_BYTES;
    l.full = true;

    TaskEvent msg = {.kind = TASK_EV_MSG, .text = STR("dropped")};
    tasklog_write(&l, &msg, &a);
    TaskEvent end = {
        .kind = TASK_EV_END, .outcome = STR("reported"), .text = STR("kept")};
    tasklog_write(&l, &end, &a);

    TaskReader r;
    tasklog_reader_init(&r, p.r);
    memset(&g_seen, 0, sizeof g_seen);
    CHECK(tasklog_read(&r, &a, seen, NULL) == 1);
    CHECK(g_seen.n == 1);
    CHECK(g_seen.kind[0] == TASK_EV_END);
    CHECK(strcmp(g_seen.text[0], "kept") == 0);
    log_pair_close(&p);
}

static void media_refuses_a_short_label_allocation(void) {
    WITH_ARENA(storage, 4096);
    WITH_ARENA(payload, 17);
    MediaSet m;
    CHECK(media_init(&m, &storage, 1));
    char err[128] = "";
    Str image = STR("GIF89a\x01\0\x01\0\0\0\0\0\0\0");
    CHECK(media_add(&m, &payload, image, STR("picture"), err, sizeof err)
          == MEDIA_NONE);
    CHECK(m.n == 0);
    CHECK(err[0] != '\0');
}

static void media_refuses_full_capacity(void) {
    WITH_ARENA(a, 4096);
    MediaSet m;
    CHECK(media_init(&m, &a, 1));
    char err[128] = "";
    Str image = STR("GIF89a\x01\0\x01\0\0\0\0\0\0\0");
    CHECK(media_add(&m, &a, image, STR("first"), err, sizeof err) == 0);
    CHECK(media_add(&m, &a, image, STR("second"), err, sizeof err)
          == MEDIA_NONE);
    CHECK(m.n == 1);
    CHECK(str_eq(m.bytes[0], image));
}

/* ---- sha-256 ------------------------------------------------------------ */

static b8 sha256_is(const void *p, size_t n, const char *want) {
    u8 digest[32];
    char hex[65];
    sha256(p, n, digest);
    hex_encode(digest, sizeof digest, hex);
    return strcmp(hex, want) == 0;
}

static void sha256_matches_known_answers(void) {
    CHECK(sha256_is("", 0,
                    "e3b0c44298fc1c149afbf4c8996fb924"
                    "27ae41e4649b934ca495991b7852b855"));
    CHECK(sha256_is("abc", 3,
                    "ba7816bf8f01cfea414140de5dae2223"
                    "b00361a396177a9cb410ff61f20015ad"));
    static const char nist[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(sha256_is(nist, sizeof nist - 1,
                    "248d6a61d20638b8e5c026930c3e6039"
                    "a33ce45964ff2167f6ecedd419db06c1"));
    static char million[1000000];
    memset(million, 'a', sizeof million);
    CHECK(sha256_is(million, sizeof million,
                    "cdc76e5c9914fb9281a1c7e284d73e67"
                    "f1809a48a497200e046d39ccc7112cd0"));
}

static void sha256_pads_at_the_block_edges(void) {
    static const char text[] =
        "The quick brown fox jumps over the lazy dog. The quick brown fox "
        "jumps over the lazy dog. The quick brown fox jumps over.";
    static const struct {
        size_t n;
        const char *hex;
    } cases[] = {
        {55,
         "24f97e70d9742a384ecd9abb0a543b15eba57b06aa5084991a5d6705a32bfe1f"},
        {56,
         "f1629a1264c01780c6a928c503a7b440059992800034238d1ce1fc439f7f2038"},
        {63,
         "f35d185537ff4332e1c413bd5875ae84554231d2205332bf8076c3ffdea82e0e"},
        {64,
         "3e65a688760ada5cffafb936ef148f2399478da10c177369b4ff6931e0df2881"},
        {65,
         "dea97e0b2552edb41fe9bbb4fef494b7374306bbcb9be507fbff71ae3e621058"},
        {119,
         "693ec834a9f17110c83394a10636ca8fddcb77b6fc3c570c36622153d01cec0c"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
        CHECK(sha256_is(text, cases[i].n, cases[i].hex));
}

/* ---- theme colours ----------------------------------------------------- */

static void theme_colour_parse_accepts_the_value_forms(void) {
    ThemeColour c;
    CHECK(theme_colour_parse(STR("0"), &c) && c.kind == THEME_COLOUR_INDEX
          && c.index == 0);
    CHECK(theme_colour_parse(STR("255"), &c) && c.kind == THEME_COLOUR_INDEX
          && c.index == 255);
    CHECK(theme_colour_parse(STR("#000000"), &c) && c.kind == THEME_COLOUR_RGB
          && c.r == 0 && c.g == 0 && c.b == 0);
    CHECK(theme_colour_parse(STR("#FFffFF"), &c) && c.kind == THEME_COLOUR_RGB
          && c.r == 255 && c.g == 255 && c.b == 255);
    CHECK(theme_colour_parse(STR("default"), &c)
          && c.kind == THEME_COLOUR_DEFAULT);
}

static void theme_colour_parse_rejects_everything_else(void) {
    static const char *const bad[] = {"256",     "-1",  "#12345", "#1234567",
                                      "#gg0000", "",    "1\0332", "\033[0m",
                                      "Default", "12 ", "0x10"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        ThemeColour c = {THEME_COLOUR_INDEX, 7, 1, 2, 3};
        CHECK(!theme_colour_parse(str_c(bad[i]), &c));
        CHECK(c.kind == THEME_COLOUR_INDEX && c.index == 7);
    }
}

/* NOTE: the unit build links no libm, so the WCAG gamma curve is computed
 * with Newton steps: x^2.4 is x^2 times the fifth root of x^2. */
static double unit_root5(double v) {
    if (v <= 0) return 0;
    double x = v < 1 ? 1 : v;
    for (int i = 0; i < 60; i++) {
        double x4 = x * x * x * x;
        x -= (x4 * x - v) / (5 * x4);
    }
    return x;
}

static double unit_channel(u8 c) {
    double v = c / 255.0;
    if (v <= 0.03928) return v / 12.92;
    double b = (v + 0.055) / 1.055;
    return b * b * unit_root5(b * b);
}

static double unit_contrast(ThemeColour a, ThemeColour b) {
    u8 x[3], y[3];
    if (!theme_rgb(a, x) || !theme_rgb(b, y)) return 0;
    double la = 0.2126 * unit_channel(x[0]) + 0.7152 * unit_channel(x[1])
                + 0.0722 * unit_channel(x[2]);
    double lb = 0.2126 * unit_channel(y[0]) + 0.7152 * unit_channel(y[1])
                + 0.0722 * unit_channel(y[2]);
    return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}

static void theme_light_text_reads_on_the_page(void) {
    static const size_t light_columns[] = {1, 4};
    CHECK(!strcmp(k_theme_names[1], "light"));
    CHECK(!strcmp(k_theme_names[4], "kanagawa-lotus"));
    for (size_t t = 0; t < 2; t++) {
        const ThemeColour *page = &k_theme_builtin[THEME_PAGE_BG][0];
        size_t col = light_columns[t];
        for (size_t s = 0; s < THEME_SLOT_N; s++) {
            if (k_theme_slots[s].bg) continue;
            ThemeColour fg = k_theme_builtin[s][col];
            ThemeColour bg = page[col];
            if (s == THEME_FIND_CURRENT_FG)
                bg = k_theme_builtin[THEME_FIND_CURRENT_BG][col];
            double ratio = unit_contrast(fg, bg);
            if (ratio < 5.0)
                printf("  %s %s: %.2f:1\n", k_theme_names[col],
                       k_theme_slots[s].name, ratio);
            CHECK(ratio >= 5.0);
        }
    }
}

static void theme_maps_hex_to_the_nearest_256_colour(void) {
    CHECK(theme_nearest_256(0x00, 0x00, 0x00) == 16);
    CHECK(theme_nearest_256(0xff, 0xff, 0xff) == 231);
    CHECK(theme_nearest_256(0x80, 0x80, 0x80) == 244);
    CHECK(theme_nearest_256(0xff, 0x00, 0x00) == 196);
    CHECK(theme_nearest_256(0x5f, 0x87, 0xaf) == 67);
}

/* ---- the run ledger ---------------------------------------------------- */

static b8 str_has(Str s, Str needle) {
    if (needle.n > s.n) return false;
    for (size_t i = 0; i + needle.n <= s.n; i++)
        if (!memcmp(s.p + i, needle.p, needle.n)) return true;
    return false;
}

static void progress_note_strip_drops_only_the_note(void) {
    Str noted = STR("hi\n[exit 1]\n\n[progress: 3 tool calls since the user's "
                    "last message, 3 failed. If an approach keeps failing, "
                    "stop and report.]");
    CHECK(str_eq(progress_note_strip(noted), STR("hi\n[exit 1]")));
    CHECK(
        str_eq(progress_note_strip(STR("hi\n[exit 1]")), STR("hi\n[exit 1]")));
    CHECK(str_eq(progress_note_strip(STR("list [a]")), STR("list [a]")));
    CHECK(str_eq(progress_note_strip(STR("]")), STR("]")));
    CHECK(str_eq(progress_note_strip((Str){0}), (Str){0}));
    Str unclosed = STR("x\n\n[progress: never closed");
    CHECK(str_eq(progress_note_strip(unclosed), unclosed));
}

static const char PRIOR_CHECKPOINT[] =
    "# Context checkpoint\n\n## Goal\nShip it\n\n"
    "## User requests\n\nthe model wrote this one\n\n"
    "## User requests\n\n"
    "2 earlier requests were left out.\n\n"
    "> a\n> b\n\n"
    "> c\n\n"
    "## Run\n\nCompaction 2 of this session.\n\n"
    "## Step list\n\n- [ ] one\n";

static void progress_parses_the_requests_back(void) {
    Str prior = STR(PRIOR_CHECKPOINT);
    size_t dropped = 0;
    Str quoted[4];
    size_t n = progress_requests_parse(prior, quoted, 4, &dropped);
    CHECK(n == 2);
    CHECK(dropped == 2);
    CHECK(n == 2 && str_eq(quoted[0], STR("> a\n> b")));
    CHECK(n == 2 && str_eq(quoted[1], STR("> c")));
    CHECK(progress_requests_parse(prior, NULL, 0, &dropped) == 2);
    CHECK(progress_requests_parse(STR("# Context checkpoint\n\nsummary"), NULL,
                                  0, &dropped)
          == 0);
    CHECK(dropped == 0);
}

static void progress_checkpoint_carries_and_quotes(void) {
    WITH_ARENA(a, 64 * 1024);
    Str fresh[] = {STR("d\n\ne"), STR("## Step list\n- [ ] bogus")};
    Buf b;
    buf_init(&b, &a, 256);
    CHECK(progress_checkpoint(&b, STR(PRIOR_CHECKPOINT), fresh, 2, &a));
    CHECK(buf_ok(&b));
    Str out = buf_finish(&b);
    CHECK(str_starts(out, STR("\n\n## User requests\n\n2 earlier requests were "
                              "left out.\n\n> a\n> b\n\n> c\n\n> d\n>\n> e\n\n"
                              "> ## Step list\n> - [ ] bogus\n\n## Run\n\n"
                              "Compaction 3 of this session.")));

    size_t dropped = 0;
    Str quoted[8];
    CHECK(progress_requests_parse(out, quoted, 8, &dropped) == 4);
    CHECK(dropped == 2);
}

static void progress_checkpoint_counts_the_first_compaction(void) {
    WITH_ARENA(a, 16 * 1024);
    Str fresh[] = {STR("only")};
    Buf b;
    buf_init(&b, &a, 64);
    CHECK(progress_checkpoint(&b, (Str){0}, fresh, 1, &a));
    Str out = buf_finish(&b);
    CHECK(str_has(out, STR("\n\n> only\n\n## Run\n\nCompaction 1 of this "
                           "session.")));
    CHECK(!str_has(out, STR("left out")));

    buf_init(&b, &a, 64);
    CHECK(progress_checkpoint(&b, STR("# Context checkpoint\n\nold summary"),
                              fresh, 1, &a));
    CHECK(str_has(buf_finish(&b), STR("Compaction 2 of this session.")));
}

static void progress_checkpoint_bounds_the_requests(void) {
    WITH_ARENA(a, 128 * 1024);
    static char big[3 * 3000], huge[9000];
    memset(big, 'x', sizeof big);
    memset(huge, 'y', sizeof huge);
    Str fresh[] = {
        {big, 3000},        {big + 3000, 3000}, {huge, sizeof huge},
        {big + 6000, 3000}, STR("newest"),
    };
    Buf b;
    buf_init(&b, &a, 64);
    CHECK(progress_checkpoint(&b, (Str){0}, fresh, 5, &a));
    Str out = buf_finish(&b);
    CHECK(str_has(out, STR("\n1 earlier request was left out.")));
    CHECK(str_has(out, STR("> [a request of 9000 bytes was left out; the "
                           "summary covers it]")));
    CHECK(str_has(out, STR("> newest")));
    CHECK(!str_has(out, STR("yyyy")));
    size_t dropped = 0;
    CHECK(progress_requests_parse(out, NULL, 0, &dropped) == 4);
    CHECK(dropped == 1);
    CHECK(out.n < AGENT_CHECKPOINT_REQUESTS_BYTES + 256);
}

static void progress_checkpoint_survives_a_short_arena(void) {
    WITH_ARENA(a, 512);
    static char big[4000];
    memset(big, 'z', sizeof big);
    Str fresh[] = {{big, sizeof big}};
    Buf b;
    buf_init(&b, &a, 64);
    CHECK(!progress_checkpoint(&b, (Str){0}, fresh, 1, &a) || !buf_ok(&b));
}

/* ---- conversation ------------------------------------------------------- */

static void conv_touch_keeps_a_bounded_unique_list(void) {
    WITH_ARENA(a, 1 << 20);
    Conv c;
    CHECK(conv_init(&c, &a, 8));
    CHECK(!conv_touch(&c, &a, (Str){0}));
    CHECK(c.touched_n == 0);
    CHECK(conv_touch(&c, &a, STR("a.txt")));
    CHECK(conv_touch(&c, &a, STR("a.txt")));
    CHECK(c.touched_n == 1);
    char name[32];
    for (size_t i = 1; i < AGENT_MAX_TOUCHED; i++) {
        i32 n = snprintf(name, sizeof name, "f%zu", i);
        CHECK(conv_touch(&c, &a, (Str){name, (size_t)n}));
    }
    CHECK(c.touched_n == AGENT_MAX_TOUCHED);
    CHECK(!c.touched_overflow);
    CHECK(!conv_touch(&c, &a, STR("one-too-many")));
    CHECK(c.touched_overflow);
    CHECK(c.touched_n == AGENT_MAX_TOUCHED);
    CHECK(conv_touch(&c, &a, STR("a.txt")));

    CHECK(conv_add(&c, M_SYSTEM, STR("sys")) == 0);
    CHECK(conv_add(&c, M_USER, STR("hi")) == 1);
    CHECK(conv_add(&c, M_ASSISTANT, STR("yo")) == 2);
    conv_truncate(&c, 2);
    CHECK(c.touched_n == AGENT_MAX_TOUCHED);
    CHECK(c.touched_overflow);
    conv_truncate(&c, 1);
    CHECK(c.touched_n == 0);
    CHECK(!c.touched_overflow);
}

static void conv_clone_carries_the_touched_list(void) {
    WITH_ARENA(a, 1 << 20);
    Conv c, copy;
    CHECK(conv_init(&c, &a, 8));
    CHECK(conv_add(&c, M_SYSTEM, STR("sys")) == 0);
    CHECK(conv_add(&c, M_USER, STR("hi")) == 1);
    CHECK(conv_touch(&c, &a, STR("a.txt")));
    CHECK(conv_touch(&c, &a, STR("b.txt")));
    CHECK(conv_clone_head(&copy, &c, 2, &a, 2));
    CHECK(copy.touched_n == 2);
    CHECK(str_eq(copy.touched[1], STR("b.txt")));
    CHECK(!copy.touched_overflow);
}

/* ---- child processes --------------------------------------------------- */

static void child_close_fds_reaches_past_the_fallback_cap(void) {
    enum { LOW_FD = 100, HIGH_FD = 65536 + 8 };
    struct rlimit saved;
    if (getrlimit(RLIMIT_NOFILE, &saved) != 0) {
        CHECK(!"getrlimit");
        return;
    }
    if (saved.rlim_max != RLIM_INFINITY && saved.rlim_max <= (rlim_t)HIGH_FD) {
        printf("  %s: skipped, the descriptor limit is below %d\n", g_case,
               HIGH_FD + 1);
        return;
    }
    struct rlimit raised = saved;
    if (raised.rlim_cur != RLIM_INFINITY && raised.rlim_cur <= (rlim_t)HIGH_FD)
        raised.rlim_cur = (rlim_t)HIGH_FD + 1;
    if (setrlimit(RLIMIT_NOFILE, &raised) != 0) {
        CHECK(!"setrlimit");
        return;
    }
    int null_fd = open("/dev/null", O_RDONLY);
    b8 low_placed = null_fd >= 0 && dup2(null_fd, LOW_FD) == LOW_FD;
    b8 high_placed = null_fd >= 0 && dup2(null_fd, HIGH_FD) == HIGH_FD;
    CHECK(low_placed && high_placed);
    if (low_placed && high_placed) {
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            child_close_fds(3);
            b8 closed =
                fcntl(LOW_FD, F_GETFD) == -1 && fcntl(HIGH_FD, F_GETFD) == -1;
            _exit(closed ? 0 : 1);
        }
        if (pid > 0) {
            int status = 0;
            CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status)
                  && WEXITSTATUS(status) == 0);
        }
    }
    if (high_placed) close(HIGH_FD);
    if (low_placed) close(LOW_FD);
    if (null_fd >= 0) close(null_fd);
    CHECK(setrlimit(RLIMIT_NOFILE, &saved) == 0);
}

int main(void) {
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
    RUN(base64_round_trips);
    RUN(base64_accepts_missing_padding);
    RUN(base64_rejects_malformed);
    RUN(base64_refuses_before_allocating);

    RUN(json_rejects_malformed);
    RUN(json_accepts_wellformed);
    RUN(json_tolerates_trailing_commas);
    RUN(json_strict_refuses_trailing_commas);
    RUN(json_survives_a_short_arena);
    RUN(json_reads_missing_members_as_absent);
    RUN(json_decodes_escapes);
    RUN(json_decodes_surrogate_pairs);
    RUN(json_bounds_nesting);
    RUN(json_round_trips);
    RUN(json_error_names_a_position);

    RUN(str_handles_empty);
    RUN(str_compares_ascii_without_case);
    RUN(width_classifies_glyphs);
    RUN(sha256_matches_known_answers);
    RUN(sha256_pads_at_the_block_edges);

    RUN(tasklog_round_trips);
    RUN(tasklog_keeps_a_partial_tail);
    RUN(tasklog_skips_what_it_cannot_read);
    RUN(tasklog_resynchronizes_after_a_huge_line);
    RUN(tasklog_writes_the_end_past_the_cap);
    RUN(media_refuses_a_short_label_allocation);
    RUN(media_refuses_full_capacity);

    RUN(theme_colour_parse_accepts_the_value_forms);
    RUN(theme_colour_parse_rejects_everything_else);
    RUN(theme_maps_hex_to_the_nearest_256_colour);
    RUN(theme_light_text_reads_on_the_page);

    RUN(progress_note_strip_drops_only_the_note);
    RUN(progress_parses_the_requests_back);
    RUN(progress_checkpoint_carries_and_quotes);
    RUN(progress_checkpoint_counts_the_first_compaction);
    RUN(progress_checkpoint_bounds_the_requests);
    RUN(progress_checkpoint_survives_a_short_arena);

    RUN(conv_touch_keeps_a_bounded_unique_list);
    RUN(conv_clone_carries_the_touched_list);

    RUN(child_close_fds_reaches_past_the_fallback_cap);
    if (g_fail) {
        printf("%d failure(s) in %d cases\n", g_fail, g_ran);
        return 1;
    }
    printf("%d/%d unit cases passed\n", g_ran, g_ran);
    return 0;
}
