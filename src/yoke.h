/* yoke.h: umbrella header. Every module includes this. */
#ifndef YOKE_H
#define YOKE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>
#include <signal.h>

#include "highlight_protocol.h"

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
/* Both arenas are static storage, so this is address space rather than
 * startup cost, sized an order of magnitude above the per-turn peak. */
#define YOKE_ARENA_BYTES      (1u << 27)  /* 128 MiB scratch arena            */
#define YOKE_PERSIST_BYTES    (1u << 26)  /* 64  MiB persistent arena         */
/* The rows of a modal screen, which outlive an action that rerenders the
 * transcript from the scratch arena; see choose_settings. */
#define YOKE_SCREEN_BYTES     (1u << 15)  /* 32  KiB modal screen arena       */
#define YOKE_MAX_MESSAGES     4096        /* default; see Config.max_messages   */
/* A reply that reaches this stops mid-sentence, so the default is above what
 * a long answer or a large patch needs rather than at a provider's minimum. */
#define YOKE_MAX_TOKENS       32768       /* default; see Config.max_tokens    */
#define YOKE_MAX_TOOLS        64
#define YOKE_MAX_TOOL_CALLS   1024        /* per turn                          */
#define YOKE_MAX_TOOL_ARGS    8
#define YOKE_MAX_JSON_DEPTH   64          /* nesting a provider may hand us    */
#define YOKE_MAX_PATH         4096        /* longest path a tool will accept   */
#define YOKE_MAX_COMMAND      (1u << 16)  /* longest shell command             */
#define YOKE_MAX_FILE_BYTES   (16u << 20) /* largest file a tool will read     */
/* A tool result is replayed on every later turn, so each cap below makes one
 * call a page rather than a file; the call says where to continue. */
#define YOKE_TOOL_RESULT_BYTES (8u << 10) /* hard cap, including paging notes   */
#define YOKE_READ_LINES       2000        /* lines one read returns by default  */
/* Leave enough room under the result cap for a continuation/status line. */
#define YOKE_READ_BYTES       (YOKE_TOOL_RESULT_BYTES - 256u)
#define YOKE_SHELL_OUT_BYTES  (YOKE_TOOL_RESULT_BYTES - 256u)
#define YOKE_GREP_RESULTS     100         /* matches one grep returns by default*/
#define YOKE_FIND_RESULTS     200         /* paths one find returns by default  */
#define YOKE_GREP_LINE        200         /* of a matched line, what is shown   */
#define YOKE_WALK_DEPTH       32          /* directories a walk descends        */
#define YOKE_WALK_ENTRIES     4096        /* names one directory level holds    */
#define YOKE_WALK_BYTES       (4u << 20)  /* scratch a walk carves for names    */
#define YOKE_MAX_GREP_FILE    (1u << 20)  /* larger files are not searched      */
#define YOKE_MAX_PATCH_FILES  32          /* files one patch call may touch     */
#define YOKE_MAX_PATCH_HUNKS  512         /* hunks one patch call may carry     */
/* A tool result older than this many user turns is replaced on the wire by a
 * line naming what it was; see conv_write_json. */
#define YOKE_ELIDE_TURNS      2
#define YOKE_ELIDE_BYTES      512         /* under this, saying so costs more   */
/* The delay doubles per attempt from Config.retry_delay_ms and stops here. */
#define YOKE_RETRIES          3           /* extra attempts a turn is allowed  */
#define YOKE_RETRY_DELAY_MS   500         /* wait before the first of them     */
#define YOKE_MAX_RETRY_DELAY_MS 8000
#define YOKE_MAX_COMMANDS     32          /* slash commands offered by the TUI */
#define YOKE_LINE_BUF         (1u << 20)  /* 1 MiB input line buffer          */
#define YOKE_RESP_BUF         (1u << 22)  /* 4 MiB response accumulation      */
#define YOKE_MAX_HISTORY      500         /* recallable prompts kept on disk   */
#define YOKE_HISTORY_BYTES    (1u << 20)  /* entry storage carved from persist */
#define YOKE_MAX_HISTORY_LINE (1u << 16)  /* longest prompt worth remembering  */
#define YOKE_MAX_HISTORY_BYTES (8u << 20) /* largest history file we will read */
#define YOKE_MAX_CONFIG_FILES 8           /* XDG config candidates we consider */
#define YOKE_MAX_SETTINGS     512         /* key lines one settings file holds */
#define YOKE_MAX_SETTINGS_BYTES (1u << 20)/* largest settings file we will read */
#define YOKE_MAX_SET_KEYS     8           /* keys one settings_set writes      */
/* Past this yoke refuses to start rather than send a truncated prompt. */
#define YOKE_MAX_PROMPT_FILE  (1u << 16)
#define YOKE_MAX_AGENTS_FILES 8           /* AGENTS.md chain depth we collect  */
#define YOKE_MAX_SESSIONS     64          /* saved sessions the picker offers  */
#define YOKE_MAX_SESSION_BYTES (32u << 20)/* largest session file we will read */
/* A popup shows a handful of rows at a time, so this bounds what it can hold
 * and scroll or search through, not what it can show: a provider that serves
 * hundreds of models is the reason it is not the row count. */
#define YOKE_MAX_POPUP        4096        /* entries the popup can hold        */
#define YOKE_MAX_MODELS       YOKE_MAX_POPUP /* models the /model picker offers */
#define YOKE_MAX_ENDPOINTS    32          /* providers /provider can hold      */
#define YOKE_MAX_ENDPOINT_NAME 64
#define YOKE_MAX_URL          512
#define YOKE_MAX_MODEL_NAME   128
#define YOKE_MAX_API_KEY      512
#define YOKE_MAX_REASONING_LIST 1024
#define YOKE_MAX_REASONING_TEMPLATE (16u << 10)
#define YOKE_MAX_MODEL_BYTES  (1u << 20)  /* largest /models reply we will read */
#define YOKE_STATUS_FIELDS    9

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
/* Contents are uninitialised. */
#define arena_new(a, T, n) ((T *)arena_alloc_array((a), (n), sizeof(T), alignof(T)))

/* ---- string view + builder ---------------------------------------------- */
typedef struct { const char *p; size_t n; } Str;
#define STR(lit) ((Str){ (lit), sizeof(lit) - 1 })

Str     str_c(const char *z);
Str     str_dup(Arena *a, Str s);              /* copies into arena         */
b8    str_eq(Str a, Str b);
b8    str_starts(Str s, Str prefix);
Str     str_trim(Str s);
Str     str_take(Str s, size_t n);
Str     str_drop(Str s, size_t n);
/* `line` excludes the newline, `off` advances past it. */
b8      str_line(Str s, size_t *off, Str *line);
size_t  str_lines(Str s);
/* At most `max` bytes of `s`, never cutting a UTF-8 sequence in half. */
Str     str_clip_utf8(Str s, size_t max);
i64    str_int(Str s, b8 *ok);
/* FNV-1a: enough to tell two strings apart across runs, not enough to name
 * either, which is what a session slug and a telemetry field need. */
u64     str_hash64(Str s);

/* Growable char buffer doubling into an arena. `oom` latches when a growth
 * failed: every later write is dropped, and callers check buf_ok() before
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
Str     buf_finish(Buf *b);                    /* nul-terminates            */

/* ---- files ---------------------------------------------------------------
 * The one reader every file yoke owns goes through, so a size that comes from
 * the filesystem is checked in one place rather than at each caller.
 */
typedef enum {
    FILE_OK, FILE_MISSING, FILE_NOT_REGULAR, FILE_TOO_LARGE,
    FILE_NO_MEMORY, FILE_UNREADABLE
} FileStatus;

/* Reads `path` into `a`, nul-terminated past `out->n`. A file over `max` is
 * FILE_TOO_LARGE and reads nothing; a nonzero `head` caps how much of one
 * within the limit is read, which is what a preview asks for. `size_out` is
 * optional and carries the file's own size, for a message about it. `out` is
 * empty for every status but FILE_OK. */
FileStatus file_read(Arena *a, const char *path, size_t max, size_t head,
                     Str *out, u64 *size_out);

/* ---- logging ------------------------------------------------------------ */
enum { YOKE_LOG_DEBUG, YOKE_LOG_INFO, YOKE_LOG_WARN, YOKE_LOG_ERROR };
void    yoke_log(i32 level, const char *fmt, ...) __attribute__((format(printf,2,3)));
void    yoke_log_set_level(i32 level);
/* Raw stderr would paint over the frame, so the TUI redirects log lines into
 * the transcript while it is up. NULL restores plain stderr. */
typedef void (*YokeLogSink)(i32 level, Str msg, void *ud);
void    yoke_log_set_sink(YokeLogSink sink, void *ud);

/* ---- telemetry -----------------------------------------------------------
 * An anonymized record of a session, appended as JSON lines to that
 * conversation's own $XDG_STATE_HOME/yoke/telemetry/<cwd>/<timestamp>.jsonl
 * while /telemetry is on: the file is named after the session file and is
 * rebound with it, so /clear starts a record and /resume continues one.
 *
 * The record is the shape of a session, never its content: a message is a
 * byte and a line count, a tool call is its name and the keys of its
 * arguments, the working directory is a hash. A string field is for text yoke
 * formats itself; user and model text goes through tel_shape, which keeps no
 * bytes of it. With telemetry off every call below is a no-op.
 */
/* One line, sized for the widest event: a network transfer is two dozen
 * counters and curl's own timings. */
typedef struct { char buf[1024]; size_t n; b8 full, live; } TelEvent;

void telemetry_init(Arena *scratch);
b8   telemetry_on(void);
/* Records into the file named after `session_path`, under a directory named
 * after its parent, taking with it whatever was recorded while no session had
 * a file. Called by session.c when a session file is written or resumed. */
void telemetry_bind(Str session_path);
/* The conversation is over: what follows waits for the next one's file. */
void telemetry_detach(void);
/* Writes out whatever is still waiting, as a record named after the run. */
void telemetry_close(void);
/* Writes the session event: what the record needs to be read on its own.
 * Called on the first event of a file, since a file that starts mid-run
 * would otherwise say nothing about the run. */
typedef void (*TelHeader)(void *ud);
void telemetry_set_header(TelHeader fn, void *ud);
/* False when no state directory resolves or the setting could not be
 * written, leaving it unchanged. */
b8   telemetry_set(b8 on, Arena *scratch);
/* The file being recorded to. Empty when no state directory resolves. */
Str  telemetry_file(void);
/* Mirror of a yoke_log line, so the diagnostics sit beside the events they
 * explain. Called by yoke_log. */
void telemetry_log(i32 level, Str msg);

void tel_open(TelEvent *e, const char *ev);
void tel_int(TelEvent *e, const char *key, i64 v);
void tel_bool(TelEvent *e, const char *key, b8 v);
/* Escaped and clipped. Never pass text a user or a model wrote. */
void tel_str(TelEvent *e, const char *key, Str v);
/* "<key>_bytes" and "<key>_lines": what a message looked like, not what it
 * said. */
void tel_shape(TelEvent *e, const char *key, Str text);
/* The top-level keys of a tool call's JSON arguments, comma-separated; the
 * values carry the path or the command, so none is recorded. `scratch` is
 * rewound before returning. */
void tel_arg_keys(TelEvent *e, const char *key, Str args, Arena *scratch);
/* A stable 64-bit hash: enough to tell two directories apart across runs,
 * not enough to name either. */
void tel_hash_field(TelEvent *e, const char *key, Str v);
void tel_send(TelEvent *e);

/* ---- time --------------------------------------------------------------- */
f64  yoke_now_seconds(void);   /* monotonic                                */

/* ---- JSON --------------------------------------------------------------- */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct JVal JVal;
struct JVal {
    Str   key;        /* set for object members                            */
    JType type;
    union {
        b8    b;
        f64  n;
        Str     s;
        struct { JVal *items; size_t n; } arr;
        struct { JVal *head; } obj;                     /* linked members */
    } u;
    JVal  *next;     /* next sibling in object                                */
};

typedef struct { Arena *a; const char *src; size_t pos, len; i32 depth; b8 oom; } JParser;

JVal   *json_parse(Arena *a, Str s);            /* NULL on error             */
void    json_write(Buf *b, const JVal *v);
const JVal *json_get(const JVal *obj, Str key);
const JVal *json_at(const JVal *arr, size_t i);
/* Empty when the member is absent or is not a string, which is the same
 * answer to the caller: the field it asked for is not there. */
Str    json_str(const JVal *obj, Str key);
/* False unless the member is present and true. */
b8     json_bool(const JVal *obj, Str key);

/* ---- XDG base directories ------------------------------------------------
 * Every file yoke owns is resolved here; none sits directly in $HOME. A
 * relative XDG_* value is invalid and ignored, as the spec requires.
 */
typedef enum { YOKE_DIR_CONFIG, YOKE_DIR_DATA, YOKE_DIR_STATE, YOKE_DIR_CACHE } YokeDir;

/* Absolute "<base>/yoke" path, empty when no base resolves. */
Str    paths_dir(YokeDir kind, Arena *a);
Str    paths_file(YokeDir kind, Str name, Arena *a);
b8     paths_ensure_dir(Str dir);    /* mkdir -p, mode 0700                  */
/* Candidates for a config file, lowest precedence first. */
size_t paths_config_files(Str name, Arena *a, Str *out, size_t max);

/* ---- settings files ------------------------------------------------------
 * One syntax for every setting yoke owns: "key = value" lines grouped under
 * optional "[section]" headers, '#' comments, values unquoted to end of line.
 * The config file carries the user's settings and their providers, the state
 * file what the UI last chose, the credentials file the keys alone.
 *
 * Parsed Strs point into the arena copy of the file, so they live as long as
 * the arena they were read into.
 */
typedef struct {
    Str    section[YOKE_MAX_SETTINGS];   /* empty above the first header    */
    Str    key[YOKE_MAX_SETTINGS];
    Str    val[YOKE_MAX_SETTINGS];
    size_t n;
} Settings;

/* False when the file is missing, empty or past YOKE_MAX_SETTINGS_BYTES. */
b8     settings_load(Settings *s, Str path, Arena *a);
/* Empty when the key is unset; a key repeated in one file reads as the last
 * assignment wins. */
Str    settings_get(const Settings *s, Str section, Str key);
/* Distinct section names starting with `prefix`, in file order. */
size_t settings_sections(const Settings *s, Str prefix, Str *out, size_t max);
/* Upserts `n` keys in `section` (empty names the head of the file), keeping
 * every other line, its order and its comments; an empty value removes the
 * key. `mode` creates a new file; an existing one keeps its own. Written
 * through a temporary file and renamed, so a failed write leaves the previous
 * file. `scratch` is rewound before returning. */
b8     settings_set(Str path, Str section, const Str *keys, const Str *vals,
                   size_t n, u32 mode, Arena *scratch);
b8     settings_set_one(Str path, Str section, Str key, Str val, u32 mode,
                       Arena *scratch);
/* Removes one named section whole, preserving every byte outside it. Missing
 * files and sections are already absent and therefore succeed. */
b8     settings_remove_section(Str path, Str section, Arena *scratch);

/* $XDG_STATE_HOME/yoke/state: the choices the UI remembers between runs. */
Str    state_get(Str key, Arena *out, Arena *scratch);
b8     state_set(Str key, Str val, Arena *scratch);
/* Folds an older yoke's one-file-per-key state into that file and removes
 * the files. Call before anything reads the state. */
void   state_sweep(Arena *scratch);

/* Presentation choices remembered by /settings and /statusline. Values are
 * loaded with config files below remembered UI state and environment above
 * it. The structure owns no strings. */
typedef struct {
    b8 verbose_tools;
    b8 raw_markdown;
    b8 show_ignored;
    b8 show_instructions;
    b8 justify;
    u64 status_fields;
} UiPrefs;

void   ui_prefs_load(UiPrefs *p, Arena *scratch);
b8     ui_pref_set(Str key, Str val, Arena *scratch);

/* ---- prompt history ------------------------------------------------------
 * A ring of past prompts, mirrored to $XDG_STATE_HOME/yoke/history as they
 * are submitted. `cursor` is the browse position; cursor == n is the live
 * draft the composer restores on the way back down.
 *
 * Entries need an arena of their own because /clear rewinds the session's. A
 * full one is compacted in place: a bump allocator hands entries out in
 * ascending address order, so the move never overlaps forward.
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

/* ---- provider API shapes -------------------------------------------------
 * The two wire formats an endpoint may speak. They differ in the request
 * path, the header the key rides in and the shape of a message, and in
 * nothing above provider.c: a turn, a tool call and a transcript are the
 * same either way.
 */
typedef enum { API_OPENAI = 0, API_ANTHROPIC } ApiKind;

/* The name the config file writes and reads back. An unknown name is
 * API_OPENAI, since that is what an endpoint whose `api` key was mistyped
 * most likely speaks. */
ApiKind api_from_str(Str s);
Str     api_name(ApiKind k);

/* ---- endpoints -----------------------------------------------------------
 * The providers /provider creates and switches between: a name, a base URL,
 * the API that URL speaks and the model last used against it. Each is a
 * "[provider <name>]" section of the config file, and its key alone lives
 * under the same section of $XDG_STATE_HOME/yoke/credentials, so a shared
 * configuration cannot carry a secret; the state file's `provider` key names
 * the active one. An oversized field is dropped on load rather than
 * truncated, since a cut URL names a different service.
 */
typedef struct {
    Str     name[YOKE_MAX_ENDPOINTS];
    Str     base_url[YOKE_MAX_ENDPOINTS];
    Str     model[YOKE_MAX_ENDPOINTS];   /* empty when none was chosen yet  */
    Str     reasoning_efforts[YOKE_MAX_ENDPOINTS];
    Str     thinking_budgets[YOKE_MAX_ENDPOINTS];
    Str     reasoning_effort[YOKE_MAX_ENDPOINTS];
    Str     thinking_budget[YOKE_MAX_ENDPOINTS];
    Str     reasoning_template[YOKE_MAX_ENDPOINTS];
    ApiKind api[YOKE_MAX_ENDPOINTS];
    size_t  n;
} Endpoints;

#define ENDPOINT_NONE ((size_t)-1)

/* Every Str lands in `a` and lives as long as it does. */
size_t endpoints_load(Endpoints *e, Arena *a);
size_t endpoints_find(const Endpoints *e, Str name);
/* False when the store is full or a field is past its cap. */
b8     endpoints_put(Endpoints *e, Str name, Str base_url, Str model,
                     ApiKind api, Str efforts, Str budgets, Str effort,
                     Str budget, Str templ, Arena *a);
/* Writes one endpoint's section, leaving the rest of the config file alone. */
b8     endpoints_save_one(Str name, Str base_url, Str model, ApiKind api,
                          Str efforts, Str budgets, Str effort, Str budget,
                          Str templ, Arena *scratch);
/* Where /model writes while a provider is active. */
b8     endpoints_remember_model(Str name, Str model, Arena *scratch);
/* The key stored for `name`, allocated in `out`. Empty when there is none,
 * and empty with `err` filled in when the credentials file is readable by
 * anyone but its owner: that is a key to rotate rather than one to load. */
Str    endpoints_key(Str name, Arena *out, Arena *scratch,
                     char *err, size_t err_cap);
b8     endpoints_set_key(Str name, Str key, Arena *scratch,
                         char *err, size_t err_cap);
/* Removes the provider's config and credential sections. */
b8     endpoints_delete(Str name, Arena *scratch, char *err, size_t err_cap);
Str    endpoints_active(Arena *a);
/* An empty name forgets the active provider. */
b8     endpoints_remember_active(Str name, Arena *scratch);

/* ---- agent modes ---------------------------------------------------------
 * Build carries the work out; Plan reads and proposes, handing over through
 * the submit_plan tool. The mode decides which system prompt slot 0 holds
 * and which tools the registry offers, so "read-only" is a property of the
 * registry rather than a request made in the prompt.
 */
typedef enum { MODE_BUILD = 0, MODE_PLAN } AgentMode;

/* The effective prompt remains one system message. These copies explain where
 * it came from when the session chooses to show instructions; they never go
 * through Conv or a provider request. */
typedef struct {
    Str primary, primary_label, primary_path;
    Str agents[YOKE_MAX_AGENTS_FILES];
    Str agent_paths[YOKE_MAX_AGENTS_FILES];
    size_t n_agents;
} PromptSources;

/* ---- config ------------------------------------------------------------- */
typedef struct {
    Str base_url;     /* e.g. https://api.openai.com/v1                    */
    Str model;
    Str api_key;
    ApiKind api;      /* the wire format base_url speaks                   */
    Str provider;     /* active endpoint name; empty when none is selected */
    Str reasoning_efforts, thinking_budgets;
    Str reasoning_effort, thinking_budget;
    Str reasoning_template;
    /* A run with neither this nor a key has nothing to talk to, and asks for
     * a provider instead of starting a conversation. */
    b8  base_url_set;
    Str system_prompt; /* Only --system and YOKE_SYSTEM_PROMPT set this. */
    Str plan_prompt;   /* Plan mode's; built at startup, never configured. */
    PromptSources system_sources, plan_sources;
    AgentMode mode;
    i32  max_tokens;
    /* Configurable so the full-history path is reachable in a test without
     * streaming four thousand messages. */
    size_t max_messages;
    b8 stream;
    /* Further attempts a turn that reached nothing may make, and the wait
     * before the first one. */
    i32 retries;
    i32 retry_delay_ms;
    /* Tools to turn off before the first turn, comma separated. Applied once
     * the registry exists, since config_load runs before tools_init. */
    Str disable_tools;
    /* Commands may replace these fields after the conversation rewind mark.
     * Their owned copies live here so /clear cannot reclaim them. */
    char owned_base_url[YOKE_MAX_URL + 1];
    char owned_model[YOKE_MAX_MODEL_NAME + 1];
    char owned_api_key[YOKE_MAX_API_KEY + 1];
    char owned_provider[YOKE_MAX_ENDPOINT_NAME + 1];
    char owned_reasoning_efforts[YOKE_MAX_REASONING_LIST + 1];
    char owned_thinking_budgets[YOKE_MAX_REASONING_LIST + 1];
    char owned_reasoning_effort[YOKE_MAX_REASONING_LIST + 1];
    char owned_thinking_budget[YOKE_MAX_REASONING_LIST + 1];
    char owned_reasoning_template[YOKE_MAX_REASONING_TEMPLATE + 1];
} Config;

b8    config_load(Config *c, Arena *persist, Arena *scratch);
/* Writes the state file's `model` key, which config_load applies above the
 * config files and below YOKE_MODEL. */
b8    config_remember_model(Str model, Arena *scratch);
/* Runtime choices are copied into Config itself and survive /clear. */
b8    config_set_model(Config *c, Str model);
b8    config_set_endpoint(Config *c, Str name, Str base_url, Str model,
                          ApiKind api, Str key, Str efforts, Str budgets,
                          Str effort, Str budget, Str templ);
b8    config_set_reasoning(Config *c, b8 effort, Str value);


/* ---- command line ------------------------------------------------------- */
/* Every Str points into argv, so nothing is copied and nothing is freed. */
typedef struct {
    Str base_url, model, api_key, system_prompt;
    Str api;           /* "openai" or "anthropic"; empty leaves the config */
    Str disable_tools; /* comma separated names; replaces the configured set */
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
void      cli_apply(const CliOpts *o, Config *c);

/* ---- HTTP (libcurl) ----------------------------------------------------- */
typedef struct {
    const char *base_url;
    const char *api_key;
    /* Which path the request goes to and which header carries the key. */
    ApiKind api;
    /* Each accumulated SSE line. Return false to abort the stream. */
    b8 (*on_line)(Str line, void *ud);
    void *ud;
    /* Where a streamed line is accumulated. An event carries as much as the
     * provider chose to send, so the buffer grows instead of clipping: half a
     * delta is not JSON, and the reply behind it would be lost without a
     * word. Required whenever on_line is set. */
    Arena *line_arena;
    /* Non-streaming: the whole body lands here and on_line is never called. */
    Buf *body_out;
    const char *body;     /* nul-terminated JSON request                    */
    const volatile sig_atomic_t *interrupt_flag;
    /* The transfer waits on curl's sockets and `idle_fd` together, so the UI
     * stays alive without threads. on_idle runs after every wait and must
     * not block. */
    i32   idle_fd;        /* -1 disables the extra poll fd                  */
    void (*on_idle)(void *ud);
    void *idle_ud;
    /* Optional: curl's own message for a transport failure. */
    char  *fail_out;
    size_t fail_cap;
} HttpReq;

/* POST the body to the API's completion path (/chat/completions, or
 * /messages for API_ANTHROPIC), delivering the reply through on_line or
 * body_out. 0 on success, a negative HTTP status for a refused request, 3
 * for an interrupt, other positive values for a transport failure. */
i32     http_post(const HttpReq *r);
/* GET base_url + path, appending the whole body to `out`, with the statuses
 * above. Blocking: callers fetch a short document between turns. */
i32     http_get(const char *base_url, const char *path, const char *api_key,
                ApiKind api, Buf *out);

/* ---- tools (SoA registry) ----------------------------------------------- */
typedef b8 (*ToolRun)(Str args_json, Arena *scratch, Buf *out,
                      char *err, size_t err_cap);

/* A bit per mode in ToolRegistry.modes. */
#define TOOL_IN_BUILD 1u
#define TOOL_IN_PLAN  2u
/* Answered by the agent loop rather than run, so the mode owns it: it is
 * never offered as a toggle and never disabled. */
#define TOOL_FIXED    4u

typedef struct {
    Str     *name;        /* [YOKE_MAX_TOOLS]                               */
    Str     *desc;        /* [YOKE_MAX_TOOLS] what the model is told         */
    /* What a row of the settings screen says: one line that fits beside the
     * name, since the model's description is written for a model. */
    Str     *brief;       /* [YOKE_MAX_TOOLS]                               */
    Str     *schema;      /* [YOKE_MAX_TOOLS] JSON schema fragment (object) */
    ToolRun *run;         /* [YOKE_MAX_TOOLS]                               */
    u8      *modes;       /* [YOKE_MAX_TOOLS] TOOL_IN_* bits                */
    b8      *off;         /* [YOKE_MAX_TOOLS] turned off by the user        */
    size_t   n;
} ToolRegistry;

/* Ids are indices into the registry. */
#define TOOL_NONE ((size_t)-1)

void        tools_init(ToolRegistry *r, Arena *persist);
/* The mode tools_write_schemas offers and tools_run enforces. */
void        tools_set_mode(AgentMode mode);
/* False for a tool the mode does not offer and for one the user turned off,
 * which is what withholds it from the schemas and the prompt listing. */
b8          tools_available(const ToolRegistry *r, size_t id, AgentMode mode);
size_t      tools_find(const ToolRegistry *r, Str name);
/* Whether the user may turn `id` off: everything but a TOOL_FIXED entry. */
b8          tools_can_disable(const ToolRegistry *r, size_t id);
b8          tools_disabled(const ToolRegistry *r, size_t id);
void        tools_set_disabled(ToolRegistry *r, size_t id, b8 off);
/* Turns off every tool named in a comma or space separated list, which is
 * what disable_tools and --disable-tools carry. False with `err` filled in
 * on a name no tool answers to or one that cannot be disabled: a typo in a
 * list whose point is that bash cannot run is not worth ignoring. */
b8          tools_disable_list(ToolRegistry *r, Str names,
                               char *err, size_t err_cap);
b8          tools_run(const ToolRegistry *r, size_t id, Str args,
                      Arena *scratch, Buf *out, char *err, size_t err_cap);
/* The registry as the API declares tools: an array of OpenAI "function"
 * wrappers, or of Anthropic entries carrying an "input_schema". */
void        tools_write_schemas(Buf *b, const ToolRegistry *r, ApiKind api);
/* Run `cmd` through the shell, appending its output to `out` followed by a
 * bracketed status line ("[exit 0]") that render.c reads back. Only the last
 * YOKE_SHELL_OUT_BYTES are kept, behind a line saying how much was dropped.
 * False with `err` filled in when the command is longer than
 * YOKE_MAX_COMMAND or the shell could not be started; a command is never
 * clamped to fit, since a truncated one is a different program. */
b8          shell_capture(Str cmd, Buf *out, char *err, size_t err_cap);
/* Pumped while a command runs, so a slow one keeps the UI live the way an
 * in-flight request does; unset by default, since a tool is not the TUI's. */
void        shell_set_idle(void (*fn)(void *ud), void *ud);

/* ---- prompt ------------------------------------------------------------- */
/* The system prompt, placeholders expanded. `configured` is what --system or
 * YOKE_SYSTEM_PROMPT set, unset to take .yoke/SYSTEM.md, the global
 * SYSTEM.md or the built-in template, in that order. Returned in `persist`,
 * falling back to the unexpanded template when it cannot take the result.
 * Empty with `err` set when a SYSTEM.md is larger than YOKE_MAX_PROMPT_FILE.
 *
 * Every AGENTS.md from the working directory up to the root is appended to
 * whichever prompt won, outermost first and verbatim: it is a document about
 * the project rather than a template. */
Str   prompt_build(const ToolRegistry *tools, Str configured, Arena *persist,
                   Arena *scratch, PromptSources *sources, char *err,
                   size_t err_cap);
/* The plan-mode prompt, resolved the same way from .yoke/PLAN.md, the global
 * PLAN.md or the built-in template, with {tools} listing only what plan mode
 * offers. Nothing configures it: --system describes Build mode. */
Str   prompt_build_plan(const ToolRegistry *tools, Arena *persist,
                        Arena *scratch, PromptSources *sources, char *err,
                        size_t err_cap);

/* ---- conversation (SoA) ------------------------------------------------- */
typedef enum { M_SYSTEM = 0, M_USER, M_ASSISTANT, M_TOOL } MRole;

/* Returned by every conv_* append when the conversation is full. */
#define CONV_NONE ((size_t)-1)

typedef struct {
    MRole *role;          /* [cap]                                       */
    Str   *text;          /* [cap] prose, tool result or call arguments  */
    Str   *tool_name;     /* [cap]                                       */
    Str   *tool_call_id;  /* [cap]                                       */
    Str   *shell_out;     /* [cap] what a '!' run printed                */
    b8  *has_tool_call; /* [cap]                                       */
    b8  *expanded;      /* [cap] this block's transcript caps are lifted */
    /* [cap] milliseconds the work behind a slot took, 0 when it took none
     * that was measured. A tool result and a '!' run carry one, which is
     * what lets a replay render the time a live turn showed. */
    u32   *ms;
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
 * since it is one turn the user took. */
size_t  conv_add_shell(Conv *c, Str cmd, Str out);
b8      conv_is_shell(const Conv *c, size_t i);
b8      conv_is_call(const Conv *c, size_t i);
size_t  conv_room(const Conv *c);
/* The messages array of an OpenAI request. The system prompt is slot 0's
 * message like any other. */
void    conv_write_json(Buf *b, const Conv *c, const ToolRegistry *reg);
/* The same conversation as Anthropic messages: content blocks rather than
 * flat text, tool results carried by the user, and consecutive slots of one
 * role merged into a single message, which is what that API accepts. The
 * system prompt is not a message there, so it is written by the caller and
 * skipped here. */
void    conv_write_json_anthropic(Buf *b, const Conv *c);

/* ---- sessions ------------------------------------------------------------
 * The conversation as it happened, one JSON object per line under
 * $XDG_DATA_HOME/yoke/sessions/<cwd>/<timestamp>.jsonl, keyed by the
 * directory yoke was launched in so browsing from one project never surfaces
 * another's.
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
/* Continue in a new file holding the conversation whole, leaving the one it
 * was appending to as it is. */
b8     session_fork(Session *s, const Conv *c);
size_t session_list(const Session *s, Arena *a, SessionList *out, size_t max);
/* Reading is separate from replaying because replaying rewinds the live
 * conversation, so a file that cannot be read has to be known before
 * anything is thrown away. `session_read` returns the raw contents in
 * `scratch` (empty when unreadable); `session_apply` replays them into a
 * conversation the caller has rewound to its system prompt and continues
 * appending to that file. False means the conversation filled up. */
Str    session_read(Str path, Arena *scratch);
b8     session_apply(Session *s, Str src, Str path, Str name, Conv *c,
                     Arena *persist, Arena *scratch);

/* ---- provider ----------------------------------------------------------- */
typedef struct {
    const Config      *cfg;
    const ToolRegistry*tools;
    Conv              *conv;
    Arena             *persist;   /* message storage                           */
    Arena             *scratch;   /* reset each turn                           */
    void (*on_text)(Str delta, void *ud);
    /* "reasoning_content" or "reasoning": displayed as the turn streams,
     * never appended to the conversation, since a provider rejects a
     * thinking trace it did not produce itself. */
    void (*on_reason)(Str delta, void *ud);
    void (*on_tool_call)(i32 index, Str id, Str name, Str args_delta, void *ud);
    /* The request's usage as it is heard: the mock and most providers send it
     * once, on the last stream event, so `total` is the context the turn is
     * being charged for. Fired from inside the request wait, so it reaches
     * the status line even when the turn is interrupted before it ends. */
    void (*on_usage)(size_t total, void *ud);
    /* A request that produced nothing is about to be sent again. `attempt`
     * is 1-based over `attempts`, and `reason` is yoke's own wording: an
     * HTTP status or curl's catalogue string, never a URL. */
    void (*on_retry)(i32 attempt, i32 attempts, i32 delay_ms, Str reason,
                     void *ud);
    void *ud;
    /* Pumped while the request is in flight; see HttpReq.on_idle. */
    void (*on_idle)(void *ud);
    i32   idle_fd;                /* -1 when there is nothing to watch         */
    const volatile sig_atomic_t *interrupt_flag;
    size_t prompt_tokens;
    size_t completion_tokens;
    size_t total_tokens;
    b8 usage_valid;
} Provider;

/* Run one completion turn, appending the assistant message and its tool
 * calls to conv. Returns the number of tool calls, or -1 with `err` set. */
i32     provider_run(Provider *p, char *err, size_t err_cap);

/* Model ids from GET <base_url>/models, in the order the endpoint serves
 * them, allocated in `scratch`. Zero with `err` set when it could not be
 * read. */
size_t  provider_models(const Config *cfg, Arena *scratch, Str *out, size_t max,
                        char *err, size_t err_cap);

/* ---- TUI --------------------------------------------------------------- */
/* A block style claims whole rows (a wrapped continuation is painted like
 * its first row); an inline one applies to the bytes it covers. */
typedef enum {
    TUI_PLAIN = 0, TUI_HEADING, TUI_CODE, TUI_QUOTE,   /* block */
    TUI_BOLD, TUI_EMPH, TUI_MONO, TUI_MARKER, TUI_STRIKE /* inline */
} TuiStyle;

typedef enum {
    YHL_HINT_MARKDOWN_ALIAS = YHL_HINT_ALIAS,
    YHL_HINT_PATH = YHL_HINT_FILENAME,
} YhlHintKind;

typedef struct { u32 start, end; u8 semantic; } YhlRun;
typedef struct { YhlRun run[YHL_RUN_MAX]; size_t n; } YhlResult;

/* Resolves the optional companion without starting it. Requests lazily start
 * one persistent process and return false for every plain-text fallback. */
void highlight_init(const char *argv0);
b8   highlight_request(YhlHintKind kind, Str hint, Str source,
                       YhlResult *result);
void highlight_close(void);
/* A slash command the completion popup offers. The table is owned by the
 * caller and only read here. */
typedef struct { Str name; Str desc; } TuiCmd;
/* A byte range of the matching row's `desc`, painted as the chosen one of the
 * options that row lists. A zero `n` is a description rather than options.
 * Rows and marks are parallel arrays; the caller owns both. */
typedef struct { size_t off, n; } TuiMark;
void tui_set_commands(const TuiCmd *cmds, size_t n);
/* A second name a command answers to. It is a way to find the command rather
 * than a command of its own: the popup lists the entry it stands for, and
 * accepting one leaves that name in the composer. */
typedef struct { Str alias; Str name; } TuiAlias;
void tui_set_aliases(const TuiAlias *aliases, size_t n);
/* Which end of a picker's list the selection opens on and returns to after a
 * search: a list ordered like the transcript ends at the entry nearest the
 * composer. */
typedef enum { TUI_PICK_FIRST = 0, TUI_PICK_LAST } TuiPickAnchor;

typedef enum {
    TUI_STATUS_STATE, TUI_STATUS_MODEL, TUI_STATUS_REASONING,
    TUI_STATUS_THINKING, TUI_STATUS_MODE, TUI_STATUS_PROVIDER,
    TUI_STATUS_CWD, TUI_STATUS_CONTEXT, TUI_STATUS_COPY, TUI_STATUS_N
} TuiStatusItem;
/* `start` for a list that recommends none of its entries, which opens on the
 * end `anchor` names. */
#define TUI_PICK_NONE ((size_t)-1)
/* Modal picker: the completion popup over a caller-owned list. `title` names
 * it in the status line, Enter chooses (index in *out), Esc/Ctrl-C cancels.
 * `start` opens the selection on one entry, which is how a list carrying a
 * recommendation offers it without reordering itself. Past ten entries typing
 * filters the list by literal substring, leaving the composer's own text
 * untouched. */
b8 tui_pick(Str title, const TuiCmd *items, size_t n, TuiPickAnchor anchor,
            size_t start, size_t *out);
/* As tui_pick, with `search_n` excluding fixed action rows from the length
 * that decides whether keyboard search opens. */
b8 tui_pick_search_count(Str title, const TuiCmd *items, size_t n,
                         size_t search_n, TuiPickAnchor anchor, size_t start,
                         size_t *out);
/* The settings screen: the same list, read rather than chosen from. Space,
 * Enter and Right act on the selected row forwards, Left backwards, Escape
 * closes, and typing narrows the rows by fuzzy match.
 *
 * A change is applied without leaving the screen, which is why the caller
 * hands over the way to rebuild its rows rather than reopening: a popup that
 * collapsed between two frames is a screen that blinks. `build` fills `rows`
 * and `marks` and returns how many rows it wrote, at most `max`; `act`
 * changes the setting row `row` names, forward for a positive `delta` and
 * back for a negative one. The caller owns the arrays and keeps them alive
 * for the call; both hooks are passed `ud`. `build` must not write the rows
 * into an arena `act` resets, since the rows outlive every call. */
typedef struct {
    TuiCmd  *rows;
    TuiMark *marks;                          /* optional, NULL for none    */
    size_t   max;
    size_t (*build)(void *ud);
    void   (*act)(void *ud, size_t row, i32 delta);
    void    *ud;
} TuiSettings;
void tui_settings(Str title, const TuiSettings *set);
/* The same screen without taking the keyboard, for a turn that is streaming:
 * it is driven by tui_poll_input and closes when the user closes it, or on
 * the next prompt, which reads for it. False when it could not open, which
 * includes a screen already being up. The rows, `marks` and `ud` must stay
 * alive until it closes, so they outlive the caller's frame; `set` itself is
 * copied. */
b8 tui_settings_open(Str title, const TuiSettings *set);
/* Read-only modal rows. Enter, Escape, Ctrl-C or Ctrl-D closes the page; the
 * caller keeps ownership of every string for the duration of the call. */
void tui_info(Str title, const TuiCmd *rows, size_t n);
/* The read-only page opened the way tui_settings_open opens a screen. */
b8 tui_info_open(Str title, const TuiCmd *rows, size_t n);
/* Whether a screen is up, modal or not. */
b8 tui_screen_open(void);
/* Modal one-line question, answered in the composer with `question` in the
 * notice row. `secret` echoes the answer as dots and keeps it out of the
 * prompt history and the transcript. False when it was cancelled, the answer
 * was empty, or there is no fullscreen UI; the composer's own text is
 * restored on the way out. */
b8 tui_ask(Str question, b8 secret, char *out, size_t cap);
/* Edits the existing value in `inout`; Enter may accept an empty result. */
b8 tui_ask_edit(Str question, b8 allow_empty, char *inout, size_t cap);
/* The '@' picker hides what the project's .gitignore and .ignore exclude,
 * and always hides .git; this offers them anyway. Off by default. A path
 * typed by hand is unaffected either way. */
void tui_set_show_ignored(b8 on);
b8   tui_show_ignored(void);
/* Rows wrap between words either way; this also widens the gaps of a wrapped
 * prose row until it reaches the right edge. Off by default. */
void tui_set_justify(b8 on);
b8   tui_justify(void);
/* Composer history for Up/Down recall; NULL disables it. */
void tui_set_history(History *h);
/* `plain` forces the line-oriented path and drops the banner even on a tty,
 * which is what a one-shot -p run wants. */
void tui_start(Str model, Str base_url, b8 missing_key, b8 setup,
               size_t tool_count, b8 show_ignored, b8 justify,
               u64 status_fields, AgentMode mode, b8 plain);
/* The strings the status line names; they must outlive the call. */
void tui_set_model(Str model);
void tui_set_mode(AgentMode mode);
void tui_set_provider(Str name);
void tui_set_reasoning(Str effort, Str thinking_budget);
/* Switch to truthful first-run chrome, clearing endpoint fields. */
void tui_set_setup(b8 on);
/* Visibility is remembered by /statusline. */
b8   tui_status_visible(TuiStatusItem item);
void tui_set_status_visible(TuiStatusItem item, b8 visible);
/* What a run with no endpoint says, on the welcome screen and again if a
 * message is submitted anyway. */
#define NO_PROVIDER_HINT \
    STR("no provider yet: type /provider, then \"+ add a provider\"")
void tui_needs_provider(b8 on);
/* Hand `text` to the terminal's clipboard over OSC 52 and acknowledge it on
 * the status line. False for an empty payload or one past the sequence cap,
 * which is refused rather than truncated. */
b8 tui_copy(Str text);
void tui_stop(void);
void tui_set_status(const char *status);
void tui_set_context_tokens(size_t tokens);
void tui_clear(void);
/* Drop the transcript alone, leaving the context counter as it is: the
 * conversation is unchanged, only its rendering is about to be replayed. */
void tui_clear_transcript(void);
/* Mark the transcript bytes written between these calls as a click target
 * carrying nonzero `id`. A click inside one makes tui_readline submit
 * "/expand <id>", the same way Escape submits "/rewind". Zones are dropped
 * with the transcript; a replay reuses the ids so a target survives it. */
void tui_zone_begin(u32 id);
void tui_zone_end(void);
/* Mark the current end of the transcript as landmark `id`, which a re-render
 * can steer the viewport by. Landmarks are dropped with the transcript; a
 * replay reuses the ids so one survives it. */
void tui_pin(u32 id);
/* Keep zone `id`, or the landmark nearest the top of the viewport, where it
 * is on screen across a re-render: anchor before the clear, restore after the
 * replay. A viewport pinned to the bottom is left there. */
void tui_anchor_zone(u32 id);
void tui_anchor_view(void);
void tui_restore_anchor(void);
/* One line where the completion popup would be: the answer to a command that
 * opened no popup, retired by the next keystroke. Empty clears it. */
void tui_notice(Str msg);
/* Open a transcript block; the only place the air above one comes from. A
 * block writes no air of its own, so the gap between any two is one blank
 * row, and a trailing newline a writer did emit is absorbed rather than
 * stacked. */
void tui_block(void);
void tui_write(Str s);
/* Explicit normal foreground inside a row owned by a surrounding style. */
void tui_write_text(Str s);
/* The styles a tool block and a thinking trace are built from: muted for
 * quoted input, output and reasoning, yellow for a call's header, green for a
 * result, red for a failure. Style is a recorded byte range, so a write that
 * overflowed the scrollback loses it. */
void tui_write_muted(Str s);
void tui_write_tool(Str s);
void tui_write_result(Str s);
void tui_write_error(Str s);
/* A padded block with its own background, which is what marks a user turn
 * apart from the agent's output. The split form lets Markdown write styled
 * spans inside the panel; `tui_write_user` is the plain convenience form. */
void tui_user_begin(void);
void tui_user_end(void);
void tui_write_user(Str s);
/* TUI_PLAIN records no style at all. This is how markdown.c paints. */
void tui_write_styled(Str s, TuiStyle style);
/* Cells a transcript row holds, 0 without a fullscreen UI. */
size_t tui_body_cols(void);
/* Terminal cells occupied by UTF-8 text. Invalid bytes take one cell; control
 * and combining code points take none, matching transcript wrapping. */
size_t tui_text_cells(Str s);
/* False on a pipe or in a one-shot run, where output is text, not a view. */
b8 tui_is_fullscreen(void);
/* Syntax overlays exist only in the fullscreen colour presentation. The
 * epoch changes whenever transcript offsets move or are cleared. */
b8     tui_highlight_enabled(void);
size_t tui_transcript_pos(void);
u64    tui_transcript_epoch(void);
void   tui_syntax_add(size_t start, size_t end, u8 semantic);
void   tui_syntax_commit(void);
/* Flag Esc raises to cancel an in-flight turn (same path as SIGINT). */
void tui_set_interrupt_flag(volatile sig_atomic_t *flag);
void tui_printf(const char *fmt, ...) __attribute__((format(printf,1,2)));
/* Read one submitted line. Escape at an idle composer with nothing to dismiss
 * arms a rewind and the next Escape submits "/rewind", leaving the composed
 * text where it is; Shift+Tab submits "/mode" the same way. */
b8 tui_readline(const char *prompt, char *buf, size_t cap, size_t *out_n);
/* Replace the composer's text, cursor at its end; ignored without a
 * fullscreen UI. This is how a rewind hands an earlier message back. */
void tui_set_input(Str s);
/* While a turn is in flight keystrokes are accepted and a message waits in
 * the composer. Callers pump tui_poll_input from wherever they wait. */
void tui_set_busy(b8 busy);
b8   tui_busy(void);
/* Enter mid-turn submits a slash command to `fn`, which returns whether it
 * took it: a refused command is handed back to the composer untouched. The
 * hook runs inside tui_poll_input, so it must not block the wait it is
 * pumping; a screen it opens is driven by later polls. */
void tui_set_busy_command(b8 (*fn)(Str line, void *ud), void *ud);
/* One transient row under the transcript naming the operation in flight, with
 * a spinner and the seconds since it began. It is painted rather than
 * written, so it leaves the transcript untouched and a replay never repeats
 * it. Each label is timed from its own start, and once a wait holds more than
 * the operation on screen the row carries both, since how long this tool has
 * run and how long the turn has are different questions. */
void tui_activity(Str label);
void tui_activity_end(void);
void tui_poll_input(void);
i32  tui_input_fd(void);      /* readable-input fd, or -1 when not interactive */

/* ---- markdown ------------------------------------------------------------
 * Renders a message into the transcript: headings, lists, tables, rules,
 * block quotes and fenced code become shapes, inline markup becomes styles,
 * and its markers are dropped. Rendering is incremental except for bounded
 * lines waiting to prove they start a table; `md_end` closes whatever the
 * message left unterminated.
 *
 * `md_set_raw` sends the text through untouched, and so does the absence of
 * a fullscreen UI, where the output is a message rather than a view. */
void md_write(Str delta);
void md_end(void);
/* Make unstyled Markdown prose muted for a thinking trace. Changing it closes
 * the current Markdown stream, so markers never span reasoning and a reply. */
void md_set_muted(b8 on);
void md_set_raw(b8 on);
b8   md_raw(void);

/* ---- render.c ------------------------------------------------------------ */
/* Write one tool call, and later its result, into the transcript. The JSON
 * arguments are parsed in `scratch`, which is rewound before returning, and
 * unparsable ones fall back to the raw text. `result` is the tool's own
 * output, an "ERROR: " prefix included. `id` marks the block as a click
 * target and `expanded` is the state that click left behind, which lifts
 * this block's caps the way /verbose lifts every block's. */
void render_tool_call(Str name, Str args, Arena *scratch, u32 id, b8 expanded);
/* The header of a '!' shell run, the same shape a tool call gets. Its output
 * follows through render_tool_result under the name "shell". */
void render_shell_call(Str cmd, u32 id, b8 expanded);
/* `ms` is how long the run took; 0 leaves it off, which is what a result
 * replayed from a session written before it was measured carries. */
void render_tool_result(Str name, Str args, Str result, Arena *scratch,
                        u32 id, b8 expanded, u32 ms);
/* A plan mode handover and the question that led to it. The answer to either
 * arrives as a tool result and reads like one. */
void render_plan(Str plan);
void render_question(Str question);
/* Every line of a call's input and result, with no "... N more lines" tail
 * and no per-line clip. Off by default: a tool that read a thousand lines
 * would otherwise be the whole scrollback. */
void render_set_verbose(b8 on);
b8   render_verbose(void);

#endif /* YOKE_H */
