/* libFuzzer harness for the provider reply readers.
 *
 * The first input byte picks the API (bit 0: Anthropic), the transport (bit
 * 1: one JSON body instead of an SSE stream) and whether reasoning is on (bit
 * 2). The rest is what the provider sent. A stub http_post hands it to the
 * reader line by line, the way the real one does, and provider_run builds the
 * assistant turn from it, under ASan and UBSan.
 *
 * Whatever arrived, the conversation it leaves must still serialize to JSON
 * that parses, since that is the next request. Every stored tool call must
 * have a name. Built by `make fuzz` into bin/fuzz/fuzz_stream.
 */

#define _XOPEN_SOURCE   700
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "agent.h"

void telemetry_log(i32 level, Str msg) {
    (void)level;
    (void)msg;
}

void tel_open(TelEvent *e, const char *ev) {
    (void)e;
    (void)ev;
}

void tel_int(TelEvent *e, const char *key, i64 v) {
    (void)e;
    (void)key;
    (void)v;
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

Str api_name(ApiKind k) {
    return k == API_ANTHROPIC ? STR("anthropic") : STR("openai");
}

void tools_write_schemas(Buf *b, const ToolRegistry *r, ApiKind api,
                         ToolAudience audience) {
    (void)r;
    (void)api;
    (void)audience;
    buf_puts(b, STR("[]"));
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
    return 1;
}

#include "core.c"
#include "width.c"
#include "json.c"
#include "spill.c"
#include "media.c"
#include "provider.c"

#include <stdint.h>

static struct {
    Str reply;
} g_fuzz_wire;

i32 http_post(const HttpReq *r) {
    Str in = g_fuzz_wire.reply;
    if (r->body_out) {
        buf_put(r->body_out, in.p, in.n);
        return 0;
    }
    size_t start = 0;
    for (size_t i = 0; i <= in.n; i++) {
        if (i < in.n && in.p[i] != '\n') continue;
        Str line = {in.p + start, i - start};
        start = i + 1;
        if (line.n && line.p[line.n - 1] == '\r') line.n--;
        if (r->on_line && !r->on_line(line, r->ud)) break;
    }
    return 0;
}

static void fuzz_on_text(Str delta, void *ud) {
    (void)delta;
    (void)ud;
}

static void fuzz_on_tool_call(i32 index, Str id, Str name, Str args_delta,
                              size_t received, void *ud) {
    (void)index;
    (void)id;
    (void)args_delta;
    (void)received;
    (void)ud;
    if (!name.n) abort();
}

static void fuzz_on_usage(const Conv *conv, size_t prompt_tokens,
                          size_t completion_tokens, void *ud) {
    (void)conv;
    (void)prompt_tokens;
    (void)completion_tokens;
    (void)ud;
}

static alignas(64) u8 g_fuzz_persist[1u << 23];
static alignas(64) u8 g_fuzz_scratch[1u << 25];
static alignas(64) u8 g_fuzz_check[1u << 25];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1 || size > (1u << 20)) return 0;
    u8 flags = data[0];
    g_fuzz_wire.reply = (Str){(const char *)data + 1, size - 1};

    Arena persist, scratch, check;
    arena_init(&persist, g_fuzz_persist, sizeof g_fuzz_persist);
    arena_init(&scratch, g_fuzz_scratch, sizeof g_fuzz_scratch);
    arena_init(&check, g_fuzz_check, sizeof g_fuzz_check);

    Config cfg = {0};
    cfg.api = (flags & 1) ? API_ANTHROPIC : API_OPENAI;
    cfg.stream = !(flags & 2);
    cfg.model = STR("fuzz-model");
    cfg.base_url = STR("http://127.0.0.1:1");
    cfg.max_tokens = 1024;
    if (flags & 4) cfg.reasoning_effort = STR("high");

    Conv conv;
    if (!conv_init(&conv, &persist, 64)) abort();
    conv_add(&conv, M_SYSTEM, STR("system"));
    conv_add(&conv, M_USER, STR("hello"));

    Provider p = {
        .cfg = &cfg,
        .conv = &conv,
        .persist = &persist,
        .scratch = &scratch,
        .on_text = fuzz_on_text,
        .on_reason = fuzz_on_text,
        .on_tool_call = fuzz_on_tool_call,
        .on_usage = fuzz_on_usage,
        .idle_fd = -1,
    };
    char err[256];
    i32 rc = provider_run(&p, err, sizeof err);
    if (rc > 0 && conv.n < 3 + (size_t)rc) abort();

    for (size_t i = 0; i < conv.n; i++)
        if (conv_is_call(&conv, i) && !conv.tool_name[i].n) abort();

    size_t turn_end = conv.n;
    for (size_t i = 0; i < turn_end; i++)
        if (conv_is_call(&conv, i))
            conv_add_tool(&conv, conv.tool_call_id[i], STR("ok"));

    Buf out;
    buf_init(&out, &check, 1u << 22);
    if (cfg.api == API_ANTHROPIC)
        conv_write_json_anthropic(&out, &conv);
    else
        conv_write_json(&out, &conv, NULL);
    if (!buf_ok(&out)) return 0;
    if (!json_parse(&check, buf_finish(&out))) abort();
    return 0;
}
