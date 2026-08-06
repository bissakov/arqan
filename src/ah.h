/* ah.h — umbrella header for the ah AI coding harness.
 *
 * Every module includes this. Types are data-oriented (SoA) and everything
 * is backed by arenas; no malloc/free appears in the hot path.
 */
#ifndef AH_H
#define AH_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>
#include <signal.h>

/* ---- fixed-width aliases ----------------------------------------------- */
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float    f32;
typedef double   f64;
typedef bool     b8;

#define AH_VERSION "0.1.0"

/* ---- capacities (compile-time, no growth) ------------------------------- */
#define AH_ARENA_BYTES      (1u << 28)  /* 256 MiB scratch arena            */
#define AH_PERSIST_BYTES    (1u << 26)  /* 64  MiB persistent arena         */
#define AH_MAX_MESSAGES     4096
#define AH_MAX_TOOLS        64
#define AH_MAX_TOOL_CALLS   1024        /* per turn                          */
#define AH_MAX_TOOL_ARGS    8
#define AH_LINE_BUF         (1u << 20)  /* 1 MiB input line buffer          */
#define AH_RESP_BUF         (1u << 22)  /* 4 MiB response accumulation      */

/* ---- arenas ------------------------------------------------------------- */
typedef struct {
    u8 *base;
    size_t         cap;
    size_t         off;
} Arena;

void    arena_init(Arena *a, void *mem, size_t cap);
void   *arena_alloc(Arena *a, size_t n, size_t align);
void    arena_reset(Arena *a);
size_t  arena_used(const Arena *a);
/* aligned, zeroed, typed helper */
#define arena_new(a, T, n) ((T *)arena_alloc((a), sizeof(T) * (n), alignof(T)))

/* ---- string view + builder ---------------------------------------------- */
typedef struct { const char *p; size_t n; } Str;
#define STR(lit) ((Str){ (lit), sizeof(lit) - 1 })

Str     str_c(const char *z);                  /* strlen-based              */
Str     str_dup(Arena *a, Str s);              /* copies into arena         */
b8    str_eq(Str a, Str b);
Str     str_trim(Str s);
Str     str_take(Str s, size_t n);
Str     str_drop(Str s, size_t n);
i64    str_int(Str s, b8 *ok);

/* growable char buffer living in an arena (no realloc: doubles into arena) */
typedef struct { char *p; size_t n, cap; Arena *a; } Buf;
void    buf_init(Buf *b, Arena *a, size_t cap);
void    buf_putc(Buf *b, char c);
void    buf_put(Buf *b, const void *p, size_t n);
void    buf_puts(Buf *b, Str s);
void    buf_putf(Buf *b, const char *fmt, ...) __attribute__((format(printf,2,3)));
void    buf_json_str(Buf *b, Str s);           /* JSON-escaped string      */
Str     buf_finish(Buf *b);                    /* nul-terminates, returns   */

/* ---- logging ------------------------------------------------------------ */
enum { AH_LOG_DEBUG, AH_LOG_INFO, AH_LOG_WARN, AH_LOG_ERROR };
void    ah_log(i32 level, const char *fmt, ...) __attribute__((format(printf,2,3)));
void    ah_log_set_level(i32 level);

/* ---- time --------------------------------------------------------------- */
f64  ah_now_seconds(void);   /* monotonic                                */

/* ---- JSON ----------------------------------------------------------------
 * Minimal arena-backed DOM. Values live in the arena; no heap.
 */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct JVal JVal;
struct JVal {
    Str   key;        /* set for object members                            */
    JType type;
    union {
        b8    b;
        f64  n;
        Str     s;            /* for J_STR                              */
        struct { JVal *items; size_t n; } arr;          /* J_ARR          */
        struct { JVal *head; } obj;                     /* J_OBJ (linked) */
    } u;
    JVal  *next;     /* next sibling in object                                */
};

typedef struct { Arena *a; const char *src; size_t pos, len; b8 oom; } JParser;

JVal   *json_parse(Arena *a, Str s);            /* NULL on error             */
void    json_write(Buf *b, const JVal *v);
const JVal *json_get(const JVal *obj, Str key);  /* object lookup            */
const JVal *json_at(const JVal *arr, size_t i);

/* ---- config ------------------------------------------------------------- */
typedef struct {
    Str base_url;     /* e.g. https://api.openai.com/v1                    */
    Str model;
    Str api_key;
    Str system_prompt;
    i32  max_tokens;
    b8 stream;
} Config;

b8    config_load(Config *c, Arena *persist);

/* ---- HTTP (libcurl) ----------------------------------------------------- */
typedef struct {
    const char *base_url;
    const char *api_key;
    /* SSE chunk callback: called with each accumulated line (without the
     * "data:" prefix). Return false to abort the stream. */
    b8 (*on_line)(Str line, void *ud);
    void *ud;
    const char *body;     /* nul-terminated JSON request                    */
    const volatile sig_atomic_t *interrupt_flag;
} HttpReq;

i32     http_sse_post(const HttpReq *r);  /* 0 on success, nonzero on error */

/* ---- tools (SoA registry) ---------------------------------------------- */
typedef struct {
    Str  name;
    Str  desc;
    Str  schema;          /* JSON schema fragment (object)                  */
    b8 (*run)(Str args_json, Arena *scratch, Buf *out, char *err, size_t err_cap);
} ToolDef;

typedef struct {
    ToolDef *defs;        /* [AH_MAX_TOOLS] SoA: one big array             */
    size_t   n;
} ToolRegistry;

void        tools_init(ToolRegistry *r, Arena *persist);
const ToolDef *tools_find(const ToolRegistry *r, Str name);
void        tools_write_schemas(Buf *b, const ToolRegistry *r);

/* ---- conversation (SoA) ------------------------------------------------- */
typedef enum { M_SYSTEM = 0, M_USER, M_ASSISTANT, M_TOOL } MRole;

typedef struct {
    /* SoA: parallel arrays indexed by message id                         */
    MRole *role;          /* [cap]                                       */
    Str   *text;          /* [cap] content (prose / tool result / args)  */
    Str   *tool_name;     /* [cap] tool name (assistant tool-call entry) */
    Str   *tool_call_id;  /* [cap] tool call id                          */
    b8  *has_tool_call; /* [cap] true => assistant msg w/ a tool call  */
    size_t n, cap;
} Conv;

void    conv_init(Conv *c, Arena *persist, size_t cap);
size_t  conv_add(Conv *c, MRole role, Str text);
size_t  conv_add_assistant_toolcall(Conv *c, Str content, Str id, Str name, Str args);
size_t  conv_add_tool(Conv *c, Str tool_call_id, Str text);
void    conv_write_json(Buf *b, const Conv *c, const ToolRegistry *reg);

/* ---- provider ----------------------------------------------------------- */
typedef struct {
    const Config      *cfg;
    const ToolRegistry*tools;
    Conv              *conv;
    Arena             *persist;   /* persistent arena (message storage)        */
    Arena             *scratch;   /* per-turn scratch (reset each turn)        */
    /* streaming sinks */
    void (*on_text)(Str delta, void *ud);
    void (*on_tool_call)(i32 index, Str id, Str name, Str args_delta, void *ud);
    void *ud;
    const volatile sig_atomic_t *interrupt_flag;
    size_t prompt_tokens;
    size_t completion_tokens;
    size_t total_tokens;
    b8 usage_valid;
} Provider;

/* Run one completion turn. Appends the assistant message + tool calls to
 * conv (in the persistent arena). Returns number of tool calls emitted. */
i32     provider_run(Provider *p, char *err, size_t err_cap);

/* ---- TUI --------------------------------------------------------------- */
void tui_start(Str model, Str base_url, b8 missing_key, size_t tool_count);
void tui_stop(void);
void tui_set_status(const char *status);
void tui_set_context_tokens(size_t tokens);
void tui_clear(void);
void tui_write(Str s);
void tui_printf(const char *fmt, ...) __attribute__((format(printf,1,2)));
void tui_enter_raw(void);
void tui_exit_raw(void);
void tui_putstr(Str s);
b8 tui_readline(const char *prompt, char *buf, size_t cap, size_t *out_n);

#endif /* AH_H */
