#ifndef AGENT_H
#define AGENT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>
#include <signal.h>
#include <stdio.h>

#include "highlight_protocol.h"


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

#define AGENT_VERSION "0.6.0"

#define AGENT_NAME       "arqan"
#define AGENT_ENV_PREFIX "ARQAN_"


/* Both arenas are static storage, so this is address space rather than
 * startup cost, sized an order of magnitude above the per-turn peak. */
#define AGENT_ARENA_BYTES      (1u << 27)
#define AGENT_PERSIST_BYTES    (1u << 26)
/* The rows of a modal screen, which outlive an action that rerenders the
 * transcript from the scratch arena; see choose_settings. */
#define AGENT_SCREEN_BYTES     (1u << 15)
#define AGENT_MAX_MESSAGES     4096
/* A reply that reaches this stops mid-sentence, so the default is above what
 * a long answer or a large patch needs rather than at a provider's minimum. */
#define AGENT_MAX_TOKENS       32768
#define AGENT_MAX_TOOLS        64
#define AGENT_MAX_TOOL_CALLS   1024
#define AGENT_MAX_TOOL_ARGS    8
#define AGENT_MAX_JSON_DEPTH   64
#define AGENT_MAX_PATH         4096
#define AGENT_MAX_COMMAND      (1u << 16)
#define AGENT_MAX_FILE_BYTES   (16u << 20)
/* Images a turn carries. The per-image cap is the smallest a served API
 * enforces, so an image accepted here is one every provider takes; the side
 * cap refuses the decompression bombs a header can claim. Neither is
 * negotiable at runtime: an image over them is refused, never rescaled,
 * since nothing here decodes pixels. */
#define AGENT_MAX_IMAGE_BYTES  (5u << 20)
#define AGENT_MAX_IMAGE_SIDE   8000u
#define AGENT_MAX_MEDIA        64
#define AGENT_MAX_MEDIA_PER_TURN 4
/* A tool result is replayed on every later turn, so each cap below makes one
 * call a page rather than a file; the call says where to continue. */
#define AGENT_TOOL_RESULT_BYTES (8u << 10)
#define AGENT_READ_LINES       2000

#define AGENT_READ_BYTES       (AGENT_TOOL_RESULT_BYTES - 256u)

#define AGENT_SHELL_OUT_BYTES  (AGENT_TOOL_RESULT_BYTES - 256u \
                                - AGENT_SPILL_NOTE_BYTES)
#define AGENT_GREP_RESULTS     100
#define AGENT_FIND_RESULTS     200
#define AGENT_GREP_LINE        200
#define AGENT_WALK_DEPTH       32
#define AGENT_WALK_ENTRIES     4096
#define AGENT_WALK_BYTES       (4u << 20)
#define AGENT_IGNORE_PATTERNS  512
#define AGENT_IGNORE_BYTES     (1u << 14)
#define AGENT_MAX_GREP_FILE    (1u << 20)
#define AGENT_MAX_PATCH_FILES  32
#define AGENT_MAX_PATCH_HUNKS  512
/* A patch reports every hunk it could not place, not just the first, so one
 * call names every edit that needs fixing; past this many the count stands in
 * for the rest. `AGENT_TOOL_ERR` bounds the whole report. */
#define AGENT_MAX_PATCH_NOTES  4
#define AGENT_PATCH_CONTEXT_LINES 5
#define AGENT_TOOL_ERR         1024

#define AGENT_SPILL_BYTES      (16u << 20)
#define AGENT_SPILL_PATH_MAX   128
#define AGENT_SPILL_NOTE_BYTES 256
/* A tool result older than this many user turns, or than this many tool
 * rounds, is replaced on the wire by a line naming what it was; see
 * conv_write_json. Both, because neither alone measures age: an autonomous
 * turn is one user message and many rounds, and a conversation of short
 * exchanges has too few rounds to reach back at all. The round boundary
 * advances a block of ROUNDS at a time, so a boundary moving every round
 * does not leave a provider's prefix cache nothing to hit. */
#define AGENT_ELIDE_TURNS      2
#define AGENT_ELIDE_ROUNDS     4
#define AGENT_ELIDE_BYTES      512
/* Nominal cost of that replacement line. The context gauge charges this for
 * an elided result instead of the bytes the request will not carry. */
#define AGENT_ELIDE_NOTE_BYTES 76
/* Compaction is due at whichever is lower of AT percent of the window and
 * the window less RESERVE: a percentage of a small window leaves no room for
 * the round that discovers it. KEEP_PCT is how much of the window the tail
 * replayed verbatim past a compaction may cost, capped so at least HEAD_PCT
 * of the conversation is left to summarize. The tail starts at a round
 * boundary, so an assistant call is never separated from the results that
 * answer it. */
#define AGENT_COMPACT_RESERVE    16384
#define AGENT_COMPACT_KEEP_PCT   30
#define AGENT_COMPACT_HEAD_PCT   50
#define AGENT_COMPACT_AT         85

#define AGENT_RETRIES          4
#define AGENT_RETRY_DELAY_MS   2000
#define AGENT_MAX_RETRY_DELAY_MS 30000

#define AGENT_ASK_TIMEOUT_MS   180000

#define AGENT_SHELL_TIMEOUT_MS 120000

#define AGENT_JOB_WAIT_MS      120000
#define AGENT_JOB_WAIT_MAX_MS  240000
#define AGENT_MAX_JOBS         8

#define AGENT_JOB_CMD_CHARS    96
#define AGENT_MAX_COMMANDS     32
#define AGENT_LINE_BUF         (1u << 20)
#define AGENT_RESP_BUF         (1u << 22)
#define AGENT_MAX_HISTORY      500
#define AGENT_HISTORY_BYTES    (1u << 20)
#define AGENT_MAX_HISTORY_LINE (1u << 16)
#define AGENT_MAX_HISTORY_BYTES (8u << 20)
#define AGENT_MAX_CONFIG_FILES 8
#define AGENT_MAX_PROJECT_FILES 8
#define AGENT_MAX_SETTINGS     512
#define AGENT_MAX_SETTINGS_BYTES (1u << 20) 
#define AGENT_MAX_SET_KEYS     8
#define AGENT_MAX_TOOL_LIST    256

#define AGENT_CONFIG_NAME      STR("config.toml")
#define AGENT_STATE_NAME       STR("state.toml")
#define AGENT_CREDENTIALS_NAME STR("credentials.toml")

#define AGENT_PROJECT_DIR      STR("." AGENT_NAME)
// Past this arqan refuses to start rather than send a truncated prompt.
#define AGENT_MAX_PROMPT_FILE  (1u << 16)
#define AGENT_MAX_AGENTS_FILES 8
#define AGENT_MAX_SESSIONS     64
#define AGENT_MAX_SESSION_BYTES (32u << 20) 
/* A session's name: one short line of display text, not a file name. */
#define AGENT_MAX_TITLE        64

#define AGENT_MAX_POPUP        4096
#define AGENT_MAX_MODELS       AGENT_MAX_POPUP

#define AGENT_MAX_FAVORITES    64
#define AGENT_MAX_KEY_ROWS     128
#define AGENT_MAX_ENDPOINTS    32
#define AGENT_MAX_ENDPOINT_NAME 64
#define AGENT_MAX_URL          512
#define AGENT_MAX_MODEL_NAME   128
#define AGENT_MAX_API_KEY      512
#define AGENT_MAX_SECRET_ARGV  16
#define AGENT_MAX_SECRET_CMD   512
/* A locked keyring may prompt through its own agent; past this the helper is
 * killed, since a wait with no end would take the UI with it. */
#define AGENT_SECRET_TIMEOUT_MS 15000
/* A clipboard helper answers at once or not at all: it reads what the display
 * server already holds. Past this it is killed, so a stuck one costs a moment
 * rather than the session. */
#define AGENT_CLIPBOARD_TIMEOUT_MS 3000
#define AGENT_MAX_REASONING_LIST 1024
/* A desktop notification is a one-line summary, not a transcript: the text
 * is cut to this and the tail is dropped rather than wrapped. */
#define AGENT_MAX_NOTIFY_TEXT  128
#define AGENT_MAX_NOTIFY_CMD   512
#define AGENT_MAX_NOTIFY_ARGV  16
#define AGENT_MAX_REASONING_TEMPLATE (16u << 10)
#define AGENT_MAX_MODEL_BYTES  (1u << 20)
/* A context window an endpoint reports above this is not one; the field is
 * left unknown rather than clamped to something we made up. */
#define AGENT_MAX_CONTEXT_WINDOW ((size_t)1 << 31)
#define AGENT_WEB_BODY_BYTES   (2u << 20)
#define AGENT_WEB_URL_BYTES    4096
#define AGENT_WEB_QUERY_BYTES  1025
#define AGENT_WEB_TYPE_BYTES   128
#define AGENT_WEB_SEARCH_INTERVAL_MS 10000
#define AGENT_WEB_SEARCH_PAUSE_MS 3600000

#define AGENT_WEB_USER_AGENT \
    "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0"
#define AGENT_STATUS_FIELDS    10

// ---- arenas -------------------------------------------------------------
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

#define arena_new(a, T, n) ((T *)arena_alloc_array((a), (n), sizeof(T), alignof(T)))


typedef struct { const char *p; size_t n; } Str;
#define STR(lit) ((Str){ (lit), sizeof(lit) - 1 })

Str     str_c(const char *z);
/* Copies into `a`, terminated. Safe for an empty Str carrying a NULL pointer.
 * An empty `s` still allocates, so a NULL `.p` back means the arena is full. */
Str     str_dup(Arena *a, Str s);
/* An optional value: empty stays unset ((Str){0}) rather than becoming an
 * allocated "", which callers that test the pointer or hand it to curl rely
 * on. A full arena answers the same way, so tell the two apart with `s.n`. */
Str     str_dup_opt(Arena *a, Str s);
b8    str_eq(Str a, Str b);
b8    str_starts(Str s, Str prefix);
Str     str_trim(Str s);
Str     str_take(Str s, size_t n);
Str     str_drop(Str s, size_t n);

b8      str_line(Str s, size_t *off, Str *line);
size_t  str_lines(Str s);

Str     str_clip_utf8(Str s, size_t max);
/* Decode one UTF-8 sequence from `s`, writing the code point to `*cp` and
 * returning its length in bytes. 0 when the bytes are not a well formed
 * sequence, leaving `*cp` untouched: the caller decides what to do with
 * input it did not produce. */
size_t  utf8_decode(const char *s, size_t n, u32 *cp);

i32     agent_width(u32 cp);
i64    str_int(Str s, b8 *ok);

u64     str_hash64(Str s);

/* Growable char buffer doubling into an arena. `oom` latches when a growth
 * failed: every later write is dropped, and callers check buf_ok() before
 * trusting the contents. */
typedef struct { char *p; size_t n, cap; Arena *a; b8 oom; } Buf;
void    buf_init(Buf *b, Arena *a, size_t cap);
b8      buf_ok(const Buf *b);
/* Wraps bytes already in `a` as a full buffer so a caller that read them can
 * keep editing them in place. The buffer does not own the bytes: growth
 * allocates elsewhere in `a` and copies, leaving the originals untouched. */
void    buf_adopt(Buf *b, Arena *a, Str s);
/* Makes `need` bytes writable, returning whether they are. Failure latches
 * the buffer the way a failed write does. */
b8      buf_reserve(Buf *b, size_t need);
void    buf_putc(Buf *b, char c);
void    buf_put(Buf *b, const void *p, size_t n);
void    buf_puts(Buf *b, Str s);
void    buf_putf(Buf *b, const char *fmt, ...) __attribute__((format(printf,2,3)));
void    buf_json_str(Buf *b, Str s);

void    buf_json_chars(Buf *b, Str s);
/* Standard base64 with padding, appended raw: a caller that needs it inside
 * JSON writes the quotes, since the alphabet needs no escaping. */
void    buf_base64(Buf *b, const void *p, size_t n);
Str     buf_finish(Buf *b);

/* ---- files ---------------------------------------------------------------
 * The one reader every file arqan owns goes through, so a size that comes from
 * the filesystem is checked in one place rather than at each caller.
 */
typedef enum {
    FILE_OK, FILE_MISSING, FILE_NOT_REGULAR, FILE_TOO_LARGE,
    FILE_NO_MEMORY, FILE_UNREADABLE
} FileStatus;


FileStatus file_read(Arena *a, const char *path, size_t max, size_t head,
                     Str *out, u64 *size_out);

/* Writes a sibling temporary file, flushes and syncs it, then renames it over
 * `path`. An existing non-symlink keeps its permission bits; a new file uses
 * `mode` subject to the process umask. A destination symlink is replaced, not
 * followed. `write_fn` does not own or close the stream. When `sync_parent`
 * is true, a failure syncing the parent can be reported after the replacement
 * is visible. On failure errno names the operation that failed. */
typedef b8 (*FileWriteFn)(FILE *f, void *ud);
b8 file_write_atomic(const char *path, u32 mode, b8 sync_parent,
                     FileWriteFn write_fn, void *ud);
b8 file_write_atomic_str(const char *path, Str data, u32 mode,
                         b8 sync_parent);


enum { AGENT_LOG_DEBUG, AGENT_LOG_INFO, AGENT_LOG_WARN, AGENT_LOG_ERROR };
void    agent_log(i32 level, const char *fmt, ...) __attribute__((format(printf,2,3)));
void    agent_log_set_level(i32 level);

typedef void (*AgentLogSink)(i32 level, Str msg, void *ud);
void    agent_log_set_sink(AgentLogSink sink, void *ud);


typedef struct { char buf[1024]; size_t n; b8 full, live; } TelEvent;


void telemetry_init(Arena *scratch, b8 on);
b8   telemetry_on(void);

void telemetry_bind(Str session_path);

void telemetry_detach(void);

void telemetry_close(void);
/* Writes the session event: what the record needs to be read on its own.
 * Called on the first event of a file, since a file that starts mid-run
 * would otherwise say nothing about the run. */
typedef void (*TelHeader)(void *ud);
void telemetry_set_header(TelHeader fn, void *ud);

b8   telemetry_set(b8 on, Arena *scratch);

Str  telemetry_file(void);

void telemetry_log(i32 level, Str msg);

void tel_open(TelEvent *e, const char *ev);
void tel_int(TelEvent *e, const char *key, i64 v);
void tel_bool(TelEvent *e, const char *key, b8 v);

void tel_bucket(TelEvent *e, const char *key, u64 v);

void tel_str(TelEvent *e, const char *key, Str v);

void tel_shape(TelEvent *e, const char *key, Str text);
/* The top-level keys of a tool call's JSON arguments, comma-separated; the
 * values carry the path or the command, so none is recorded. `scratch` is
 * rewound before returning. */
void tel_arg_keys(TelEvent *e, const char *key, Str args, Arena *scratch);

void tel_hash_field(TelEvent *e, const char *key, Str v);
void tel_send(TelEvent *e);


f64  agent_now_seconds(void);


typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct JVal JVal;
struct JVal {
    Str   key;
    JType type;
    union {
        b8    b;
        f64  n;
        Str     s;
        struct { JVal *items; size_t n; } arr;
        struct { JVal *head; } obj;
    } u;
    JVal  *next;
};

typedef struct { Arena *a; const char *src; size_t pos, len; i32 depth; b8 oom; } JParser;

JVal   *json_parse(Arena *a, Str s);
/* Like json_parse, but reports the 1-based byte and nearby input when the
 * document is invalid. `err` is untouched on success. */
JVal   *json_parse_error(Arena *a, Str s, char *err, size_t err_cap);
void    json_write(Buf *b, const JVal *v);
const JVal *json_get(const JVal *obj, Str key);
const JVal *json_at(const JVal *arr, size_t i);
/* Empty when the member is absent or is not a string, which is the same
 * answer to the caller: the field it asked for is not there. */
Str    json_str(const JVal *obj, Str key);

b8     json_bool(const JVal *obj, Str key);

/* ---- XDG base directories ------------------------------------------------
 * Every file arqan owns is resolved here; none sits directly in $HOME. A
 * relative XDG_* value is invalid and ignored, as the spec requires.
 */
typedef enum { AGENT_DIR_CONFIG, AGENT_DIR_DATA, AGENT_DIR_STATE, AGENT_DIR_CACHE } AgentDir;


Str    paths_dir(AgentDir kind, Arena *a);
Str    paths_file(AgentDir kind, Str name, Arena *a);
b8     paths_ensure_dir(Str dir);

size_t paths_config_files(Str name, Arena *a, Str *out, size_t max);

size_t paths_project_files(Str name, Arena *a, Str *out, size_t max);

Str    paths_project_dir(Arena *a);

/* ---- settings files ------------------------------------------------------
 * One syntax for every setting arqan owns, a subset of TOML: "key = value"
 * lines grouped under optional "[section]" headers, with '#' comments. A
 * value may be quoted or bare; reading accepts both and writing quotes
 * anything that is not an integer or a boolean, so the result parses as TOML
 * and an editor highlights it.
 *
 * Parsed Strs point into the arena copy of the file, so they live as long as
 * the arena they were read into. A quoted value is unescaped in place in
 * that copy.
 */
typedef struct {
    Str    section[AGENT_MAX_SETTINGS];
    Str    key[AGENT_MAX_SETTINGS];
    Str    val[AGENT_MAX_SETTINGS];
    size_t n;
} Settings;


b8     settings_load(Settings *s, Str path, Arena *a);

Str    settings_get(const Settings *s, Str section, Str key);

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

b8     settings_remove_section(Str path, Str section, Arena *scratch);
/* Replaces a settings file wholesale, through a temporary file and a rename.
 * For rewriting a file whose format changed; a per-key change is an upsert,
 * since a settings file is a document its owner edits. */
b8     settings_write(Str path, Str data, u32 mode);


b8     state_set(Str key, Str val, Arena *scratch);

b8     state_set_many(const Str *keys, const Str *vals, size_t n,
                      Arena *scratch);

b8     state_set_in(Str section, Str key, Str val, Arena *scratch);

/* ---- prompt history ------------------------------------------------------
 * A ring of past prompts, mirrored to $XDG_STATE_HOME/arqan/history as they
 * are submitted. `cursor` is the browse position; cursor == n is the live
 * draft the composer restores on the way back down.
 *
 * Entries need an arena of their own because /clear rewinds the session's. A
 * full one is compacted in place: a bump allocator hands entries out in
 * ascending address order, so the move never overlaps forward.
 */
typedef struct {
    Str   *entry;
    size_t n, cap;
    size_t cursor;
    Str    path;
    Arena *a;
    size_t base_off; 
} History;

b8   history_init(History *h, Arena *own, size_t cap);
void history_load(History *h, Str path, Arena *scratch);
void history_rewrite(const History *h);
void history_add(History *h, Str line);
b8   history_prev(History *h, Str *out);

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


typedef enum {
    SECRET_STORED = 0,
    SECRET_SERVICE,
    SECRET_PASS,
    SECRET_KEYCHAIN,
    SECRET_COMMAND,
} SecretSource;

/* The name written in and read from the credentials file. `known` reports
 * whether the value matched one; an unknown source is not a silent fallback,
 * since guessing which store to ask is guessing where the key is. */
SecretSource secret_source_from_str(Str s, b8 *known);
Str          secret_source_name(SecretSource src);
b8           secret_source_external(SecretSource src);
/* False for the sources arqan can read but not write, which must be filled in
 * with their own tool. */
b8           secret_source_can_store(SecretSource src);


Str secret_lookup(SecretSource src, Str account, Str command, Arena *out,
                  char *err, size_t err_cap);
b8  secret_store(SecretSource src, Str account, Str key,
                 char *err, size_t err_cap);

b8  secret_erase(SecretSource src, Str account, char *err, size_t err_cap);

/* ---- endpoints -----------------------------------------------------------
 * The providers /provider creates: a name, a base URL and the API that URL
 * speaks. A provider is a connection and nothing else. It is not switched
 * between and it does not own the conversation: /model offers the models of
 * every provider at once, and choosing one selects the connection that serves
 * it. The `provider` setting records which provider serves the chosen model,
 * beside the `model` that names it.
 *
 * Each is a "[providers.<name>]" section of a config file, and its key alone
 * lives under the same section of $XDG_STATE_HOME/arqan/credentials.toml, so
 * a shared configuration cannot carry a secret. An oversized field is dropped
 * on load rather than truncated, since a cut URL names a different service.
 */
typedef struct {
    Str     name[AGENT_MAX_ENDPOINTS];
    Str     base_url[AGENT_MAX_ENDPOINTS];
    
    Str     model[AGENT_MAX_ENDPOINTS];
    
    Str     small_model[AGENT_MAX_ENDPOINTS];
    ApiKind api[AGENT_MAX_ENDPOINTS];
    size_t  n;
} Endpoints;

#define ENDPOINT_NONE ((size_t)-1)


size_t endpoints_load(Endpoints *e, Arena *a);
size_t endpoints_find(const Endpoints *e, Str name);

b8     endpoint_name_ok(Str name);

b8     endpoints_put(Endpoints *e, Str name, Str base_url, ApiKind api,
                     Arena *a);

b8     endpoints_save_one(Str name, Str base_url, ApiKind api, Arena *scratch);
/* The provider's own small model, allocated in `scratch`. Empty when it has
 * none. */
Str    endpoints_small_model(Str name, Arena *scratch);
/* The key stored for `name`, allocated in `out`. Empty when there is none,
 * and empty with `err` filled in when the credentials file is readable by
 * anyone but its owner: that is a key to rotate rather than one to load. */
Str    endpoints_key(Str name, Arena *out, Arena *scratch,
                     char *err, size_t err_cap);

SecretSource endpoints_key_source(Str name, Arena *scratch);

b8     endpoints_set_key(Str name, Str key, SecretSource src, Arena *scratch,
                         char *err, size_t err_cap);

b8     endpoints_delete(Str name, Arena *scratch, char *err, size_t err_cap);

/* ---- favorite models -----------------------------------------------------
 * The models pinned to the top of the /model picker: (provider, model) pairs
 * kept in the state file as one "[favorites.<provider>]" section per
 * provider, and "[favorites]" for the endpoint a run names with a base URL
 * alone. A pin is a pair because a model is: the same id at two providers is
 * two models, and only one of them may be pinned.
 *
 * Order is the order they were pinned in, provider by provider.
 */
typedef struct {
    Str    provider[AGENT_MAX_FAVORITES];   
    Str    model[AGENT_MAX_FAVORITES];
    size_t n;
} Favorites;


size_t favorites_load(Favorites *f, const Endpoints *e, Arena *a);
b8     favorites_has(const Favorites *f, Str provider, Str model);
/* Pins the pair when it is not pinned and unpins it when it is, then writes
 * that provider's section back. `*on` is the state the pair ends in, whether
 * or not the write succeeded; false is returned with `err` filled in when the
 * list is full, the id cannot be stored or the file could not be written.
 * Both strings are kept by reference, so they must outlive `f`; `scratch` is
 * rewound before returning, so anything `f` already holds must be allocated
 * before the call. */
b8     favorites_toggle(Favorites *f, Str provider, Str model, Arena *scratch,
                        b8 *on, char *err, size_t err_cap);

b8     favorites_forget(Str provider, Arena *scratch);

/* Optional user-owned capabilities for one exact (provider, model) pair.
 * Missing fields stay unavailable rather than being inferred from names. */
typedef struct {
    Str reasoning_efforts, thinking_budgets;
    Str reasoning_effort, thinking_budget, reasoning_template;
    size_t context_window;
    b8 configured;
} ModelProfile;

/* Every returned string lives in `out`; file parsing uses `scratch`. The two
 * arenas may be the same. A missing or invalid profile is all zeroes. */
void model_profile_load(ModelProfile *p, Str provider, Str model, Arena *out,
                        Arena *scratch);
b8   model_profile_save(Str provider, Str model, const ModelProfile *p,
                        Arena *scratch);

b8   model_profiles_delete(Str provider, Arena *scratch);

/* ---- agent modes ---------------------------------------------------------
 * Build carries the work out; Plan reads and proposes, handing over through
 * the submit_plan tool. The mode decides which system prompt slot 0 holds
 * and which tools the registry offers, so "read-only" is a property of the
 * registry rather than a request made in the prompt.
 */
typedef enum { MODE_BUILD = 0, MODE_PLAN } AgentMode;


typedef enum { PERMISSION_ASK = 0, PERMISSION_FREE } PermissionPolicy;

/* Declarative effect classes carried by the tool registry. Unguarded tools
 * never need authorization; each guarded class can be remembered separately
 * for the lifetime of one process. */
typedef enum {
    TOOL_APPROVAL_NONE = 0,
    TOOL_APPROVAL_BASH,
    TOOL_APPROVAL_WRITE,
    TOOL_APPROVAL_PATCH,
} ToolApprovalClass;


typedef enum { TOOL_AUTH_DENIED = 0, TOOL_AUTH_GRANTED } ToolAuthorization;


typedef struct {
    Str primary, primary_label, primary_path;
    Str agents[AGENT_MAX_AGENTS_FILES];
    Str agent_paths[AGENT_MAX_AGENTS_FILES];
    size_t n_agents;
} PromptSources;


typedef enum {
    CONF_PROVIDER, CONF_BASE_URL, CONF_MODEL, CONF_API, CONF_API_KEY,
    CONF_MAX_TOKENS, CONF_MAX_MESSAGES, CONF_STREAM, CONF_MODE,
    CONF_PERMISSIONS,
    CONF_RETRIES, CONF_RETRY_DELAY_MS, CONF_DISABLE_TOOLS,
    CONF_VERBOSE_TOOLS, CONF_RAW_MARKDOWN, CONF_SHOW_IGNORED,
    CONF_SHOW_INSTRUCTIONS, CONF_WRAP, CONF_STATUS_FIELDS, CONF_TELEMETRY,
    CONF_NOTIFY, CONF_NOTIFY_COMMAND, CONF_NOTIFY_MIN_MS,
    CONF_SEARCH_BACKEND, CONF_SEARCH_ENDPOINT, CONF_SEARCH_API_KEY,
    CONF_SEARCH_ENGINE_ID,
    CONF_SMALL_MODEL, CONF_SMALL_PROVIDER, CONF_AUTO_TITLE,
    CONF_ASK_TIMEOUT_MS,
    CONF_SHELL_TIMEOUT_MS,
    CONF_IMAGES,
    CONF_RESUME_LAST,
    CONF_COMPACT, CONF_COMPACT_AT, CONF_COMPACT_MODEL,
    CONF_N
} ConfKey;


typedef enum {
    CONF_FROM_DEFAULT, CONF_FROM_SYSTEM, CONF_FROM_USER, CONF_FROM_PROJECT,
    CONF_FROM_STATE, CONF_FROM_ENDPOINT, CONF_FROM_ENV
} ConfOrigin;

/* Resolved settings. Values live in the arena conf_resolve was given and are
 * already validated, so a reader never rechecks a bound. */
typedef struct {
    Str val[CONF_N];
    u8  origin[CONF_N];
    ModelProfile model_profile;
} Conf;

/* Reads every source in precedence order. Values are copied into `persist`;
 * `scratch` is rewound before returning. A value a source cannot set, or one
 * outside its bounds, is reported and dropped rather than clamped, so a
 * mistyped line never shadows a good one below it. */
void   conf_resolve(Conf *c, Arena *persist, Arena *scratch);
Str    conf_key_name(ConfKey k);
Str    conf_str(const Conf *c, ConfKey k);
i64    conf_num(const Conf *c, ConfKey k);
b8     conf_bool(const Conf *c, ConfKey k);

b8     conf_value_ok(ConfKey k, Str val);

b8     conf_remember(ConfKey k, Str val, Arena *scratch);

b8     conf_remember_pair(ConfKey a, Str va, ConfKey b, Str vb,
                          Arena *scratch);
b8     conf_remember_bool(ConfKey k, b8 on, Arena *scratch);

/* Presentation choices remembered by /settings and /statusline, read from the
 * same table. The structure owns no strings. */
typedef struct {
    b8 verbose_tools;
    b8 raw_markdown;
    b8 show_ignored;
    b8 show_instructions;
    b8 justify;
    b8 telemetry;
    u64 status_fields;
} UiPrefs;

void   ui_prefs_load(UiPrefs *p, const Conf *conf);


typedef struct {
    const char *pat[AGENT_IGNORE_PATTERNS];
    u8 flag[AGENT_IGNORE_PATTERNS];
    u16 base[AGENT_IGNORE_PATTERNS];
    size_t n;
    char buf[AGENT_IGNORE_BYTES];
    size_t buf_n;
} AgentIgnore;

typedef struct { size_t n, buf_n; } AgentIgnoreMark;

void            agent_ignore_build(AgentIgnore *ig, Str dir);
void            agent_ignore_push(AgentIgnore *ig, const char *dir,
                                  size_t dir_n, size_t path_n);
b8              agent_ignore_match(const AgentIgnore *ig, const char *rel,
                                   size_t rel_n, b8 is_dir);
AgentIgnoreMark agent_ignore_mark(const AgentIgnore *ig);
void            agent_ignore_restore(AgentIgnore *ig, AgentIgnoreMark mark);

void            agent_ignore_set_show(b8 on);
b8              agent_ignore_show(void);

/* ---- notifications ------------------------------------------------------
 * What arqan tells the user about once they have looked away. The terminal
 * owns the bridge to the desktop, so nothing here links an OS notification
 * API: an escape goes out through the paint buffer, and anything more (a
 * headless session, an unsupported terminal, tmux without passthrough) is
 * the user's own notify_command, which is a route of its own rather than a
 * fallback: it runs whether or not `notify` sends an escape.
 */
typedef enum {
    NOTIFY_TURN_DONE,
    NOTIFY_TURN_FAILED,
    NOTIFY_INPUT_NEEDED,
    NOTIFY_INTERRUPTED,
} NotifyKind;

// Reads the notify settings once. `persist` owns the copied command line.
void notify_init(const Conf *c, Arena *persist);

void notify_event(NotifyKind kind, Str detail, f64 elapsed_ms);


/* What happens when the conversation nears the model's context window.
 * COMPACT_OFF says nothing, COMPACT_MANUAL says so once and leaves /compact
 * to the user, COMPACT_AUTO summarizes the older turns in place. Nothing
 * fires without a configured `context_window`: an unknown window is not a
 * window that is nearly full. */
typedef enum { COMPACT_OFF = 0, COMPACT_MANUAL, COMPACT_AUTO } CompactMode;

typedef struct {
    Str base_url;
    Str model;
    Str api_key;
    ApiKind api;
    /* The provider serving `model`, empty when a base URL from a flag, the
     * environment or a config file names the endpoint instead. */
    Str provider;
    
    Str small_model;
    
    Str small_provider;
    Str reasoning_efforts, thinking_budgets;
    Str reasoning_effort, thinking_budget;
    Str reasoning_template;
    size_t context_window;
    /* A run with neither this nor a key has nothing to talk to, and asks for
     * a provider instead of starting a conversation. */
    b8  base_url_set;
    Str system_prompt;
    Str plan_prompt;
    PromptSources system_sources, plan_sources;
    AgentMode mode;
    PermissionPolicy permissions;
    i32  max_tokens;
    
    size_t max_messages;
    b8 stream;
    
    i32 retries;
    i32 retry_delay_ms;
    /* Tools to turn off before the first turn, comma separated. Applied once
     * the registry exists, since config_load runs before tools_init. */
    Str disable_tools;
    
    b8 auto_title;
    
    b8 images;
    /* Whether an interactive start reopens the newest session of this
     * directory instead of greeting with the welcome screen. */
    b8 resume_last;
    CompactMode compact;
    /* Percentage of the window the conversation may reach before compaction
     * is due, capped by AGENT_COMPACT_RESERVE. */
    u32 compact_at;
    /* Whether the summarizing request goes to the small model rather than
     * the one the conversation is on. Ignored when none is configured. */
    b8 compact_small;
    
    i32 ask_timeout_ms;
    
    i32 shell_timeout_ms;
    
    char owned_base_url[AGENT_MAX_URL + 1];
    char owned_model[AGENT_MAX_MODEL_NAME + 1];
    char owned_small_model[AGENT_MAX_MODEL_NAME + 1];
    char owned_small_provider[AGENT_MAX_ENDPOINT_NAME + 1];
    char owned_api_key[AGENT_MAX_API_KEY + 1];
    char owned_provider[AGENT_MAX_ENDPOINT_NAME + 1];
    char owned_reasoning_efforts[AGENT_MAX_REASONING_LIST + 1];
    char owned_thinking_budgets[AGENT_MAX_REASONING_LIST + 1];
    char owned_reasoning_effort[AGENT_MAX_REASONING_LIST + 1];
    char owned_thinking_budget[AGENT_MAX_REASONING_LIST + 1];
    char owned_reasoning_template[AGENT_MAX_REASONING_TEMPLATE + 1];
} Config;

// Fills `c` from resolved settings. `persist` holds what it copies.
b8    config_load(Config *c, const Conf *conf, Arena *persist);
/* Writes the state file's `provider` and `model` keys together, since the two
 * name one model: an id belongs to the endpoint that serves it. An empty
 * provider is the endpoint a base URL names on its own, and an empty model
 * forgets the choice. conf_resolve applies both above the config files and
 * below the environment. */
b8    config_remember_model(Str provider, Str model, Arena *scratch);

b8    config_set_model(Config *c, Str model);

b8    config_set_small_model(Config *c, Str model, Str provider);
b8    config_set_endpoint(Config *c, Str name, Str base_url, Str model,
                          ApiKind api, Str key);
b8    config_set_model_profile(Config *c, const ModelProfile *p);
b8    config_set_reasoning(Config *c, b8 effort, Str value);


typedef struct {
    Str base_url, model, api_key, system_prompt;
    Str api;
    Str disable_tools;
    Str prompt;
    b8  have_prompt;
    i32 max_tokens;
} CliOpts;

typedef enum {
    CLI_RUN,
    CLI_DONE,
    CLI_ERROR,
} CliStatus;

CliStatus cli_parse(i32 argc, char **argv, CliOpts *out);
void      cli_apply(const CliOpts *o, Config *c);


#include <curl/curl.h>

/* libcurl is opened at the first request rather than at exec. Loading its
 * dependency tree costs about 1.5ms, some four times everything else this
 * program does before its first frame, and a session that asks nothing of a
 * provider never needs it. Calls go through the table, which the macros below
 * put in the way of the ordinary names; a static build has no dynamic loader
 * and links libcurl directly instead.
 *
 * curl_load fills the table on its first success and is idempotent after it.
 * Every entry point that reaches libcurl must call it and refuse the work
 * when it reports false: the pointers are null until it succeeds. */
#if AGENT_CURL_DLOPEN
typedef struct {
    CURL              *(*easy_init)(void);
    CURLcode           (*easy_setopt)(CURL *, CURLoption, ...);
    CURLcode           (*easy_getinfo)(CURL *, CURLINFO, ...);
    CURLcode           (*easy_perform)(CURL *);
    void               (*easy_cleanup)(CURL *);
    const char        *(*easy_strerror)(CURLcode);
    struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
    void               (*slist_free_all)(struct curl_slist *);
    CURLU             *(*url)(void);
    CURLUcode          (*url_set)(CURLU *, CURLUPart, const char *, unsigned);
    CURLUcode          (*url_get)(CURLU *, CURLUPart, char **, unsigned);
    void               (*url_cleanup)(CURLU *);
    void               (*free)(void *);
    CURLM             *(*multi_init)(void);
    CURLMcode          (*multi_add_handle)(CURLM *, CURL *);
    CURLMcode          (*multi_remove_handle)(CURLM *, CURL *);
    CURLMcode          (*multi_perform)(CURLM *, i32 *);
    CURLMcode          (*multi_poll)(CURLM *, struct curl_waitfd *, unsigned,
                                     i32, i32 *);
    CURLMsg           *(*multi_info_read)(CURLM *, i32 *);
    CURLMcode          (*multi_cleanup)(CURLM *);
    const char        *(*multi_strerror)(CURLMcode);
} CurlApi;

extern CurlApi g_curl;
b8 curl_load(char *err, size_t err_cap);


#undef curl_easy_setopt
#undef curl_easy_getinfo
#define curl_easy_init         g_curl.easy_init
#define curl_easy_setopt       g_curl.easy_setopt
#define curl_easy_getinfo      g_curl.easy_getinfo
#define curl_easy_perform      g_curl.easy_perform
#define curl_easy_cleanup      g_curl.easy_cleanup
#define curl_easy_strerror     g_curl.easy_strerror
#define curl_slist_append      g_curl.slist_append
#define curl_slist_free_all    g_curl.slist_free_all
#define curl_url               g_curl.url
#define curl_url_set           g_curl.url_set
#define curl_url_get           g_curl.url_get
#define curl_url_cleanup       g_curl.url_cleanup
#define curl_free              g_curl.free
#define curl_multi_init        g_curl.multi_init
#define curl_multi_add_handle  g_curl.multi_add_handle
#define curl_multi_remove_handle g_curl.multi_remove_handle
#define curl_multi_perform     g_curl.multi_perform
#define curl_multi_poll        g_curl.multi_poll
#define curl_multi_info_read   g_curl.multi_info_read
#define curl_multi_cleanup     g_curl.multi_cleanup
#define curl_multi_strerror    g_curl.multi_strerror
#else
#define curl_load(err, cap)    ((void)(err), (void)(cap), true)
#endif

typedef struct {
    const char *base_url;
    const char *api_key;

    ApiKind api;

    b8 (*on_line)(Str line, void *ud);
    void *ud;
    /* Where a streamed line is accumulated. An event carries as much as the
     * provider chose to send, so the buffer grows instead of clipping: half a
     * delta is not JSON, and the reply behind it would be lost without a
     * word. Required whenever on_line is set. */
    Arena *line_arena;

    Buf *body_out;
    const char *body;
    const volatile sig_atomic_t *interrupt_flag;
    /* The transfer waits on curl's sockets and `idle_fd` together, so the UI
     * stays alive without threads. on_idle runs after every wait and must
     * not block. */
    i32   idle_fd;
    void (*on_idle)(void *ud);
    void *idle_ud;

    char  *fail_out;
    size_t fail_cap;
} HttpReq;

/* POST the body to the API's completion path (/chat/completions, or
 * /messages for API_ANTHROPIC), delivering the reply through on_line or
 * body_out. 0 on success, a negative HTTP status for a refused request, 3
 * for an interrupt, other positive values for a transport failure. */
i32     http_post(const HttpReq *r);
/* GET base_url + path, appending the whole body to `out`, with the statuses
 * above. Blocking: callers fetch a short document between turns. A transport
 * failure is written to `fail_out` when one is given, and is not logged as an
 * error: the caller says what a document it could not fetch means. */
i32     http_get(const char *base_url, const char *path, const char *api_key,
                ApiKind api, Buf *out, char *fail_out, size_t fail_cap);

typedef struct {
    const char *url;
    const char *operation;
    Buf *out;
    size_t max_bytes;
    i32 connect_timeout_ms;
    i32 timeout_ms;
    i32 max_redirects;
    b8 public_only;
    /* Extra request headers, "Name: value" each, unset entries NULL. Their
     * storage belongs to the caller and outlives the call. */
    const char *header[2];
    const volatile sig_atomic_t *interrupt_flag;
    i32 idle_fd;
    void (*on_idle)(void *ud);
    void *idle_ud;

    char effective_url[AGENT_WEB_URL_BYTES];
    char content_type[AGENT_WEB_TYPE_BYTES];
    char failure[256];
    i64 status;
} HttpUrlReq;

/* GET one arbitrary HTTP(S) URL. The decompressed body is either complete in
 * `out` or absent with a failure: a source document is never truncated.
 * Redirects share the protocol, credential and socket-address restrictions.
 * Returns the same status classes as http_post. */
i32     http_url_get(HttpUrlReq *r);

#ifdef AGENT_TESTING

void    http_print_ca_trust(void);
#endif

/* ---- spilled tool output -------------------------------------------------
 * The full output of a tool whose result is a page, written to
 * "$TMPDIR/arqan-<tool>-<hash>.<ext>" at mode 0600 so a later call can
 * narrow it without replaying it. Best effort throughout: every entry point is a
 * no-op once the spill has failed or was never opened, and the tool answers
 * as it would without one. No arena: a Spill owns only its file. */
typedef struct {
    char   path[AGENT_SPILL_PATH_MAX];
    i32    fd;
    size_t written;
    size_t buf_n;
    b8     full;
    char   buf[4096];
} Spill;


void    spill_open(Spill *s, const char *tool, const char *ext, Str key);
void    spill_put(Spill *s, const char *p, size_t n);
void    spill_putf(Spill *s, const char *fmt, ...);
/* Closes the spill. With `keep`, appends one line to `out` naming the file
 * and roughly how large it is; otherwise, and when nothing was written, the
 * file is unlinked and `out` is untouched. The note fits in
 * AGENT_SPILL_NOTE_BYTES, which a caller reserves in its result budget. */
void    spill_finish(Spill *s, Buf *out, b8 keep);
/* Hands the file over: flushes what is buffered, copies the path into `path`,
 * reports how much is already in it through `written`, and leaves the Spill
 * empty without unlinking. Returns the open write fd, which the caller owns
 * and must close, or -1 when there is no file or the flush failed (and then
 * the file is unlinked as spill_finish would). */
i32     spill_release(Spill *s, char *path, size_t path_cap, size_t *written);

void    spill_size_text(char *z, size_t cap, size_t n);


typedef b8 (*ToolRun)(Str args_json, Arena *scratch, Buf *out,
                      char *err, size_t err_cap);


#define TOOL_IN_BUILD 1u
#define TOOL_IN_PLAN  2u
/* Answered by the agent loop rather than run, so it is never offered as a
 * toggle and never disabled. */
#define TOOL_FIXED    4u

#define TOOL_INTERACTIVE 8u

typedef struct {
    Str     *name;
    Str     *desc;
    /* What a row of the settings screen says: one line that fits beside the
     * name, since the model's description is written for a model. */
    Str     *brief;
    Str     *schema;
    ToolRun *run;
    u8      *modes;
    u8      *approval;
    b8      *off;
    size_t   n;
} ToolRegistry;


#define TOOL_NONE ((size_t)-1)

void        tools_init(ToolRegistry *r, Arena *persist,
                       i32 shell_timeout_ms);

void        tools_set_mode(AgentMode mode);

void        tools_set_interactive(b8 interactive);

b8          tools_available(const ToolRegistry *r, size_t id, AgentMode mode);
size_t      tools_find(const ToolRegistry *r, Str name);
ToolApprovalClass tools_approval_class(const ToolRegistry *r, size_t id);

Str         tools_approval_name(ToolApprovalClass approval);

b8          tools_can_disable(const ToolRegistry *r, size_t id);
b8          tools_disabled(const ToolRegistry *r, size_t id);
void        tools_set_disabled(ToolRegistry *r, size_t id, b8 off);

b8          tools_disable_list(ToolRegistry *r, Str names,
                               char *err, size_t err_cap);
b8          tools_run(const ToolRegistry *r, size_t id, Str args,
                      ToolAuthorization authorization,
                      Arena *scratch, Buf *out, char *err, size_t err_cap);

void        tools_write_schemas(Buf *b, const ToolRegistry *r, ApiKind api);
/* What those schemas cost a request, near enough for an estimate: the bytes
 * the currently available tools would write. Counts no arena. */
size_t      tools_schema_bytes(const ToolRegistry *r);

void        web_set_idle(void (*fn)(void *ud), void *ud, i32 idle_fd,
                         const volatile sig_atomic_t *interrupt_flag);
/* Chooses the search engines internet_search tries and copies the endpoint,
 * key and engine id it needs into `persist`. A backend whose requirement is
 * missing is reported here and the search chain falls back to the keyless
 * engines, so a half-configured key never fails silently at call time. */
void        web_search_init(const Conf *c, Arena *persist);
b8          internet_search_run(Str args, Arena *scratch, Buf *out,
                                char *err, size_t err_cap);
b8          page_fetch_run(Str args, Arena *scratch, Buf *out,
                           char *err, size_t err_cap);
/* Run `cmd` through the shell, appending its output to `out` followed by a
 * bracketed status line ("[exit 0]") that render.c reads back. Only the last
 * AGENT_SHELL_OUT_BYTES are kept, behind a line saying how much was dropped.
 * False with `err` filled in when the command is longer than
 * AGENT_MAX_COMMAND or the shell could not be started; a command is never
 * clamped to fit, since a truncated one is a different program. */
b8          shell_capture(Str cmd, Buf *out, char *err, size_t err_cap);
/* Pumped while a command runs, so a slow one keeps the UI live the way an
 * in-flight request does; unset by default, since a tool is not the TUI's. */
void        shell_set_idle(void (*fn)(void *ud), void *ud);
void        shell_set_interrupt_flag(volatile sig_atomic_t *flag);

void        shell_set_timeout(i32 ms);
/* Kills every job still running, reaps it and drops its log. Registered with
 * atexit, so a detached command never outlives the session that started it. */
void        jobs_stop(void);


/* The system prompt, placeholders expanded. `configured` is what --system or
 * ARQAN_SYSTEM_PROMPT set, unset to take .arqan/SYSTEM.md, the global
 * SYSTEM.md or the built-in template, in that order. Returned in `persist`,
 * falling back to the unexpanded template when it cannot take the result.
 * Empty with `err` set when a SYSTEM.md is larger than AGENT_MAX_PROMPT_FILE.
 *
 * Every AGENTS.md from the working directory up to the root is appended to
 * whichever prompt won, outermost first and verbatim: it is a document about
 * the project rather than a template. */
Str   prompt_build(const ToolRegistry *tools, Str configured, Arena *persist,
                   Arena *scratch, PromptSources *sources, char *err,
                   size_t err_cap);

Str   prompt_build_plan(const ToolRegistry *tools, Arena *persist,
                        Arena *scratch, PromptSources *sources, char *err,
                        size_t err_cap);
/* The compaction instruction: a static document, so it needs no arena. It
 * stands in for the system prompt of the one request /compact makes, which
 * asks for a context checkpoint rather than for work. */
Str   prompt_compact(void);
/* The user turn that request ends on, so the summary is asked for by a
 * message rather than only by the system prompt. */
Str   prompt_compact_ask(void);

Str   prompt_title(void);

Str   prompt_title_ask(void);

/* ---- media (SoA) ---------------------------------------------------------
 * Images attached to a turn. The bytes are kept as they were read and are
 * base64-encoded on the first request that carries them. The encoding is
 * kept beside them, since the request is rebuilt from the whole conversation
 * for every turn and every tool round after it: a session holds an image
 * once at its own size and once encoded, a third larger.
 *
 * An entry whose `bytes` are empty is one a resumed session could not read
 * back. It keeps its place so the numbering in the transcript still matches
 * the text, and every writer skips it.
 */
typedef struct {
    Str  *mime;    
    Str  *bytes;   
    /* [cap] `bytes` encoded, written on first use and empty until then. The
     * arena is the one the entries were allocated from. */
    Str  *b64;
    Str  *label;   
    
    Str  *file;
    u32  *w, *h;   
    size_t n, cap;
    Arena *arena;
} MediaSet;

#define MEDIA_NONE ((size_t)-1)

b8     media_init(MediaSet *m, Arena *persist, size_t cap);

b8     media_sniff(Str bytes, Str *mime, u32 *w, u32 *h);

Str    media_ext(Str mime);
/* Copies `bytes` into `persist` and records it under `label`. MEDIA_NONE
 * with `err` filled in when the table is full, the bytes are not a supported
 * image, they are over AGENT_MAX_IMAGE_BYTES or AGENT_MAX_IMAGE_SIDE, or the
 * arena cannot take them. */
size_t media_add(MediaSet *m, Arena *persist, Str bytes, Str label,
                 char *err, size_t err_cap);
/* media_add on a file's contents, read through `scratch` so a refused image
 * costs `persist` nothing. `scratch` is rewound before returning. */
size_t media_add_file(MediaSet *m, Arena *persist, Arena *scratch, Str path,
                      char *err, size_t err_cap);

size_t media_add_missing(MediaSet *m, Arena *persist, Str label, Str mime,
                         Str file);
// Whether the entry has bytes to send. A missing one never reaches the wire.
b8     media_live(const MediaSet *m, size_t id);

size_t media_keep(MediaSet *m, size_t base, const size_t *ids, size_t n);

void   media_describe(char *out, size_t cap, const MediaSet *m, size_t id);

void   media_write_openai(Buf *b, const MediaSet *m, size_t id);
void   media_write_anthropic(Buf *b, const MediaSet *m, size_t id);

/* ---- clipboard ----------------------------------------------------------
 * The image the system clipboard holds, read through the first helper that
 * is installed (wl-paste, xclip, pngpaste) and returned as the bytes it
 * gave, in `scratch`. False with `err` filled in when no helper is
 * installed, the clipboard holds no image, or it holds more bytes than an
 * image may have. The bytes are not validated here: the caller hands them
 * to media_add, which refuses a format or a size the same way it would from
 * a file. Blocks for at most AGENT_CLIPBOARD_TIMEOUT_MS and does not pump
 * the UI, so it is safe to call from inside input handling.
 */
b8     clipboard_image(Arena *scratch, Str *out, char *err, size_t err_cap);


typedef enum { M_SYSTEM = 0, M_USER, M_ASSISTANT, M_TOOL } MRole;


#define CONV_NONE ((size_t)-1)

typedef struct {
    MRole *role;
    Str   *text;
    /* [cap] canonical JSON array of Anthropic thinking blocks attached to
     * an assistant head. The encrypted signatures must survive tool rounds. */
    Str   *anthropic_thinking;
    Str   *tool_name;
    Str   *tool_call_id;
    Str   *shell_out;
    b8  *has_tool_call;
    b8  *expanded;
    /* [cap] the carrier's arguments parse as a JSON object. Decided once,
     * when the call is recorded, since every later Anthropic request would
     * otherwise re-parse the whole history to ask the same question. */
    b8  *args_object;
    
    u32   *ms;
    /* [cap] the slot's images: `media_n` entries of `media` from `media_off`.
     * A slot's entries are contiguous because a turn's attachments are added
     * together, which is what lets one pair of numbers stand for a list. */
    u32   *media_off;
    u16   *media_n;
    /* The table those indices address, owned by the caller and shared with
     * every clone of this conversation; NULL when nothing can be attached. */
    MediaSet *media;
    size_t n, cap;
} Conv;

b8      conv_init(Conv *c, Arena *persist, size_t cap);
size_t  conv_add(Conv *c, MRole role, Str text);

void    conv_set_media(Conv *c, MediaSet *m);
/* Attach `n` media entries starting at `off` to slot `i`. The caller has
 * just appended them, so they are the table's last `n` entries. */
void    conv_attach_media(Conv *c, size_t i, size_t off, size_t n);
/* Drop every slot from `keep` on, releasing the media entries they held.
 * This is what /clear, a rewind and a resume rewind the conversation with:
 * the bytes behind those entries live in the region the caller is about to
 * rewind, so leaving the table pointing into it would outlive them. */
void    conv_truncate(Conv *c, size_t keep);

size_t  conv_add_assistant_calls(Conv *c, Str content);
/* `scratch` holds the parse that decides whether `args` are a JSON object;
 * it is rewound before returning. */
size_t  conv_add_call(Conv *c, Arena *scratch, Str id, Str name, Str args);
size_t  conv_add_tool(Conv *c, Str tool_call_id, Str text);
/* A '!' shell run: one user slot holding the command and what it printed,
 * since it is one turn the user took. */
size_t  conv_add_shell(Conv *c, Str cmd, Str out);
b8      conv_is_shell(const Conv *c, size_t i);
b8      conv_is_call(const Conv *c, size_t i);
size_t  conv_room(const Conv *c);
/* A second conversation over the same message storage: `src`'s slots are
 * copied and the strings they point at are shared, so the copy must not
 * outlive them. `extra` free slots are left past the copy, for the messages
 * the caller appends. False when `a` cannot take the arrays, leaving `dst`
 * with no capacity. */
b8      conv_clone(Conv *dst, const Conv *src, Arena *a, size_t extra);
/* The same copy over the first `keep` slots only, for a request made about
 * part of a conversation. */
b8      conv_clone_head(Conv *dst, const Conv *src, size_t keep, Arena *a,
                        size_t extra);

void    conv_write_json(Buf *b, const Conv *c, const ToolRegistry *reg);
/* The same conversation as Anthropic messages: content blocks rather than
 * flat text, preserved thinking blocks before their assistant content, tool
 * results carried by the user, and consecutive slots of one role merged into
 * a single message. The system prompt is written by the caller and skipped. */
void    conv_write_json_anthropic(Buf *b, const Conv *c);

/* Where a request stops eliding old tool results: the later of the slot the
 * last AGENT_ELIDE_TURNS user turns begin at and the slot the last
 * AGENT_ELIDE_ROUNDS tool rounds begin at. Zero when the conversation
 * reaches back no further than that, which elides nothing. */
size_t  conv_elide_start(const Conv *c);
/* True when slot `i` goes out as the elision note rather than its own text,
 * for a request whose recent window begins at `recent`. Anything measuring
 * what a request carries has to ask this rather than read `text[i].n`. */
b8      conv_result_elided(const Conv *c, size_t i, size_t recent);

/* True when a request may begin at slot `i`: a user turn, a plain assistant
 * reply, or the assistant message that opens a group of tool calls. A tool
 * result names a call id its assistant message declares, so a cut anywhere
 * else would send an answer whose question is gone. */
b8      conv_round_start(const Conv *c, size_t i);

/* Replace the slots from 1 up to `keep` with one user message holding
 * `checkpoint`, leaving slot 0 and everything from `keep` on as they stand.
 * `keep` must be at least 2 and must name a round boundary or the end. The
 * dropped slots' strings and media entries are not reclaimed: they live in
 * the persistent arena, which only /clear rewinds. False when the message
 * does not fit or `keep` is not a boundary, leaving the conversation
 * untouched. */
b8      conv_compact_head(Conv *c, size_t keep, Str checkpoint);

/* ---- sessions ------------------------------------------------------------
 * The conversation as it happened, one JSON object per line under
 * $XDG_DATA_HOME/arqan/sessions/<cwd>/<timestamp>.jsonl, keyed by the
 * directory arqan was launched in so browsing from one project never surfaces
 * another's.
 *
 * Paths live in the struct instead of an arena because /clear rewinds the
 * session arena and the file the next message appends to has to outlive it.
 */
typedef struct {
    char   dir_buf[AGENT_MAX_PATH];
    char   path_buf[AGENT_MAX_PATH];
    char   name_buf[32];
    
    char   title_buf[AGENT_MAX_TITLE + 1];
    Str    dir;
    Str    path;
    Str    name;
    Str    title;
    
    b8     title_tried;
    /* A failed append is retried while its old end was restored. If restoring
     * that boundary failed, later writes stop rather than risk duplicating or
     * appending behind a partial JSON record. */
    b8     save_blocked;
    b8     sync_dir;
    size_t written;
} Session;

typedef struct {
    Str   *name;
    Str   *path;
    Str   *preview;
    Str   *title;          
    size_t n;
} SessionList;

b8     session_init(Session *s, Arena *scratch);
b8     session_begin(Session *s);
/* Append the messages produced since the last call; the file is created on
 * the first one, so an untouched session never reaches the picker. False
 * fills `err`; uncommitted messages remain pending, while bytes confirmed by
 * fsync are never repeated. A later call retries unless restoring a failed
 * append's old boundary was itself unsafe. */
b8     session_save(Session *s, const Conv *c, char *err, size_t err_cap);

/* Start a copy only after its conversation is durable. False leaves `s`
 * naming the original session and fills `err`. */
b8     session_fork(Session *s, const Conv *c, char *err, size_t err_cap);

b8     session_export_markdown(const Conv *c, Str requested,
                               char *path, size_t path_cap,
                               char *err, size_t err_cap);

size_t session_list(const Session *s, Arena *a, SessionList *out, size_t max);
/* Remove one saved session file. `path` must name a file directly in this
 * session's own directory, which is what session_list hands out; the file
 * the live conversation is appending to is refused, since a session cannot
 * delete itself while it is still being written. False leaves the file
 * alone, including when it is that one. */
b8     session_delete(const Session *s, Str path);
/* Reading is separate from replaying because replaying rewinds the live
 * conversation, so a file that cannot be read has to be known before
 * anything is thrown away. `session_read` returns the raw contents in
 * `scratch` (empty when unreadable); `session_apply` replays them into a
 * conversation the caller has rewound to its system prompt and continues
 * appending to that file. False means the conversation filled up. */
Str    session_read(Str path, Arena *scratch);
b8     session_apply(Session *s, Str src, Str path, Str name, Conv *c,
                     Arena *persist, Arena *scratch);

b8     session_set_title(Session *s, Str title);


typedef struct {
    const Config      *cfg;
    const ToolRegistry*tools;
    Conv              *conv;
    Arena             *persist;
    Arena             *scratch;
    void (*on_text)(Str delta, void *ud);
    
    void (*on_reason)(Str delta, void *ud);
    void (*on_tool_call)(i32 index, Str id, Str name, Str args_delta, void *ud);
    /* The request's usage as it is heard: the mock and most providers send it
     * once, on the last stream event. `prompt_tokens` is the context this
     * request carried and is the only exact measurement of it there is;
     * `completion_tokens` is what the reply cost and includes reasoning the
     * next request will not carry. Fired from inside the request wait, so it
     * reaches the status line even when the turn is interrupted before it
     * ends. `conv` is the conversation the request was built from, which the
     * count describes and which the reply has not been appended to yet; it
     * is passed because `ud` belongs to the stream while one is running. */
    void (*on_usage)(const Conv *conv, size_t prompt_tokens,
                     size_t completion_tokens, void *ud);
    
    void (*on_retry)(i32 attempt, i32 attempts, i32 delay_ms, Str reason,
                     void *ud);
    void *ud;

    void (*on_idle)(void *ud);
    i32   idle_fd;
    const volatile sig_atomic_t *interrupt_flag;
    size_t prompt_tokens;
    size_t completion_tokens;
    size_t cache_creation_tokens;
    size_t cache_read_tokens;
    size_t total_tokens;
    b8 usage_valid;
} Provider;


#define PROVIDER_EMPTY (-2)
/* Run one completion turn, appending the assistant message and its tool
 * calls to conv. Returns the number of tool calls, PROVIDER_EMPTY for no
 * semantic output, or -1 with `err` set for every other failure. */
i32     provider_run(Provider *p, char *err, size_t err_cap);

/* Model ids from GET <base_url>/models, in the order the endpoint serves
 * them, allocated in `scratch`. Zero with `err` set when it could not be
 * read. Model capabilities deliberately do not come from this listing. */
size_t  provider_models(const Config *cfg, Arena *scratch, Str *out,
                        size_t max, char *err, size_t err_cap);

/* ---- model catalog -------------------------------------------------------
 * Every model every configured provider serves, in one list: what /model
 * offers. An entry is a (provider, model) pair, because that is what a choice
 * is. One id may be served by several providers, and picking an entry picks
 * the connection that serves it.
 *
 * `provider` is empty for this run's own endpoint, which a base URL from a
 * flag, the environment or a config file names without naming a provider.
 * That endpoint is listed only when no provider was chosen, so a session
 * never sees the same models twice under two labels.
 *
 * Listing is one request per provider, made where the picker opens rather
 * than at startup: nothing is asked of an endpoint until the user asks for
 * the list. A provider that cannot be listed lands in `failed` with the
 * reason, and keeps whatever was pinned from it, since pins are local state.
 *
 * Entry providers are the names of `e`, kept by reference, so the Endpoints a
 * catalog was built from must outlive it.
 */
typedef struct {
    Str   *provider;
    Str   *model;
    size_t n, cap;
    Str    failed[AGENT_MAX_ENDPOINTS];
    Str    reason[AGENT_MAX_ENDPOINTS];
    size_t n_failed;
    // Entries were dropped because `cap` was reached, which a caller says.
    b8     full;
} Catalog;


b8     catalog_init(Catalog *c, size_t cap, Arena *a);
/* Both strings are kept by reference, so they must outlive `c`. False when
 * the catalog is full, which also sets `full`. */
b8     catalog_add(Catalog *c, Str provider, Str model);

size_t catalog_endpoints(const Config *cfg, const Endpoints *e, Str *out,
                         size_t max);
/* Named before each endpoint is asked, since one request per provider is a
 * wait worth narrating. An empty name is the run's own endpoint. */
typedef void (*CatalogProgress)(Str provider, void *ud);


size_t catalog_load(Catalog *c, const Config *cfg, const Endpoints *e,
                    size_t cap, Arena *out, CatalogProgress progress,
                    void *ud);

/* ---- context gauge ------------------------------------------------------
 * What the status line's context field reports.
 *
 * A provider states the tokens a request carried, so an exact figure exists
 * only for the conversation as it stood when a response arrived. Everything
 * appended since is estimated from conversation bytes through a fit the
 * current model's own measurements supply: `slope` is what a byte is worth
 * in this model's tokens, `offset` what a request carries beyond the
 * conversation, which is the system prompt and the tool schemas.
 *
 * Before this model has measured anything the field still answers, from the
 * same bytes at a default slope with the tool schemas counted directly. A
 * resumed session holds a conversation whose cost is no mystery, and saying
 * nothing about it until a reply arrives is worse than saying it roughly.
 * Such a figure is never exact, and the first measurement replaces it.
 *
 * The bytes are the ones a request carries rather than the ones the
 * conversation holds: an old tool result the writer elides is charged its
 * note. Counting the full text instead would fit the slope against bytes
 * that were never sent, and would hold the field high over a conversation
 * whose next request is far smaller than its last.
 *
 * The fit describes one model. A model change keeps it as a starting point
 * but drops exactness and the discovered window, so the field reads as an
 * estimate until the new model has measured itself, rather than reporting
 * one tokenizer's count as another's.
 *
 * An image is not bytes of conversation: it is billed by its pixels, and its
 * base64 never appears in what the slope is fitted to. Its estimate is added
 * after the slope and taken back out of a measurement before the slope sees
 * it, so attaching one neither distorts the fit nor goes uncounted.
 */
typedef struct {
    f64    slope;
    f64    offset;
    size_t fit_tokens;
    
    f64    fit_text;
    f64    fit_media;
    f64    fit_bytes;
    size_t exact_slots;
    size_t window;
    const ToolRegistry *tools;
    b8     measured;
    b8     basis;
} CtxGauge;


void ctx_init(CtxGauge *g);
/* The registry whose schemas ride with every request, for the estimate made
 * before a measurement exists. Borrowed for the session; availability is
 * read at each use, so a mode change needs no new call. */
void ctx_set_tools(CtxGauge *g, const ToolRegistry *tools);
/* Fold one request's reported prompt tokens into the fit. `c` must be the
 * conversation that request was built from, which is what it still is while
 * the response streams. A zero count is not a measurement. */
void ctx_note_usage(CtxGauge *g, const Conv *c, size_t prompt_tokens);

void ctx_model_changed(CtxGauge *g);

void ctx_set_window(CtxGauge *g, size_t window);

/* Whether the next request would carry more than `percent` of the window, or
 * more than the window less AGENT_COMPACT_RESERVE, whichever is lower. False
 * when no window is configured: an unknown window cannot be nearly full. */
b8 ctx_over(const CtxGauge *g, const Conv *c, u32 percent);

/* Where to cut a conversation that has to be compacted: the slot the
 * verbatim tail begins at, the oldest round boundary whose tail still fits
 * a budget of AGENT_COMPACT_KEEP_PCT of the window, measured in this model's
 * tokens through the same fit the gauge reports. The budget is capped so at
 * least AGENT_COMPACT_HEAD_PCT of the conversation is left to summarize: a
 * checkpoint that stands for nothing is worth less than the work it
 * replaces. The round in flight is kept whether or not it fits.
 *
 * With no window declared that cap is the whole budget, so a model of
 * unknown size still keeps a tail. Zero when the conversation is too short
 * to have a head at all; a caller that asks anyway summarizes all of it. */
size_t ctx_compact_split(const CtxGauge *g, const Conv *c);

/* Whether summarizing the head that `keep` leaves behind pays for the request
 * it costs: the head must be worth at least as much as the tail budget it
 * makes room for. False holds off the first rounds of a long turn, whose head
 * is one user message, and a conversation whose bulk is the system prompt and
 * the schemas. The head grows as the turn does, so a caller refused now is
 * worth asking again a round later. This gates automatic compaction only: a
 * user who asks for one gets it. */
b8 ctx_compact_worth(const CtxGauge *g, const Conv *c, size_t keep);

void ctx_sync(const CtxGauge *g, const Conv *c);


typedef enum {
    TUI_PLAIN = 0, TUI_HEADING, TUI_CODE, TUI_QUOTE,
    TUI_BOLD, TUI_EMPH, TUI_MONO, TUI_MARKER, TUI_STRIKE
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

TuiCmd tui_separator(Str label);
/* A byte range of the matching row's `desc`, painted as the chosen one of the
 * options that row lists. A zero `n` is a description rather than options.
 * Rows and marks are parallel arrays; the caller owns both. */
typedef struct { size_t off, n; } TuiMark;
void tui_set_commands(const TuiCmd *cmds, size_t n);

typedef struct { Str alias; Str name; } TuiAlias;
void tui_set_aliases(const TuiAlias *aliases, size_t n);

typedef enum { TUI_PICK_FIRST = 0, TUI_PICK_LAST } TuiPickAnchor;

typedef enum {
    TUI_STATUS_STATE, TUI_STATUS_MODEL, TUI_STATUS_REASONING,
    TUI_STATUS_THINKING, TUI_STATUS_MODE, TUI_STATUS_PROVIDER,
    TUI_STATUS_CWD, TUI_STATUS_CONTEXT, TUI_STATUS_COPY,
    TUI_STATUS_PERMISSIONS, TUI_STATUS_N
} TuiStatusItem;

#define TUI_PICK_NONE ((size_t)-1)

void tui_keep_visible(size_t off);
/* Modal picker: the completion popup over a caller-owned list. `title` names
 * it in the status line, Enter chooses (index in *out), Esc/Ctrl-C cancels.
 * `start` opens the selection on one entry, which is how a list carrying a
 * recommendation offers it without reordering itself. Past ten entries typing
 * filters the list by literal substring, leaving the composer's own text
 * untouched. */
b8 tui_pick(Str title, const TuiCmd *items, size_t n, TuiPickAnchor anchor,
            size_t start, size_t *out);

b8 tui_pick_notice(Str title, Str notice, const TuiCmd *items, size_t n,
                   TuiPickAnchor anchor, size_t start, size_t *out);
/* As tui_pick_notice, answering itself with `start` once `timeout_ms` passes
 * with no key pressed; every key restarts the wait. `timeout_ms` of 0, or a
 * `start` outside the list, waits for a key the way tui_pick_notice does.
 * `*expired` reports an answer the deadline gave rather than the reader, and
 * is false whenever this returns false. */
b8 tui_pick_timed(Str title, Str notice, const TuiCmd *items, size_t n,
                  TuiPickAnchor anchor, size_t start, i32 timeout_ms,
                  size_t *out, b8 *expired);

b8 tui_pick_search_count(Str title, const TuiCmd *items, size_t n,
                         size_t search_n, TuiPickAnchor anchor, size_t start,
                         size_t *out);
typedef struct {
    size_t (*act)(void *ud, size_t row, size_t *moved);
    void   *ud;
    i32     key;
} TuiPickBinding;
/* Key actions a chooser offers beside choosing. A binding receives the
 * selected row, or SIZE_MAX when filtering left no selection. It rebuilds
 * the rows and returns their new count; zero closes the picker. `moved`
 * follows a row reordered by the action. `hint` is shown while the picker is
 * open, so its otherwise invisible keys are discoverable. The caller owns
 * every pointer for the duration of the call. */
typedef struct {
    TuiCmd *rows;
    size_t  max;
    const TuiPickBinding *bindings;
    size_t  n_bindings;
    Str     hint;
} TuiPickAction;

b8 tui_pick_action(Str title, size_t n, size_t search_n, TuiPickAnchor anchor,
                   size_t start, const TuiPickAction *act, size_t *out);
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
    TuiMark *marks;
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

b8 tui_info_open(Str title, const TuiCmd *rows, size_t n);

typedef struct { Str text; const YhlResult *syntax; } TuiViewPart;
/* Open a separate, centered text window without borrowing completion or
 * picker state. Nonempty `parts` are joined with one blank row and copied,
 * so the caller may release them when this returns. `start` is the logical
 * line placed at the top initially. The whole copied text remains reachable
 * by scrolling and selectable with the ordinary mouse selection. Escape,
 * Enter, Ctrl-C, Ctrl-D, q, or the visible close control closes it. False on
 * invalid input, insufficient window storage, or another screen being open;
 * failure never opens a clipped window. */
b8 tui_view_open(Str title, const TuiViewPart *parts, size_t n, size_t start);
/* The keybinding tables as info-page rows, grouped by input context with a
 * bracketed heading row per group. Writes at most `max` rows into `rows` and
 * returns how many it wrote; every string is static and outlives the caller,
 * so the rows may be held for as long as the page is open. A `max` short of
 * AGENT_MAX_KEY_ROWS truncates the page rather than failing. */
size_t tui_key_rows(TuiCmd *rows, size_t max);

b8 tui_screen_open(void);

b8 tui_ask(Str question, b8 secret, char *out, size_t cap);

b8 tui_ask_edit(Str question, b8 allow_empty, char *inout, size_t cap);

void tui_set_show_ignored(b8 on);
b8   tui_show_ignored(void);

void tui_set_justify(b8 on);
b8   tui_justify(void);

void tui_set_history(History *h);

void tui_start(Str model, Str base_url, b8 missing_key, b8 setup,
               size_t tool_count, b8 show_ignored, b8 justify,
               u64 status_fields, AgentMode mode, b8 plain);
// The strings the status line names; they must outlive the call.
void tui_set_model(Str model);
void tui_set_mode(AgentMode mode);
void tui_set_permissions(PermissionPolicy policy);
void tui_set_provider(Str name);
void tui_set_reasoning(Str effort, Str thinking_budget);

void tui_set_setup(b8 on);

b8   tui_status_visible(TuiStatusItem item);
void tui_set_status_visible(TuiStatusItem item, b8 visible);

#define NO_PROVIDER_HINT \
    STR("no provider yet: type /provider, then \"+ add a provider\"")
#define NO_MODEL_HINT \
    STR("no model yet: type /model and pick one")
/* The hint the welcome screen ends with, and the reason the status line reads
 * "setup". Empty clears it. The text is not copied, so it must outlive the
 * run; both hints above are literals. */
void tui_set_setup_hint(Str hint);
/* Hand `text` to the terminal's clipboard over OSC 52 and acknowledge it on
 * the status line. False for an empty payload or one past the sequence cap,
 * which is refused rather than truncated. */
b8 tui_copy(Str text);

b8 tui_clipboard_via_tmux(void);
#define AGENT_TMUX_COPY_NOTICE \
    STR("copied; tmux needs `set -s set-clipboard on` to pass it on")
/* Ask the terminal to raise a desktop notification (OSC 9), wrapping the
 * sequence for tmux when running under it, and ring the bell. `text` must
 * already be free of control bytes; both are no-ops off a terminal. */
void tui_desktop_notify(Str text);
void tui_bell(void);
void tui_stop(void);
void tui_set_status(const char *status);
/* The context field. `known` false is the dash a run shows before anything
 * has measured the conversation; `exact` false marks an estimate; `window`
 * is 0 when the endpoint never said what its model's window is, and the
 * field then shows the count alone rather than a share of a number arqan
 * invented. */
void tui_set_context(size_t tokens, b8 known, b8 exact, size_t window);
void tui_clear(void);

void tui_clear_transcript(void);

void tui_zone_begin(u32 id);
void tui_zone_end(void);

void tui_pin(u32 id);

void tui_anchor_zone(u32 id);
void tui_anchor_view(void);
void tui_restore_anchor(void);

void tui_scroll_to_bottom(void);
/* Holds frames while the transcript is rebuilt from the conversation, so a
 * replay paints once rather than once per line it lays down. Not nested. */
void tui_batch_begin(void);
void tui_batch_end(void);
/* One line where the completion popup would be: the answer to a command that
 * opened no popup, retired by the next keystroke. Empty clears it. It is read
 * beside the transcript, so while no screen is open the row it takes is
 * lifted off the transcript rather than covering its newest line. */
void tui_notice(Str msg);

void tui_find_open(void);

void tui_set_find_expand(void (*fn)(void *ud), void *ud);
/* Open a transcript block; the only place the air above one comes from. A
 * block writes no air of its own, so the gap between any two is one blank
 * row, and a trailing newline a writer did emit is absorbed rather than
 * stacked. */
void tui_block(void);
void tui_write(Str s);

void tui_write_text(Str s);

void tui_write_source(Str s);
/* The styles a tool block and a thinking trace are built from: muted for
 * quoted input, output and reasoning, yellow for a call's header, green for a
 * result, red for a failure. Style is a recorded byte range, so a write that
 * overflowed the scrollback loses it. */
void tui_write_muted(Str s);
void tui_write_tool(Str s);
void tui_write_result(Str s);
void tui_write_error(Str s);

void tui_user_begin(void);
void tui_user_end(void);
void tui_write_user(Str s);

void tui_write_styled(Str s, TuiStyle style);

size_t tui_body_cols(void);

size_t tui_text_cells(Str s);

size_t tui_text_fit(Str s, size_t cells, size_t *used);

b8 tui_is_fullscreen(void);

b8     tui_highlight_enabled(void);
size_t tui_transcript_pos(void);
u64    tui_transcript_epoch(void);
void   tui_syntax_add(size_t start, size_t end, u8 semantic);
void   tui_syntax_commit(void);

void tui_set_interrupt_flag(volatile sig_atomic_t *flag);
void tui_printf(const char *fmt, ...) __attribute__((format(printf,1,2)));

b8 tui_readline(const char *prompt, char *buf, size_t cap, size_t *out_n);

void tui_set_input(Str s);
/* The composer's text. TUI-owned and valid until the next edit; empty
 * without a fullscreen UI. A caller that adds to a draft reads it first
 * rather than replacing what the user has written. */
Str  tui_input(void);
/* While a turn is in flight keystrokes are accepted and Enter moves one
 * follow-up into a queue. Callers pump tui_poll_input from wherever they wait.
 * The queued Str is TUI-owned and remains valid until another message is
 * queued; taking it clears the queue but not its bytes. */
void tui_set_busy(b8 busy);
b8   tui_busy(void);
b8   tui_queued_pending(void);
Str  tui_queued_take(void);
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
i32  tui_input_fd(void);

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

void md_set_muted(b8 on);
void md_set_raw(b8 on);
b8   md_raw(void);
b8   md_muted(void);


/* Write one tool call, and later its result, into the transcript. The JSON
 * arguments are parsed in `scratch`, which is rewound before returning, and
 * unparsable ones fall back to the raw text. `result` is the tool's own
 * output, an "ERROR: " prefix included. `id` marks the block as a click
 * target and `expanded` is the state that click left behind, which lifts
 * this block's caps the way /verbose lifts every block's. */
void render_tool_call(Str name, Str args, Arena *scratch, u32 id, b8 expanded);

void render_shell_call(Str cmd, u32 id, b8 expanded);

void render_tool_result(Str name, Str args, Str result, Arena *scratch,
                        u32 id, b8 expanded, u32 ms);

void render_plan(Str plan);
void render_question(Str question);
/* Complete readable input or output for a block's text window. A call may
 * parse `args` in `scratch` and return a string that lives there; result and
 * shell strings are borrowed slices. `shown` is the number of logical lines
 * represented before the block's fold tail, including a first line that its
 * header summarizes, and therefore the line the window initially opens on.
 * `syntax`, when given, receives the highlighter's runs over the returned
 * text and is emptied for text that carries no source. */
Str render_call_text(Str name, Str args, Arena *scratch, size_t *shown,
                     YhlResult *syntax);
Str render_result_text(Str name, Str args, Str result, Arena *scratch,
                       size_t *shown, YhlResult *syntax);
Str render_shell_text(Str cmd, size_t *shown, YhlResult *syntax);

void render_set_verbose(b8 on);
b8   render_verbose(void);

#endif
