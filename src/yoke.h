/* yoke.h: umbrella header for the yoke AI coding harness.
 *
 * Every module includes this. Types are data-oriented (SoA) and everything
 * is backed by arenas; no malloc/free appears in the hot path.
 */
#ifndef YOKE_H
#define YOKE_H

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

#define YOKE_VERSION "0.1.0"

/* ---- capacities (compile-time, no growth) ------------------------------- */
/* Sized against the per-turn peak rather than picked round: a turn holds one
 * 4 MiB event arena, the accumulated reply, the tool output and the doubling
 * these go through, which lands an order of magnitude under the scratch
 * arena. Both are static storage, so this is address space, not startup
 * cost, but it is also what a core dump has to carry. */
#define YOKE_ARENA_BYTES      (1u << 27)  /* 128 MiB scratch arena            */
#define YOKE_PERSIST_BYTES    (1u << 26)  /* 64  MiB persistent arena         */
#define YOKE_MAX_MESSAGES     4096        /* default; see Config.max_messages   */
#define YOKE_MAX_TOOLS        64
#define YOKE_MAX_TOOL_CALLS   1024        /* per turn                          */
#define YOKE_MAX_TOOL_ARGS    8
#define YOKE_MAX_JSON_DEPTH   64          /* nesting a provider may hand us    */
#define YOKE_MAX_PATH         4096        /* longest path a tool will accept   */
#define YOKE_MAX_COMMAND      (1u << 16)  /* longest shell command             */
#define YOKE_MAX_FILE_BYTES   (16u << 20) /* largest file a tool will read     */
#define YOKE_MAX_COMMANDS     32          /* slash commands offered by the TUI */
#define YOKE_LINE_BUF         (1u << 20)  /* 1 MiB input line buffer          */
#define YOKE_RESP_BUF         (1u << 22)  /* 4 MiB response accumulation      */
#define YOKE_MAX_HISTORY      500         /* recallable prompts kept on disk   */
#define YOKE_HISTORY_BYTES    (1u << 20)  /* entry storage carved from persist */
#define YOKE_MAX_HISTORY_LINE (1u << 16)  /* longest prompt worth remembering  */
#define YOKE_MAX_HISTORY_BYTES (8u << 20) /* largest history file we will read */
#define YOKE_MAX_CONFIG_FILES 8           /* XDG config candidates we consider */
/* Largest SYSTEM.md accepted, about 65k characters: a prompt that long is a
 * mistake worth reporting, not a document worth loading. Past it yoke refuses
 * to start rather than send a truncated prompt. */
#define YOKE_MAX_PROMPT_FILE  (1u << 16)
#define YOKE_MAX_AGENTS_FILES 8           /* AGENTS.md chain depth we collect  */
#define YOKE_MAX_SESSIONS     64          /* saved sessions the picker offers  */
#define YOKE_MAX_SESSION_BYTES (32u << 20)/* largest session file we will read */
#define YOKE_MAX_POPUP        256         /* entries the popup can hold        */
#define YOKE_MAX_MODELS       256         /* models the /model picker offers   */
#define YOKE_MAX_ENDPOINTS    32          /* providers /provider can hold      */
#define YOKE_MAX_ENDPOINT_NAME 64
#define YOKE_MAX_URL          512
#define YOKE_MAX_MODEL_NAME   128
#define YOKE_MAX_API_KEY      512
#define YOKE_MAX_MODEL_BYTES  (1u << 20)  /* largest /models reply we will read */

/* ---- arenas ------------------------------------------------------------- */
typedef struct {
    u8 *base;
    size_t         cap;
    size_t         off;
} Arena;

void    arena_init(Arena *a, void *mem, size_t cap);
void   *arena_alloc(Arena *a, size_t n, size_t align);
/* count * size with the multiplication checked: a size derived from provider
 * or file data must never wrap into a small, satisfiable request. */
void   *arena_alloc_array(Arena *a, size_t count, size_t size, size_t align);
void    arena_reset(Arena *a);
size_t  arena_used(const Arena *a);
/* aligned, typed helper (contents are uninitialised) */
#define arena_new(a, T, n) ((T *)arena_alloc_array((a), (n), sizeof(T), alignof(T)))

/* ---- string view + builder ---------------------------------------------- */
typedef struct { const char *p; size_t n; } Str;
#define STR(lit) ((Str){ (lit), sizeof(lit) - 1 })

Str     str_c(const char *z);                  /* strlen-based              */
Str     str_dup(Arena *a, Str s);              /* copies into arena         */
b8    str_eq(Str a, Str b);
b8    str_starts(Str s, Str prefix);
Str     str_trim(Str s);
Str     str_take(Str s, size_t n);
Str     str_drop(Str s, size_t n);
i64    str_int(Str s, b8 *ok);

/* Growable char buffer living in an arena (no realloc: doubles into arena).
 * `oom` latches when the arena could not satisfy a growth: every later write
 * is dropped rather than run past `cap`, and callers check buf_ok() before
 * trusting the contents. */
typedef struct { char *p; size_t n, cap; Arena *a; b8 oom; } Buf;
void    buf_init(Buf *b, Arena *a, size_t cap);
b8      buf_ok(const Buf *b);
void    buf_putc(Buf *b, char c);
void    buf_put(Buf *b, const void *p, size_t n);
void    buf_puts(Buf *b, Str s);
void    buf_putf(Buf *b, const char *fmt, ...) __attribute__((format(printf,2,3)));
void    buf_json_str(Buf *b, Str s);           /* JSON-escaped string      */
/* Its body without the quotes, so several pieces can share one string. */
void    buf_json_chars(Buf *b, Str s);
Str     buf_finish(Buf *b);                    /* nul-terminates, returns   */

/* ---- logging ------------------------------------------------------------ */
enum { YOKE_LOG_DEBUG, YOKE_LOG_INFO, YOKE_LOG_WARN, YOKE_LOG_ERROR };
void    yoke_log(i32 level, const char *fmt, ...) __attribute__((format(printf,2,3)));
void    yoke_log_set_level(i32 level);
/* Whoever owns the terminal owns the log: while the fullscreen UI is up, raw
 * stderr would paint over the frame, so the TUI redirects log lines into the
 * transcript instead. NULL restores plain stderr. */
typedef void (*YokeLogSink)(i32 level, Str msg, void *ud);
void    yoke_log_set_sink(YokeLogSink sink, void *ud);

/* ---- time --------------------------------------------------------------- */
f64  yoke_now_seconds(void);   /* monotonic                                */

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

typedef struct { Arena *a; const char *src; size_t pos, len; i32 depth; b8 oom; } JParser;

JVal   *json_parse(Arena *a, Str s);            /* NULL on error             */
void    json_write(Buf *b, const JVal *v);
const JVal *json_get(const JVal *obj, Str key);  /* object lookup            */
const JVal *json_at(const JVal *arr, size_t i);

/* ---- XDG base directories ------------------------------------------------
 * Every file yoke owns is resolved here and none of them sits directly in
 * $HOME. A relative XDG_* value is invalid and ignored, as the spec requires.
 */
typedef enum { YOKE_DIR_CONFIG, YOKE_DIR_DATA, YOKE_DIR_STATE, YOKE_DIR_CACHE } YokeDir;

/* Absolute "<base>/yoke" path, empty when no base resolves. */
Str    paths_dir(YokeDir kind, Arena *a);
Str    paths_file(YokeDir kind, Str name, Arena *a);
b8     paths_ensure_dir(Str dir);    /* mkdir -p, mode 0700                  */
/* Candidates for a config file, lowest precedence first, so applying them in
 * order leaves the user's own file on top. */
size_t paths_config_files(Str name, Arena *a, Str *out, size_t max);

/* ---- prompt history ------------------------------------------------------
 * A ring of past prompts, mirrored to $XDG_STATE_HOME/yoke/history as they
 * are submitted. `cursor` is the browse position and cursor == n means the
 * live draft, which is what the composer restores on the way back down.
 *
 * Entries need an arena of their own because /clear rewinds the session's,
 * and a recallable prompt has to outlive that. A full one is compacted in
 * place: a bump allocator hands entries out in ascending address order, so
 * the move never overlaps forward.
 */
typedef struct {
    Str   *entry;   /* [cap] oldest first                                   */
    size_t n, cap;
    size_t cursor;
    Str    path;    /* nul-terminated; empty disables persistence           */
    Arena *a;       /* entry storage, used by nothing else                  */
    size_t base_off;/* where entries start, past the index array            */
} History;

b8   history_init(History *h, Arena *own, size_t cap);
void history_load(History *h, Str path, Arena *scratch);
void history_rewrite(const History *h);
void history_add(History *h, Str line);
b8   history_prev(History *h, Str *out);
/* false once it steps past the newest entry, where the draft belongs. */
b8   history_next(History *h, Str *out);
void history_reset_cursor(History *h);
b8   history_browsing(const History *h);

/* ---- endpoints -----------------------------------------------------------
 * The providers /provider creates and switches between: a name, an
 * OpenAI-compatible base URL and the model last used against it. None are
 * built in, since they all speak the same protocol and only the user knows
 * which ones exist. Settings live in $XDG_CONFIG_HOME/yoke/providers and the
 * keys in $XDG_STATE_HOME/yoke/credentials, so a shared configuration cannot
 * carry a secret; $XDG_STATE_HOME/yoke/provider names the active one.
 *
 * SoA, indexed by entry, with the caps above enforced on load: an oversized
 * field is dropped rather than truncated, since a cut URL or key names a
 * different service.
 */
typedef struct {
    Str    name[YOKE_MAX_ENDPOINTS];
    Str    base_url[YOKE_MAX_ENDPOINTS];
    Str    model[YOKE_MAX_ENDPOINTS];    /* empty when none was chosen yet  */
    size_t n;
} Endpoints;

#define ENDPOINT_NONE ((size_t)-1)

/* Every Str lands in `a` and lives as long as it does. */
size_t endpoints_load(Endpoints *e, Arena *a);
size_t endpoints_find(const Endpoints *e, Str name);
/* Add an entry or replace the one of that name, copying into `a`. False when
 * the store is full or a field is past its cap. */
b8     endpoints_put(Endpoints *e, Str name, Str base_url, Str model,
                     Arena *a);
b8     endpoints_save(const Endpoints *e, Arena *scratch);
/* Record the model in use against `name`, which is where /model writes while
 * a provider is active. False when there is no such entry. */
b8     endpoints_remember_model(Str name, Str model, Arena *scratch);
/* The key stored for `name`, allocated in `out`. Empty when there is none,
 * and empty with `err` filled in when the credentials file is readable by
 * anyone but its owner, which is a key to rotate rather than one to load. */
Str    endpoints_key(Str name, Arena *out, Arena *scratch,
                     char *err, size_t err_cap);
b8     endpoints_set_key(Str name, Str key, Arena *scratch,
                         char *err, size_t err_cap);
Str    endpoints_active(Arena *a);
b8     endpoints_remember_active(Str name, Arena *scratch);

/* ---- config ------------------------------------------------------------- */
typedef struct {
    Str base_url;     /* e.g. https://api.openai.com/v1                    */
    Str model;
    Str api_key;
    Str provider;     /* active endpoint name; empty when none is selected */
    /* Whether anything named the endpoint, as opposed to the built-in
     * default: a run with neither this nor a key has nothing to talk to, and
     * asks for a provider instead of starting a conversation. */
    b8  base_url_set;
    Str system_prompt; /* Only --system and YOKE_SYSTEM_PROMPT set this. */
    i32  max_tokens;
    /* Conversation capacity. Configurable so the full-history path is
     * reachable in a test without streaming four thousand messages. */
    size_t max_messages;
    b8 stream;
} Config;

/* `scratch` holds the config file while it is parsed; nothing survives in it. */
b8    config_load(Config *c, Arena *persist, Arena *scratch);
/* Remember the model picked with /model in $XDG_STATE_HOME/yoke/model, which
 * config_load applies above the config files and below YOKE_MODEL. False when
 * no state directory resolves or the write failed. */
b8    config_remember_model(Str model, Arena *scratch);


/* ---- command line ------------------------------------------------------- */
/* Every Str points into argv, so nothing is copied and nothing is freed. */
typedef struct {
    Str base_url, model, api_key, system_prompt;
    Str prompt;        /* the one turn to run; see have_prompt              */
    b8  have_prompt;   /* true even for an empty prompt, which is an error  */
    i32 max_tokens;    /* 0 leaves the configured value alone               */
} CliOpts;

typedef enum {
    CLI_RUN,     /* carry on starting up                                    */
    CLI_DONE,    /* --help or --version was answered; exit 0                */
    CLI_ERROR,   /* the usage error is already on stderr; exit 2            */
} CliStatus;

CliStatus cli_parse(i32 argc, char **argv, CliOpts *out);
/* Applies the overrides above whatever config_load resolved. */
void      cli_apply(const CliOpts *o, Config *c);

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
    /* The transfer waits on curl's sockets and `idle_fd` together, so the UI
     * stays alive during a request without threads. on_idle runs after every
     * wait (readable fd or timeout); it must not block. */
    i32   idle_fd;        /* -1 disables the extra poll fd                  */
    void (*on_idle)(void *ud);
    void *idle_ud;
} HttpReq;

i32     http_sse_post(const HttpReq *r);  /* 0 on success, nonzero on error */
/* GET base_url + path, appending the whole body to `out`. Returns 0 on
 * success, a negative HTTP status for a refused request, positive for a
 * transport failure. Blocking: the callers fetch a short document between
 * turns, not while one is streaming. */
i32     http_get(const char *base_url, const char *path, const char *api_key,
                Buf *out);

/* ---- tools (SoA registry) ----------------------------------------------
 * Parallel arrays indexed by tool id. Lookup only ever touches `name`, so
 * the names sit together instead of being spread across whole tool records.
 */
typedef b8 (*ToolRun)(Str args_json, Arena *scratch, Buf *out,
                      char *err, size_t err_cap);

typedef struct {
    Str     *name;        /* [YOKE_MAX_TOOLS]                               */
    Str     *desc;        /* [YOKE_MAX_TOOLS]                               */
    Str     *schema;      /* [YOKE_MAX_TOOLS] JSON schema fragment (object) */
    ToolRun *run;         /* [YOKE_MAX_TOOLS]                               */
    size_t   n;
} ToolRegistry;

/* Tool ids are indices into the registry; TOOL_NONE means "no such tool". */
#define TOOL_NONE ((size_t)-1)

void        tools_init(ToolRegistry *r, Arena *persist);
size_t      tools_find(const ToolRegistry *r, Str name);
b8          tools_run(const ToolRegistry *r, size_t id, Str args,
                      Arena *scratch, Buf *out, char *err, size_t err_cap);
void        tools_write_schemas(Buf *b, const ToolRegistry *r);
/* Run `cmd` through the shell, appending its output to `out` followed by a
 * bracketed status line ("[exit 0]"), which is how the bash tool reports and
 * how render.c reads a result. False with `err` filled in when the command is
 * longer than YOKE_MAX_COMMAND or the shell could not be started; a command
 * is never clamped to fit, since a truncated one is a different program. */
b8          shell_capture(Str cmd, Buf *out, char *err, size_t err_cap);

/* ---- prompt ------------------------------------------------------------- */
/* The system prompt, placeholders expanded. `configured` is what --system or
 * YOKE_SYSTEM_PROMPT set, unset to take .yoke/SYSTEM.md, the global
 * SYSTEM.md or the built-in template, in that order. Returned in `persist`,
 * `scratch` holds the file while it is read; the unexpanded template is the
 * fallback when `persist` cannot take the result. Empty with `err` set when a
 * SYSTEM.md is larger than YOKE_MAX_PROMPT_FILE.
 *
 * Whichever prompt wins, every AGENTS.md from the working directory up to the
 * root is appended to it as project context, outermost first. It is a
 * document about the project rather than a template, so it is appended
 * verbatim, placeholders included. */
Str   prompt_build(const ToolRegistry *tools, Str configured, Arena *persist,
                   Arena *scratch, char *err, size_t err_cap);

/* ---- conversation (SoA) ------------------------------------------------- */
typedef enum { M_SYSTEM = 0, M_USER, M_ASSISTANT, M_TOOL } MRole;

/* Returned by every conv_* append when the conversation is full. */
#define CONV_NONE ((size_t)-1)

typedef struct {
    /* SoA: parallel arrays indexed by message id                         */
    MRole *role;          /* [cap]                                       */
    Str   *text;          /* [cap] content (prose / tool result / args)  */
    Str   *tool_name;     /* [cap] tool name (assistant tool-call entry) */
    Str   *tool_call_id;  /* [cap] tool call id                          */
    Str   *shell_out;     /* [cap] what a '!' run printed; see conv_add_shell */
    b8  *has_tool_call; /* [cap] true => assistant msg w/ a tool call  */
    b8  *expanded;      /* [cap] this block's transcript caps are lifted */
    size_t n, cap;
} Conv;

b8      conv_init(Conv *c, Arena *persist, size_t cap);
size_t  conv_add(Conv *c, MRole role, Str text);
/* An assistant turn with tool calls is a head slot (prose) followed by one
 * carrier slot per call, each keeping its own id. `conv_is_call` picks the
 * carriers out of a tail scan. */
size_t  conv_add_assistant_calls(Conv *c, Str content);
size_t  conv_add_call(Conv *c, Str id, Str name, Str args);
size_t  conv_add_tool(Conv *c, Str tool_call_id, Str text);
/* A '!' shell run: one user slot holding the command and what it printed,
 * because it is one turn the user took. The provider sees it as the user
 * message it is; `conv_is_shell` picks it out of the user turns. */
size_t  conv_add_shell(Conv *c, Str cmd, Str out);
b8      conv_is_shell(const Conv *c, size_t i);
b8      conv_is_call(const Conv *c, size_t i);
size_t  conv_room(const Conv *c);
void    conv_write_json(Buf *b, const Conv *c, const ToolRegistry *reg);

/* ---- sessions ------------------------------------------------------------
 * The conversation as it happened, one JSON object per line under
 * $XDG_DATA_HOME/yoke/sessions/<cwd>/<timestamp>.jsonl. Sessions are keyed by
 * the directory yoke was launched in: that is the unit of work, so browsing
 * from one project never surfaces another's.
 *
 * Paths live in the struct instead of an arena because /clear rewinds the
 * session arena and the file the next message appends to has to outlive it.
 */
typedef struct {
    char   dir_buf[YOKE_MAX_PATH];
    char   path_buf[YOKE_MAX_PATH];
    char   name_buf[32];
    Str    dir;      /* per-cwd directory; empty when no XDG base resolves  */
    Str    path;     /* live session file; empty disables persistence       */
    Str    name;     /* its file name                                       */
    size_t written;  /* conversation slots already on disk                  */
} Session;

/* SoA, like every other collection: the picker only reads names and previews. */
typedef struct {
    Str   *name;     /* [n] timestamp label, newest first                   */
    Str   *path;     /* [n] nul-terminated file path                        */
    Str   *preview;  /* [n] first prompt of the session, one line           */
    size_t n;
} SessionList;

b8     session_init(Session *s, Arena *scratch);   /* resolve the cwd's dir  */
b8     session_begin(Session *s);                  /* new timestamped file   */
/* Append the messages produced since the last call; the file is created on
 * the first one, so an untouched session never reaches the picker. */
void   session_save(Session *s, const Conv *c);
/* Start a new file holding the conversation whole and continue in it, leaving
 * the one it was appending to as it is. False when the file could not be
 * started or the copy could not be written. */
b8     session_fork(Session *s, const Conv *c);
size_t session_list(const Session *s, Arena *a, SessionList *out, size_t max);
/* Reading is separate from replaying: replaying rewinds the live conversation
 * and overwrites its storage, so whether the file can be read at all has to
 * be known before anything is thrown away. `session_read` returns the raw
 * contents in `scratch` (empty when unreadable); `session_apply` replays them
 * into a conversation the caller has rewound to its system prompt and
 * continues appending to that file. False means the conversation filled up
 * and holds only part of the session. */
Str    session_read(Str path, Arena *scratch);
b8     session_apply(Session *s, Str src, Str path, Str name, Conv *c,
                     Arena *persist, Arena *scratch);

/* ---- provider ----------------------------------------------------------- */
typedef struct {
    const Config      *cfg;
    const ToolRegistry*tools;
    Conv              *conv;
    Arena             *persist;   /* persistent arena (message storage)        */
    Arena             *scratch;   /* per-turn scratch (reset each turn)        */
    /* streaming sinks */
    void (*on_text)(Str delta, void *ud);
    /* Reasoning deltas ("reasoning_content" or "reasoning"): displayed as the
     * turn streams, never appended to the conversation, since a provider
     * rejects a thinking trace it did not produce itself. */
    void (*on_reason)(Str delta, void *ud);
    void (*on_tool_call)(i32 index, Str id, Str name, Str args_delta, void *ud);
    void *ud;
    /* pumped while the request is in flight (see HttpReq.on_idle) */
    void (*on_idle)(void *ud);
    i32   idle_fd;                /* -1 when there is nothing to watch         */
    const volatile sig_atomic_t *interrupt_flag;
    size_t prompt_tokens;
    size_t completion_tokens;
    size_t total_tokens;
    b8 usage_valid;
} Provider;

/* Run one completion turn. Appends the assistant message + tool calls to
 * conv (in the persistent arena). Returns the number of tool calls emitted,
 * or -1 with `err` filled in. */
i32     provider_run(Provider *p, char *err, size_t err_cap);

/* Model ids from GET <base_url>/models, in the order the endpoint serves
 * them. Names are allocated in `scratch`; returns how many landed in `out`,
 * zero with `err` filled in when the endpoint could not be read. */
size_t  provider_models(const Config *cfg, Arena *scratch, Str *out, size_t max,
                        char *err, size_t err_cap);

/* ---- TUI --------------------------------------------------------------- */
/* Styles a range of transcript text can carry. The block styles claim whole
 * rows (a wrapped continuation is painted like its first row); the inline
 * ones apply to the bytes they cover, so several can share a row. */
typedef enum {
    TUI_PLAIN = 0, TUI_HEADING, TUI_CODE, TUI_QUOTE,   /* block */
    TUI_BOLD, TUI_EMPH, TUI_MONO, TUI_MARKER           /* inline */
} TuiStyle;
/* A slash command the composer's completion popup can offer. The table is
 * owned by the caller (static storage) and only read by the TUI. */
typedef struct { Str name; Str desc; } TuiCmd;
void tui_set_commands(const TuiCmd *cmds, size_t n);
/* Which end of a picker's list the selection sits on: where it opens, and
 * where it returns to when a search narrows the list. A list ordered like the
 * transcript it points into ends at the entry nearest the composer, which is
 * the one a picker like that opens on. */
typedef enum { TUI_PICK_FIRST = 0, TUI_PICK_LAST } TuiPickAnchor;
/* Modal picker: the completion popup, driven over a caller-owned list instead
 * of the command table. `title` names it in the status line. Enter chooses
 * (index in *out), Esc/Ctrl-C cancels. Returns false when nothing was chosen
 * or there is no fullscreen UI. Past ten entries the picker also takes the
 * keyboard: typing filters the list by literal substring, and the composer's
 * own text is left untouched. */
b8 tui_pick(Str title, const TuiCmd *items, size_t n, TuiPickAnchor anchor,
            size_t *out);
/* Modal one-line question, answered in the composer with `question` in the
 * notice row. `secret` echoes the answer as dots and keeps it out of the
 * prompt history and the transcript, which is what an API key wants. Returns
 * false when Esc or Ctrl-C cancelled, the answer was empty, or there is no
 * fullscreen UI. The composer's own text is restored on the way out. */
b8 tui_ask(Str question, b8 secret, char *out, size_t cap);
/* Composer history for Up/Down recall; NULL disables it. */
void tui_set_history(History *h);
/* `plain` forces the line-oriented path and drops the banner even on a tty,
 * which is what a one-shot -p run wants: its stdout is a reply, not a UI. */
void tui_start(Str model, Str base_url, b8 missing_key, size_t tool_count,
               b8 plain);
/* The model the status line names; the string must outlive the call. */
void tui_set_model(Str model);
/* The provider the status line names. Empty restores the host derived from
 * the base URL, which is what an endpoint-less run shows. */
void tui_set_provider(Str name);
/* What a run with no endpoint says, on the welcome screen and again if a
 * message is submitted anyway. */
#define NO_PROVIDER_HINT \
    STR("no provider yet: type /provider, then \"+ add a provider\"")
/* With nothing to talk to, the welcome screen's closing line says how to name
 * an endpoint instead of a form opening over it. */
void tui_needs_provider(b8 on);
/* Hand `text` to the terminal's clipboard over OSC 52, the path a drag-select
 * copy takes, and acknowledge it on the status line. Returns false for an
 * empty payload or one past the sequence cap, which is refused rather than
 * truncated: half a reply on the clipboard is not the one that was asked
 * for. */
b8 tui_copy(Str text);
void tui_stop(void);
void tui_set_status(const char *status);
void tui_set_context_tokens(size_t tokens);
void tui_clear(void);
/* Drop the transcript alone, leaving the context counter as it is: the
 * conversation is unchanged, only its rendering is about to be replayed. */
void tui_clear_transcript(void);
/* Mark the transcript bytes written between these calls as a click target
 * carrying `id`, which must be nonzero. A click inside one makes tui_readline
 * submit "/expand <id>", the same way Escape submits "/rewind": the gesture
 * and the command are one request. Zones are dropped with the transcript.
 * Ids are the caller's; a replay reuses them so a target survives a
 * re-render. */
void tui_zone_begin(u32 id);
void tui_zone_end(void);
/* Keep zone `id` where it is on screen across a re-render: anchor before the
 * clear, restore after the replay. A viewport pinned to the bottom is left
 * there, since the rows a replay added are what the reader asked for. */
void tui_anchor_zone(u32 id);
void tui_restore_anchor(void);
/* One line where the completion popup would be: the answer to a command that
 * opened no popup, retired by the next keystroke. Empty clears it. The
 * transcript is the conversation, so this never lands in it. */
void tui_notice(Str msg);
void tui_write(Str s);
/* Append reasoning output: same transcript, painted muted so a thinking trace
 * reads apart from the reply. */
void tui_write_reason(Str s);
/* Append transcript text under one of the styles a tool block is built from:
 * muted for quoted input and output, yellow for a call's header, green for a
 * result, red for a failure. Style is a recorded byte range, so a write that
 * overflowed the scrollback simply loses it. */
void tui_write_muted(Str s);
void tui_write_tool(Str s);
void tui_write_result(Str s);
void tui_write_error(Str s);
/* Append a user turn: rendered as a padded block with its own background,
 * which is what marks it apart from the agent's own output. */
void tui_write_user(Str s);
/* Append transcript text under one of the styles above; TUI_PLAIN records no
 * style at all. This is how markdown.c paints a rendered reply. */
void tui_write_styled(Str s, TuiStyle style);
/* Cells a transcript row holds, 0 without a fullscreen UI. */
size_t tui_body_cols(void);
/* False on a pipe or in a one-shot run, where output is text, not a view. */
b8 tui_is_fullscreen(void);
/* Flag Esc raises to cancel an in-flight turn (same path as SIGINT). */
void tui_set_interrupt_flag(volatile sig_atomic_t *flag);
void tui_printf(const char *fmt, ...) __attribute__((format(printf,1,2)));
void tui_enter_raw(void);
void tui_exit_raw(void);
void tui_putstr(Str s);
/* Read one submitted line. Escape at an idle composer with nothing to dismiss
 * arms a rewind and the next Escape submits "/rewind", leaving the composed
 * text where it is: the key and the command are the same request. */
b8 tui_readline(const char *prompt, char *buf, size_t cap, size_t *out_n);
/* Replace the composer's text, cursor at its end; ignored without a
 * fullscreen UI. This is how a rewind hands an earlier message back. */
void tui_set_input(Str s);
/* Composer editing while a turn is in flight: keystrokes are accepted, Enter
 * is not. Callers pump tui_poll_input from wherever they wait. */
void tui_set_busy(b8 busy);
void tui_poll_input(void);
i32  tui_input_fd(void);      /* readable-input fd, or -1 when not interactive */

/* ---- markdown ------------------------------------------------------------
 * A reply is Markdown, and the transcript renders it: headings, lists, rules,
 * block quotes and fenced code become shapes, emphasis becomes a style, and
 * the markers themselves are dropped. Rendering is streaming, so a delta is
 * painted as soon as its shape is known and only an unclosed marker waits.
 * `md_end` closes whatever the turn left unterminated.
 *
 * `md_set_raw` sends the text through untouched, which is what /raw toggles.
 * Without a fullscreen UI the text is a reply on stdout rather than a
 * rendering, and is passed through the same way. */
void md_write(Str delta);
void md_end(void);
void md_set_raw(b8 on);
b8   md_raw(void);

/* ---- render.c ------------------------------------------------------------ */
/* Write one tool call, and later its result, into the transcript in the shape
 * a reader wants: the JSON arguments are parsed in `scratch`, which is rewound
 * before returning, and unparsable ones fall back to the raw text. `result`
 * is the tool's own output, an "ERROR: " prefix included.
 * `id` marks the block as a click target: the truncation tail it writes folds
 * and unfolds it, and `expanded` is the state that click left behind, which
 * lifts this block's caps the way /verbose lifts every block's. */
void render_tool_call(Str name, Str args, Arena *scratch, u32 id, b8 expanded);
/* The header of a '!' shell run, the same shape a tool call gets. Its output
 * follows through render_tool_result under the name "shell". */
void render_shell_call(Str cmd, u32 id, b8 expanded);
void render_tool_result(Str name, Str result, u32 id, b8 expanded);
/* Verbose rendering shows every line of a call's input and its result, with no
 * "... N more lines" tail and no per-line clip. Off by default: a tool that
 * read a thousand lines would otherwise be the whole scrollback. */
void render_set_verbose(b8 on);
b8   render_verbose(void);

#endif /* YOKE_H */
