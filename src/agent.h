#ifndef AGENT_H
#define AGENT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>
#include <signal.h>
#include <stdio.h>

#include "highlight_protocol.h"


typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float f32;
typedef double f64;
typedef bool b8;

#define AGENT_VERSION "0.7.0"

#define AGENT_NAME       "arqan"
#define AGENT_ENV_PREFIX "ARQAN_"


#define AGENT_ARENA_BYTES        (1u << 27)
#define AGENT_PERSIST_BYTES      (1u << 26)
#define AGENT_SCREEN_BYTES       (1u << 15)
#define AGENT_MAX_MESSAGES       4096
#define AGENT_MAX_TOKENS         32768
#define AGENT_MAX_TOOLS          64
#define AGENT_MAX_TOOL_CALLS     1024
#define AGENT_MAX_TOOL_ARGS      8
#define AGENT_MAX_JSON_DEPTH     64
#define AGENT_MAX_PATH           4096
#define AGENT_MAX_COMMAND        (1u << 16)
#define AGENT_MAX_FILE_BYTES     (16u << 20)
#define AGENT_MAX_IMAGE_BYTES    (5u << 20)
#define AGENT_MAX_IMAGE_SIDE     8000u
#define AGENT_MAX_MEDIA          64
#define AGENT_MAX_MEDIA_PER_TURN 4
#define AGENT_TOOL_RESULT_BYTES  (8u << 10)
#define AGENT_READ_LINES         2000

#define AGENT_READ_BYTES (AGENT_TOOL_RESULT_BYTES - 256u)

#define AGENT_SHELL_OUT_BYTES \
    (AGENT_TOOL_RESULT_BYTES - 256u - AGENT_SPILL_NOTE_BYTES)
#define AGENT_GREP_RESULTS        100
#define AGENT_FIND_RESULTS        200
#define AGENT_GREP_LINE           200
#define AGENT_WALK_DEPTH          32
#define AGENT_WALK_ENTRIES        4096
#define AGENT_WALK_BYTES          (4u << 20)
#define AGENT_IGNORE_PATTERNS     512
#define AGENT_IGNORE_BYTES        (1u << 14)
#define AGENT_MAX_GREP_FILE       (1u << 20)
#define AGENT_MAX_PATCH_FILES     32
#define AGENT_MAX_PATCH_HUNKS     512
#define AGENT_MAX_PATCH_NOTES     4
#define AGENT_PATCH_CONTEXT_LINES 5
#define AGENT_TOOL_ERR            1024

#define AGENT_SPILL_BYTES        (16u << 20)
#define AGENT_SPILL_PATH_MAX     128
#define AGENT_SPILL_NOTE_BYTES   256
#define AGENT_ELIDE_TURNS        2
#define AGENT_ELIDE_ROUNDS       4
#define AGENT_ELIDE_BYTES        512
#define AGENT_ELIDE_AT           75
#define AGENT_ELIDE_MIN_GAIN_PCT 10
#define AGENT_CACHE_TTL_S        300.0
#define AGENT_CACHE_HISTORY      3
#define AGENT_ELIDE_NOTE_BYTES   76
#define AGENT_COMPACT_RESERVE    16384
#define AGENT_COMPACT_KEEP_PCT   30
#define AGENT_COMPACT_HEAD_PCT   50
#define AGENT_COMPACT_AT         85

#define AGENT_RETRIES            4
#define AGENT_RETRY_DELAY_MS     2000
#define AGENT_MAX_RETRY_DELAY_MS 30000

#define AGENT_ASK_TIMEOUT_MS 180000

#define AGENT_SHELL_TIMEOUT_MS 120000

#define AGENT_JOB_WAIT_MS     120000
#define AGENT_JOB_WAIT_MAX_MS 240000
#define AGENT_MAX_JOBS        8

#define AGENT_SUB_BYTES          (4u << 20)
#define AGENT_SUB_MESSAGES       512
#define AGENT_TASK_SLICE_MS      120000
#define AGENT_TASK_LIMIT         1
#define AGENT_TASK_PROMPT_MAX    8192
#define AGENT_TASK_LABEL_MAX     64
#define AGENT_TASK_MODEL_MAX     96
#define AGENT_TASK_PROVIDER_MAX  48
#define AGENT_TASK_LOG_BYTES     (4u << 20)
#define AGENT_TASK_LINE_MAX      (64u << 10)
#define AGENT_TASK_WAIT_MAX_MS   AGENT_JOB_WAIT_MAX_MS
#define AGENT_TASK_DELTA_MS      50
#define AGENT_TASK_GRACE_MS      200
#define AGENT_MAX_TASKS          AGENT_MAX_JOBS
#define AGENT_SUB_REPORT_BYTES   (AGENT_TOOL_RESULT_BYTES - 1024u)
#define AGENT_SUB_PROGRESS_BYTES 500

#define AGENT_JOB_CMD_CHARS      96
#define AGENT_MAX_COMMANDS       32
#define AGENT_LINE_BUF           (1u << 20)
#define AGENT_RESP_BUF           (1u << 22)
#define AGENT_MAX_HISTORY        500
#define AGENT_HISTORY_BYTES      (1u << 20)
#define AGENT_MAX_HISTORY_LINE   (1u << 16)
#define AGENT_MAX_HISTORY_BYTES  (8u << 20)
#define AGENT_MAX_CONFIG_FILES   8
#define AGENT_MAX_PROJECT_FILES  8
#define AGENT_MAX_SETTINGS       512
#define AGENT_MAX_SETTINGS_BYTES (1u << 20)
#define AGENT_MAX_SET_KEYS       8
#define AGENT_MAX_TOOL_LIST      256

#define AGENT_CONFIG_NAME      STR("config.toml")
#define AGENT_STATE_NAME       STR("state.toml")
#define AGENT_CREDENTIALS_NAME STR("credentials.toml")

#define AGENT_PROJECT_DIR       STR("." AGENT_NAME)
#define AGENT_ISSUES_URL        "github.com/bissakov/" AGENT_NAME "/issues"
#define AGENT_MAX_PROMPT_FILE   (1u << 16)
#define AGENT_MAX_AGENTS_FILES  8
#define AGENT_MAX_SESSIONS      64
#define AGENT_MAX_SESSION_BYTES (32u << 20)
#define AGENT_MAX_TITLE         64

#define AGENT_MAX_POPUP  4096
#define AGENT_MAX_MODELS AGENT_MAX_POPUP

#define AGENT_MAX_FAVORITES          64
#define AGENT_MAX_KEY_ROWS           128
#define AGENT_MAX_ENDPOINTS          32
#define AGENT_MAX_ENDPOINT_NAME      64
#define AGENT_MAX_URL                512
#define AGENT_MAX_MODEL_NAME         128
#define AGENT_MAX_API_KEY            512
#define AGENT_MAX_SECRET_ARGV        16
#define AGENT_MAX_SECRET_CMD         512
#define AGENT_SECRET_TIMEOUT_MS      15000
#define AGENT_CLIPBOARD_TIMEOUT_MS   3000
#define AGENT_MAX_REASONING_LIST     1024
#define AGENT_MAX_NOTIFY_TEXT        128
#define AGENT_MAX_NOTIFY_CMD         512
#define AGENT_MAX_NOTIFY_ARGV        16
#define AGENT_MAX_REASONING_TEMPLATE (16u << 10)
#define AGENT_MAX_MODEL_BYTES        (1u << 20)
#define AGENT_MAX_CONTEXT_WINDOW     ((size_t)1 << 31)
#define AGENT_WEB_BODY_BYTES         (2u << 20)
#define AGENT_WEB_URL_BYTES          4096
#define AGENT_WEB_QUERY_BYTES        1025
#define AGENT_WEB_TYPE_BYTES         128
#define AGENT_WEB_SEARCH_INTERVAL_MS 10000
#define AGENT_WEB_SEARCH_PAUSE_MS    3600000

#define AGENT_WEB_USER_AGENT \
    "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0"
#define AGENT_STATUS_FIELDS 11

// ---- arenas -------------------------------------------------------------
typedef struct {
    u8 *base;
    size_t cap;
    size_t off;
} Arena;

void arena_init(Arena *a, void *mem, size_t cap);
void *arena_alloc(Arena *a, size_t n, size_t align);
void *arena_alloc_array(Arena *a, size_t count, size_t size, size_t align);
void arena_reset(Arena *a);
size_t arena_used(const Arena *a);

#define arena_new(a, T, n) \
    ((T *)arena_alloc_array((a), (n), sizeof(T), alignof(T)))


typedef struct {
    const char *p;
    size_t n;
} Str;
#define STR(lit) ((Str){(lit), sizeof(lit) - 1})

Str str_c(const char *z);
Str str_dup(Arena *a, Str s);
Str str_dup_opt(Arena *a, Str s);
b8 str_eq(Str a, Str b);
b8 str_starts(Str s, Str prefix);
Str str_trim(Str s);
Str str_take(Str s, size_t n);
Str str_drop(Str s, size_t n);

b8 str_line(Str s, size_t *off, Str *line);
size_t str_lines(Str s);

Str str_clip_utf8(Str s, size_t max);
size_t utf8_decode(const char *s, size_t n, u32 *cp);

i32 agent_width(u32 cp);
i64 str_int(Str s, b8 *ok);

u64 str_hash64(Str s);

typedef struct {
    char *p;
    size_t n, cap;
    Arena *a;
    b8 oom;
} Buf;
void buf_init(Buf *b, Arena *a, size_t cap);
b8 buf_ok(const Buf *b);
void buf_adopt(Buf *b, Arena *a, Str s);
b8 buf_reserve(Buf *b, size_t need);
void buf_putc(Buf *b, char c);
void buf_put(Buf *b, const void *p, size_t n);
void buf_puts(Buf *b, Str s);
void buf_putf(Buf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void buf_json_str(Buf *b, Str s);

void buf_json_chars(Buf *b, Str s);
void buf_base64(Buf *b, const void *p, size_t n);
Str buf_finish(Buf *b);

/* ---- files ---------------------------------------------------------------
 * The one reader every file arqan owns goes through, so a size that comes from
 * the filesystem is checked in one place rather than at each caller.
 */
typedef enum {
    FILE_OK,
    FILE_MISSING,
    FILE_NOT_REGULAR,
    FILE_TOO_LARGE,
    FILE_NO_MEMORY,
    FILE_UNREADABLE
} FileStatus;


FileStatus file_read(Arena *a, const char *path, size_t max, size_t head,
                     Str *out, u64 *size_out);

typedef b8 (*FileWriteFn)(FILE *f, void *ud);
b8 file_write_atomic(const char *path, u32 mode, b8 sync_parent,
                     FileWriteFn write_fn, void *ud);
b8 file_write_atomic_str(const char *path, Str data, u32 mode, b8 sync_parent);


enum { AGENT_LOG_DEBUG, AGENT_LOG_INFO, AGENT_LOG_WARN, AGENT_LOG_ERROR };
void agent_log(i32 level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void agent_log_set_level(i32 level);

typedef void (*AgentLogSink)(i32 level, Str msg, void *ud);
void agent_log_set_sink(AgentLogSink sink, void *ud);


typedef struct {
    char buf[1024];
    size_t n;
    b8 full, live;
} TelEvent;


void telemetry_init(Arena *scratch, b8 on);
b8 telemetry_on(void);

void telemetry_bind(Str session_path);

void telemetry_detach(void);

void telemetry_close(void);
typedef void (*TelHeader)(void *ud);
void telemetry_set_header(TelHeader fn, void *ud);

b8 telemetry_set(b8 on, Arena *scratch);

Str telemetry_file(void);

void telemetry_log(i32 level, Str msg);

void tel_open(TelEvent *e, const char *ev);
void tel_int(TelEvent *e, const char *key, i64 v);
void tel_bool(TelEvent *e, const char *key, b8 v);

void tel_bucket(TelEvent *e, const char *key, u64 v);

void tel_str(TelEvent *e, const char *key, Str v);

void tel_shape(TelEvent *e, const char *key, Str text);
void tel_arg_keys(TelEvent *e, const char *key, Str args, Arena *scratch);

void tel_hash_field(TelEvent *e, const char *key, Str v);
void tel_send(TelEvent *e);


f64 agent_now_seconds(void);


typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct JVal JVal;
struct JVal {
    Str key;
    JType type;
    union {
        b8 b;
        f64 n;
        Str s;
        struct {
            JVal *items;
            size_t n;
        } arr;
        struct {
            JVal *head;
        } obj;
    } u;
    JVal *next;
};

typedef struct {
    Arena *a;
    const char *src;
    size_t pos, len;
    i32 depth;
    b8 oom;
} JParser;

JVal *json_parse(Arena *a, Str s);
JVal *json_parse_error(Arena *a, Str s, char *err, size_t err_cap);
void json_write(Buf *b, const JVal *v);
const JVal *json_get(const JVal *obj, Str key);
const JVal *json_at(const JVal *arr, size_t i);
Str json_str(const JVal *obj, Str key);

b8 json_bool(const JVal *obj, Str key);

/* ---- XDG base directories ------------------------------------------------
 * Every file arqan owns is resolved here; none sits directly in $HOME. A
 * relative XDG_* value is invalid and ignored, as the spec requires.
 */
typedef enum {
    AGENT_DIR_CONFIG,
    AGENT_DIR_DATA,
    AGENT_DIR_STATE,
    AGENT_DIR_CACHE
} AgentDir;

#define AGENT_SLUG_MAX 200

Str paths_dir(AgentDir kind, Arena *a);
Str paths_file(AgentDir kind, Str name, Arena *a);
b8 paths_ensure_dir(Str dir);

size_t paths_cwd_slug(char *out, size_t cap);

size_t paths_config_files(Str name, Arena *a, Str *out, size_t max);

size_t paths_project_files(Str name, Arena *a, Str *out, size_t max);

Str paths_project_dir(Arena *a);

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
    Str section[AGENT_MAX_SETTINGS];
    Str key[AGENT_MAX_SETTINGS];
    Str val[AGENT_MAX_SETTINGS];
    size_t n;
} Settings;


b8 settings_load(Settings *s, Str path, Arena *a);

Str settings_get(const Settings *s, Str section, Str key);

size_t settings_sections(const Settings *s, Str prefix, Str *out, size_t max);
b8 settings_set(Str path, Str section, const Str *keys, const Str *vals,
                size_t n, u32 mode, Arena *scratch);
b8 settings_set_one(Str path, Str section, Str key, Str val, u32 mode,
                    Arena *scratch);

b8 settings_remove_section(Str path, Str section, Arena *scratch);
b8 settings_write(Str path, Str data, u32 mode);


b8 state_set(Str key, Str val, Arena *scratch);

b8 state_set_many(const Str *keys, const Str *vals, size_t n, Arena *scratch);

b8 state_set_in(Str section, Str key, Str val, Arena *scratch);

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
    Str *entry;
    size_t n, cap;
    size_t cursor;
    Str path;
    Arena *a;
    size_t base_off;
} History;

b8 history_init(History *h, Arena *own, size_t cap);
Str history_path(Arena *a, Arena *scratch);
void history_load(History *h, Str path, Arena *scratch);
void history_rewrite(const History *h);
void history_add(History *h, Str line);
b8 history_prev(History *h, Str *out);

b8 history_next(History *h, Str *out);
void history_reset_cursor(History *h);
b8 history_browsing(const History *h);

/* ---- provider API shapes -------------------------------------------------
 * The two wire formats an endpoint may speak. They differ in the request
 * path, the header the key rides in and the shape of a message, and in
 * nothing above provider.c: a turn, a tool call and a transcript are the
 * same either way.
 */
typedef enum { API_OPENAI = 0, API_ANTHROPIC } ApiKind;

ApiKind api_from_str(Str s);
Str api_name(ApiKind k);


typedef enum {
    SECRET_STORED = 0,
    SECRET_SERVICE,
    SECRET_PASS,
    SECRET_KEYCHAIN,
    SECRET_COMMAND,
} SecretSource;

SecretSource secret_source_from_str(Str s, b8 *known);
Str secret_source_name(SecretSource src);
b8 secret_source_external(SecretSource src);
b8 secret_source_can_store(SecretSource src);


Str secret_lookup(SecretSource src, Str account, Str command, Arena *out,
                  char *err, size_t err_cap);
b8 secret_store(SecretSource src, Str account, Str key, char *err,
                size_t err_cap);

b8 secret_erase(SecretSource src, Str account, char *err, size_t err_cap);

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
    Str name[AGENT_MAX_ENDPOINTS];
    Str base_url[AGENT_MAX_ENDPOINTS];

    Str model[AGENT_MAX_ENDPOINTS];

    Str small_model[AGENT_MAX_ENDPOINTS];
    ApiKind api[AGENT_MAX_ENDPOINTS];
    size_t n;
} Endpoints;

#define ENDPOINT_NONE ((size_t)-1)


size_t endpoints_load(Endpoints *e, Arena *a);
size_t endpoints_find(const Endpoints *e, Str name);

b8 endpoint_name_ok(Str name);

b8 endpoints_put(Endpoints *e, Str name, Str base_url, ApiKind api, Arena *a);

b8 endpoints_save_one(Str name, Str base_url, ApiKind api, Arena *scratch);
Str endpoints_small_model(Str name, Arena *scratch);
Str endpoints_key(Str name, Arena *out, Arena *scratch, char *err,
                  size_t err_cap);

SecretSource endpoints_key_source(Str name, Arena *scratch);

b8 endpoints_set_key(Str name, Str key, SecretSource src, Arena *scratch,
                     char *err, size_t err_cap);

b8 endpoints_delete(Str name, Arena *scratch, char *err, size_t err_cap);

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
    Str provider[AGENT_MAX_FAVORITES];
    Str model[AGENT_MAX_FAVORITES];
    size_t n;
} Favorites;


size_t favorites_load(Favorites *f, const Endpoints *e, Arena *a);
b8 favorites_has(const Favorites *f, Str provider, Str model);
b8 favorites_toggle(Favorites *f, Str provider, Str model, Arena *scratch,
                    b8 *on, char *err, size_t err_cap);

b8 favorites_forget(Str provider, Arena *scratch);

typedef struct {
    Str reasoning_efforts, thinking_budgets;
    Str reasoning_effort, thinking_budget, reasoning_template;
    size_t context_window;
    b8 configured;
} ModelProfile;

void model_profile_load(ModelProfile *p, Str provider, Str model, Arena *out,
                        Arena *scratch);
b8 model_profile_save(Str provider, Str model, const ModelProfile *p,
                      Arena *scratch);

b8 model_profiles_delete(Str provider, Arena *scratch);

/* ---- agent modes ---------------------------------------------------------
 * Build carries the work out; Plan reads and proposes, handing over through
 * the submit_plan tool. The mode decides which system prompt slot 0 holds
 * and which tools the registry offers, so "read-only" is a property of the
 * registry rather than a request made in the prompt.
 */
typedef enum { MODE_BUILD = 0, MODE_PLAN } AgentMode;


typedef enum { PERMISSION_ASK = 0, PERMISSION_FREE } PermissionPolicy;

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
    CONF_PROVIDER,
    CONF_BASE_URL,
    CONF_MODEL,
    CONF_API,
    CONF_API_KEY,
    CONF_MAX_TOKENS,
    CONF_MAX_MESSAGES,
    CONF_STREAM,
    CONF_MODE,
    CONF_PERMISSIONS,
    CONF_RETRIES,
    CONF_RETRY_DELAY_MS,
    CONF_DISABLE_TOOLS,
    CONF_VERBOSE_TOOLS,
    CONF_RAW_MARKDOWN,
    CONF_SHOW_IGNORED,
    CONF_SHOW_INSTRUCTIONS,
    CONF_WRAP,
    CONF_STATUS_FIELDS,
    CONF_TELEMETRY,
    CONF_NOTIFY,
    CONF_NOTIFY_COMMAND,
    CONF_NOTIFY_MIN_MS,
    CONF_SEARCH_BACKEND,
    CONF_SEARCH_ENDPOINT,
    CONF_SEARCH_API_KEY,
    CONF_SEARCH_ENGINE_ID,
    CONF_SMALL_MODEL,
    CONF_SMALL_PROVIDER,
    CONF_AUTO_TITLE,
    CONF_ASK_TIMEOUT_MS,
    CONF_SHELL_TIMEOUT_MS,
    CONF_IMAGES,
    CONF_RESUME_LAST,
    CONF_COMPACT,
    CONF_COMPACT_AT,
    CONF_ELIDE_AT,
    CONF_COMPACT_MODEL,
    CONF_CACHE_GUARD,
    CONF_SUBAGENTS,
    CONF_SUBAGENT_MODEL,
    CONF_SUBAGENT_TASKS,
    CONF_SUBAGENT_SLICE_MS,
    CONF_N
} ConfKey;


typedef enum {
    CONF_FROM_DEFAULT,
    CONF_FROM_SYSTEM,
    CONF_FROM_USER,
    CONF_FROM_PROJECT,
    CONF_FROM_STATE,
    CONF_FROM_ENDPOINT,
    CONF_FROM_ENV
} ConfOrigin;

typedef struct {
    Str val[CONF_N];
    u8 origin[CONF_N];
    ModelProfile model_profile;
} Conf;

void conf_resolve(Conf *c, Arena *persist, Arena *scratch);
Str conf_key_name(ConfKey k);
Str conf_str(const Conf *c, ConfKey k);
i64 conf_num(const Conf *c, ConfKey k);
b8 conf_bool(const Conf *c, ConfKey k);

b8 conf_value_ok(ConfKey k, Str val);

b8 conf_remember(ConfKey k, Str val, Arena *scratch);

b8 conf_remember_pair(ConfKey a, Str va, ConfKey b, Str vb, Arena *scratch);
b8 conf_remember_bool(ConfKey k, b8 on, Arena *scratch);

typedef struct {
    b8 verbose_tools;
    b8 raw_markdown;
    b8 show_ignored;
    b8 show_instructions;
    b8 justify;
    b8 telemetry;
    u64 status_fields;
} UiPrefs;

void ui_prefs_load(UiPrefs *p, const Conf *conf);


typedef struct {
    const char *pat[AGENT_IGNORE_PATTERNS];
    u8 flag[AGENT_IGNORE_PATTERNS];
    u16 base[AGENT_IGNORE_PATTERNS];
    size_t n;
    char buf[AGENT_IGNORE_BYTES];
    size_t buf_n;
} AgentIgnore;

typedef struct {
    size_t n, buf_n;
} AgentIgnoreMark;

void agent_ignore_build(AgentIgnore *ig, Str dir);
void agent_ignore_push(AgentIgnore *ig, const char *dir, size_t dir_n,
                       size_t path_n);
b8 agent_ignore_match(const AgentIgnore *ig, const char *rel, size_t rel_n,
                      b8 is_dir);
AgentIgnoreMark agent_ignore_mark(const AgentIgnore *ig);
void agent_ignore_restore(AgentIgnore *ig, AgentIgnoreMark mark);

void agent_ignore_set_show(b8 on);
b8 agent_ignore_show(void);

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

void notify_init(const Conf *c, Arena *persist);

void notify_event(NotifyKind kind, Str detail, f64 elapsed_ms);


typedef enum { COMPACT_OFF = 0, COMPACT_MANUAL, COMPACT_AUTO } CompactMode;

typedef enum {
    CACHE_GUARD_STOP = 0,
    CACHE_GUARD_WARN,
    CACHE_GUARD_OFF,
} CacheGuardMode;

typedef struct {
    Str base_url;
    Str model;
    Str api_key;
    ApiKind api;
    Str provider;

    Str small_model;

    Str small_provider;
    Str reasoning_efforts, thinking_budgets;
    Str reasoning_effort, thinking_budget;
    Str reasoning_template;
    size_t context_window;
    b8 base_url_set;
    Str system_prompt;
    Str plan_prompt;
    PromptSources system_sources, plan_sources;
    AgentMode mode;
    PermissionPolicy permissions;
    i32 max_tokens;

    size_t max_messages;
    b8 stream;

    i32 retries;
    i32 retry_delay_ms;
    Str disable_tools;

    b8 auto_title;

    b8 images;
    b8 resume_last;
    CompactMode compact;
    u32 compact_at;
    u32 elide_at;
    b8 compact_small;
    CacheGuardMode cache_guard;
    b8 subagents;
    b8 subagent_small;
    i32 subagent_tasks;
    i32 subagent_slice_ms;

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

b8 config_load(Config *c, const Conf *conf, Arena *persist);
b8 config_remember_model(Str provider, Str model, Arena *scratch);

b8 config_set_model(Config *c, Str model);

b8 config_set_small_model(Config *c, Str model, Str provider);
b8 config_set_connection(Config *c, Str name, Str base_url, ApiKind api,
                         Str key);
b8 config_set_endpoint(Config *c, Str name, Str base_url, Str model,
                       ApiKind api, Str key);
b8 config_set_model_profile(Config *c, const ModelProfile *p);
b8 config_set_reasoning(Config *c, b8 effort, Str value);


typedef struct {
    Str base_url, model, api_key, system_prompt;
    Str api;
    Str disable_tools;
    Str prompt;
    b8 have_prompt;
    i32 max_tokens;
    b8 task_worker;
    i32 task_log_fd, task_ctl_fd;
} CliOpts;

typedef enum {
    CLI_RUN,
    CLI_DONE,
    CLI_ERROR,
} CliStatus;

CliStatus cli_parse(i32 argc, char **argv, CliOpts *out);
void cli_apply(const CliOpts *o, Config *c);


#include <curl/curl.h>

#if AGENT_CURL_DLOPEN
typedef struct {
    CURL *(*easy_init)(void);
    CURLcode (*easy_setopt)(CURL *, CURLoption, ...);
    CURLcode (*easy_getinfo)(CURL *, CURLINFO, ...);
    CURLcode (*easy_perform)(CURL *);
    void (*easy_cleanup)(CURL *);
    const char *(*easy_strerror)(CURLcode);
    struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
    void (*slist_free_all)(struct curl_slist *);
    CURLU *(*url)(void);
    CURLUcode (*url_set)(CURLU *, CURLUPart, const char *, unsigned);
    CURLUcode (*url_get)(CURLU *, CURLUPart, char **, unsigned);
    void (*url_cleanup)(CURLU *);
    void (*free)(void *);
    CURLM *(*multi_init)(void);
    CURLMcode (*multi_add_handle)(CURLM *, CURL *);
    CURLMcode (*multi_remove_handle)(CURLM *, CURL *);
    CURLMcode (*multi_perform)(CURLM *, i32 *);
    CURLMcode (*multi_poll)(CURLM *, struct curl_waitfd *, unsigned, i32,
                            i32 *);
    CURLMsg *(*multi_info_read)(CURLM *, i32 *);
    CURLMcode (*multi_cleanup)(CURLM *);
    const char *(*multi_strerror)(CURLMcode);
} CurlApi;

extern CurlApi g_curl;
b8 curl_load(char *err, size_t err_cap);


#undef curl_easy_setopt
#undef curl_easy_getinfo
#define curl_easy_init           g_curl.easy_init
#define curl_easy_setopt         g_curl.easy_setopt
#define curl_easy_getinfo        g_curl.easy_getinfo
#define curl_easy_perform        g_curl.easy_perform
#define curl_easy_cleanup        g_curl.easy_cleanup
#define curl_easy_strerror       g_curl.easy_strerror
#define curl_slist_append        g_curl.slist_append
#define curl_slist_free_all      g_curl.slist_free_all
#define curl_url                 g_curl.url
#define curl_url_set             g_curl.url_set
#define curl_url_get             g_curl.url_get
#define curl_url_cleanup         g_curl.url_cleanup
#define curl_free                g_curl.free
#define curl_multi_init          g_curl.multi_init
#define curl_multi_add_handle    g_curl.multi_add_handle
#define curl_multi_remove_handle g_curl.multi_remove_handle
#define curl_multi_perform       g_curl.multi_perform
#define curl_multi_poll          g_curl.multi_poll
#define curl_multi_info_read     g_curl.multi_info_read
#define curl_multi_cleanup       g_curl.multi_cleanup
#define curl_multi_strerror      g_curl.multi_strerror
#else
#define curl_load(err, cap) ((void)(err), (void)(cap), true)
#endif

typedef struct {
    const char *base_url;
    const char *api_key;

    ApiKind api;

    b8 (*on_line)(Str line, void *ud);
    void *ud;
    Arena *line_arena;

    Buf *body_out;
    const char *body;
    const volatile sig_atomic_t *interrupt_flag;
    i32 idle_fd;
    void (*on_idle)(void *ud);
    void *idle_ud;

    char *fail_out;
    size_t fail_cap;
} HttpReq;

i32 http_post(const HttpReq *r);
i32 http_get(const char *base_url, const char *path, const char *api_key,
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

i32 http_url_get(HttpUrlReq *r);

#ifdef AGENT_TESTING

void http_print_ca_trust(void);
#endif

/* ---- spilled tool output -------------------------------------------------
 * The full output of a tool whose result is a page, written to
 * "$TMPDIR/arqan-<tool>-<hash>.<ext>" at mode 0600 so a later call can
 * narrow it without replaying it. Best effort throughout: every entry point is a
 * no-op once the spill has failed or was never opened, and the tool answers
 * as it would without one. No arena: a Spill owns only its file. */
typedef struct {
    char path[AGENT_SPILL_PATH_MAX];
    i32 fd;
    size_t written;
    size_t buf_n;
    b8 full;
    char buf[4096];
} Spill;


void spill_open(Spill *s, const char *tool, const char *ext, Str key);
void spill_put(Spill *s, const char *p, size_t n);
void spill_putf(Spill *s, const char *fmt, ...);
void spill_finish(Spill *s, Buf *out, b8 keep);
i32 spill_release(Spill *s, char *path, size_t path_cap, size_t *written);

void spill_size_text(char *z, size_t cap, size_t n);


typedef b8 (*ToolRun)(Str args_json, Arena *scratch, Buf *out, char *err,
                      size_t err_cap);


#define TOOL_IN_BUILD 1u
#define TOOL_IN_PLAN  2u
#define TOOL_FIXED    4u

#define TOOL_INTERACTIVE 8u
#define TOOL_IN_SUB      16u

typedef enum { TOOL_FOR_MAIN, TOOL_FOR_SUB } ToolAudience;

typedef struct {
    Str *name;
    Str *desc;
    Str *brief;
    Str *schema;
    ToolRun *run;
    u8 *modes;
    u8 *approval;
    b8 *off;
    size_t n;
} ToolRegistry;


#define TOOL_NONE ((size_t)-1)

void tools_init(ToolRegistry *r, Arena *persist, i32 shell_timeout_ms,
                b8 subagents, i32 subagent_tasks);

void tools_set_subagents(ToolRegistry *r, b8 on);

void tools_set_task_limit(ToolRegistry *r, i32 tasks);

void tools_set_mode(AgentMode mode);

void tools_set_interactive(b8 interactive);

b8 tools_available(const ToolRegistry *r, size_t id, AgentMode mode);
b8 tools_available_to(const ToolRegistry *r, size_t id, AgentMode mode,
                      ToolAudience audience);
size_t tools_find(const ToolRegistry *r, Str name);
ToolApprovalClass tools_approval_class(const ToolRegistry *r, size_t id);

Str tools_approval_name(ToolApprovalClass approval);

b8 tools_can_disable(const ToolRegistry *r, size_t id);
b8 tools_disabled(const ToolRegistry *r, size_t id);
void tools_set_disabled(ToolRegistry *r, size_t id, b8 off);

b8 tools_disable_list(ToolRegistry *r, Str names, char *err, size_t err_cap);
b8 tools_run(const ToolRegistry *r, size_t id, Str args,
             ToolAuthorization authorization, Arena *scratch, Buf *out,
             char *err, size_t err_cap, ToolAudience audience);

void tools_write_schemas(Buf *b, const ToolRegistry *r, ApiKind api,
                         ToolAudience audience);
size_t tools_schema_bytes(const ToolRegistry *r, ToolAudience audience);

void web_set_idle(void (*fn)(void *ud), void *ud, i32 idle_fd,
                  const volatile sig_atomic_t *interrupt_flag);
void web_search_init(const Conf *c, Arena *persist);
b8 internet_search_run(Str args, Arena *scratch, Buf *out, char *err,
                       size_t err_cap);
b8 page_fetch_run(Str args, Arena *scratch, Buf *out, char *err,
                  size_t err_cap);
b8 shell_capture(Str cmd, Buf *out, char *err, size_t err_cap);
void shell_set_idle(void (*fn)(void *ud), void *ud);
void shell_set_interrupt_flag(volatile sig_atomic_t *flag);

void shell_set_timeout(i32 ms);
void jobs_stop(void);


Str prompt_build(const ToolRegistry *tools, Str configured, Arena *persist,
                 Arena *scratch, PromptSources *sources, char *err,
                 size_t err_cap);

Str prompt_build_plan(const ToolRegistry *tools, Arena *persist, Arena *scratch,
                      PromptSources *sources, char *err, size_t err_cap);
Str prompt_compact(void);
Str prompt_sub(const ToolRegistry *tools, AgentMode mode, Arena *a);
Str prompt_compact_ask(void);

Str prompt_title(void);

Str prompt_title_ask(void);

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
    Str *mime;
    Str *bytes;
    Str *b64;
    Str *label;

    Str *file;
    u32 *w, *h;
    size_t n, cap;
    Arena *arena;
} MediaSet;

#define MEDIA_NONE ((size_t)-1)

b8 media_init(MediaSet *m, Arena *persist, size_t cap);

b8 media_sniff(Str bytes, Str *mime, u32 *w, u32 *h);

Str media_ext(Str mime);
size_t media_add(MediaSet *m, Arena *persist, Str bytes, Str label, char *err,
                 size_t err_cap);
size_t media_add_file(MediaSet *m, Arena *persist, Arena *scratch, Str path,
                      char *err, size_t err_cap);

size_t media_add_missing(MediaSet *m, Arena *persist, Str label, Str mime,
                         Str file);
b8 media_live(const MediaSet *m, size_t id);

size_t media_keep(MediaSet *m, size_t base, const size_t *ids, size_t n);

void media_describe(char *out, size_t cap, const MediaSet *m, size_t id);

void media_write_openai(Buf *b, const MediaSet *m, size_t id);
void media_write_anthropic(Buf *b, const MediaSet *m, size_t id);

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
b8 clipboard_image(Arena *scratch, Str *out, char *err, size_t err_cap);


typedef enum { M_SYSTEM = 0, M_USER, M_ASSISTANT, M_TOOL } MRole;


#define CONV_NONE ((size_t)-1)

typedef struct {
    MRole *role;
    Str *text;
    Str *anthropic_thinking;
    Str *tool_name;
    Str *tool_call_id;
    Str *shell_out;
    b8 *has_tool_call;
    b8 *expanded;
    b8 *args_object;

    u32 *ms;
    u32 *media_off;
    u16 *media_n;
    MediaSet *media;
    size_t elide_start;
    size_t checkpoint;
    size_t n, cap;
} Conv;

b8 conv_init(Conv *c, Arena *persist, size_t cap);
size_t conv_add(Conv *c, MRole role, Str text);

void conv_set_media(Conv *c, MediaSet *m);
void conv_attach_media(Conv *c, size_t i, size_t off, size_t n);
void conv_truncate(Conv *c, size_t keep);

size_t conv_add_assistant_calls(Conv *c, Str content);
size_t conv_add_call(Conv *c, Arena *scratch, Str id, Str name, Str args);
size_t conv_add_tool(Conv *c, Str tool_call_id, Str text);
size_t conv_add_shell(Conv *c, Str cmd, Str out);
b8 conv_is_shell(const Conv *c, size_t i);
b8 conv_is_call(const Conv *c, size_t i);
size_t conv_room(const Conv *c);
b8 conv_clone(Conv *dst, const Conv *src, Arena *a, size_t extra);
b8 conv_clone_head(Conv *dst, const Conv *src, size_t keep, Arena *a,
                   size_t extra);

void conv_write_json(Buf *b, const Conv *c, const ToolRegistry *reg);
void conv_write_json_anthropic(Buf *b, const Conv *c);

size_t conv_elide_start(const Conv *c);
size_t conv_elide_next(const Conv *c);
b8 conv_elide_advance(Conv *c);
b8 conv_result_elided(const Conv *c, size_t i, size_t recent);
b8 conv_slot_dropped(const Conv *c, size_t i, size_t recent);
b8 conv_args_elided(const Conv *c, size_t i, size_t recent);
b8 conv_args_are_stub(Str args, Arena *scratch);

b8 conv_round_start(const Conv *c, size_t i);

b8 conv_compact_head(Conv *c, size_t keep, Str checkpoint);
void conv_set_checkpoint(Conv *c, size_t i);

/* ---- todo list -----------------------------------------------------------
 * The step list the model keeps for work that spans several rounds. The tool
 * takes the whole list on every call, so the arguments of the last todo call
 * in `Conv` are the state, and replay, resume and rewind rebuild it through
 * todo_rebuild rather than persisting anything of their own. A TodoList owns
 * its text and holds no arena pointers, so it can be assigned whole.
 */
#define AGENT_MAX_TODOS        20
#define AGENT_MAX_TODO_TEXT    100
#define AGENT_TODO_NONE        ((size_t)-1)
#define AGENT_TODO_STALE_CALLS 8
#define AGENT_TODO_COLD_CALLS  12

typedef enum { TODO_PENDING = 0, TODO_ACTIVE, TODO_DONE } TodoStatus;

typedef struct {
    char text[AGENT_MAX_TODOS][AGENT_MAX_TODO_TEXT];
    u8 len[AGENT_MAX_TODOS];
    u8 status[AGENT_MAX_TODOS];
    size_t n;
} TodoList;

b8 todo_parse(Str args_json, Arena *scratch, TodoList *out, char *err,
              size_t err_cap);
Str todo_text(const TodoList *l, size_t i);
size_t todo_active(const TodoList *l);
size_t todo_done(const TodoList *l);
b8 todo_same_items(const TodoList *a, const TodoList *b);
b8 todo_prev(const Conv *c, size_t slot, Arena *scratch, TodoList *out);
void todo_summary(Buf *b, const TodoList *l);
void todo_write_md(Buf *b, const TodoList *l);
b8 todo_parse_md(Str doc, TodoList *out);

b8 todo_run(Str args_json, Arena *scratch, Buf *out, char *err, size_t err_cap);
const TodoList *todo_current(void);
void todo_clear(void);
/* Appends a line asking for the current list to the result of the tool named
 * `tool`, once AGENT_TODO_STALE_CALLS results have answered other tools with
 * work unfinished. Writes nothing when there is no list, when every item is
 * done, or when the count is short. INVARIANT: the count lives here, so every
 * tool result must pass through this, and a `todo` result resets it. */
void todo_note_stale(Str tool, Buf *out);
/* Drops a trailing note written by todo_note_stale. The note is addressed to
 * the model and stays in the conversation, so the transcript strips it rather
 * than reading it as part of what the tool answered. */
Str todo_note_strip(Str result);
/* Asks for a first list instead, once a turn with no list has run
 * AGENT_TODO_COLD_CALLS results and changed something. Silent for a session
 * that has ever called the tool, and asks at most once, since a model that
 * declined has answered. INVARIANT: the turn count resets in todo_turn_begin,
 * which every turn must call. */
void todo_turn_begin(void);
void todo_telemetry(TelEvent *e);
/* Derives the current list from the last todo call in `c`, clearing it when
 * there is none. Cheap when the call it last read is still the last one, so
 * every path that changes history can call it: resume, /clear, rewind,
 * compaction. INVARIANT: nothing else writes the list except todo_run, which
 * marks it for rederivation, so a missed call goes stale for at most one
 * conversation change. Borrows `scratch` and restores it. */
void todo_sync(const Conv *c, Arena *scratch);

/* ---- sessions ------------------------------------------------------------
 * The conversation as it happened, one JSON object per line under
 * $XDG_DATA_HOME/arqan/sessions/<cwd>/<timestamp>.jsonl, keyed by the
 * directory arqan was launched in so browsing from one project never surfaces
 * another's.
 *
 * Paths live in the struct instead of an arena because /clear rewinds the
 * session arena and the file the next message appends to has to outlive it.
 *
 * One instance at a time appends to a session. While a session is live its
 * instance holds an advisory lock on a file named after it under
 * $XDG_STATE_HOME/arqan/locks/<cwd>/, so a second instance that resumes the
 * same transcript gets `read_only`: it renders and exports the conversation
 * but appends nothing. The lock sits beside the data rather than on the
 * session file because a title write replaces that file.
 */
typedef struct {
    char dir_buf[AGENT_MAX_PATH];
    char lock_dir_buf[AGENT_MAX_PATH];
    char path_buf[AGENT_MAX_PATH];
    char name_buf[32];

    char title_buf[AGENT_MAX_TITLE + 1];
    Str dir;
    Str lock_dir;
    Str path;
    Str name;
    Str title;

    i32 lock_fd;
    b8 read_only;
    b8 resumed;
    b8 title_tried;
    b8 save_blocked;
    b8 sync_dir;
    b8 cleared;
    size_t written;
    size_t elide_written;
} Session;

typedef struct {
    Str *name;
    Str *path;
    Str *preview;
    Str *title;
    b8 *live;
    size_t n;
} SessionList;

b8 session_init(Session *s, Arena *scratch);
b8 session_begin(Session *s);
void session_set_cleared(Session *s, b8 cleared);
b8 session_save(Session *s, const Conv *c, char *err, size_t err_cap);

b8 session_fork(Session *s, const Conv *c, char *err, size_t err_cap);

b8 session_export_markdown(const Conv *c, Str requested, char *path,
                           size_t path_cap, char *err, size_t err_cap);

size_t session_list(const Session *s, Arena *a, SessionList *out, size_t max);
b8 session_delete(const Session *s, Str path);
Str session_read(Str path, Arena *scratch);
b8 session_apply(Session *s, Str src, Str path, Str name, Conv *c,
                 Arena *persist, Arena *scratch);

b8 session_set_title(Session *s, Str title);


typedef struct {
    const Config *cfg;
    const ToolRegistry *tools;
    Conv *conv;
    Arena *persist;
    Arena *scratch;
    ToolAudience audience;
    void (*on_text)(Str delta, void *ud);

    void (*on_reason)(Str delta, void *ud);
    void (*on_tool_call)(i32 index, Str id, Str name, Str args_delta, void *ud);
    void (*on_usage)(const Conv *conv, size_t prompt_tokens,
                     size_t completion_tokens, void *ud);

    void (*on_retry)(i32 attempt, i32 attempts, i32 delay_ms, Str reason,
                     void *ud);
    void *ud;

    void (*on_idle)(void *ud);
    i32 idle_fd;
    const volatile sig_atomic_t *interrupt_flag;
    size_t prompt_tokens;
    size_t completion_tokens;
    size_t cache_creation_tokens;
    size_t cache_read_tokens;
    size_t total_tokens;
    b8 usage_valid;
} Provider;


#define PROVIDER_EMPTY (-2)
i32 provider_run(Provider *p, char *err, size_t err_cap);

/* ---- subagents -----------------------------------------------------------
 * A nested agent the `task` tool answers with: its own conversation, its own
 * system prompt and the read-only slice of the registry. The parent's
 * transcript keeps one call and one result, and the parent's context gauge
 * measures the parent's conversation alone; what the delegate does reaches
 * the caller through the SubRun hooks.
 *
 * A run is cut into slices at round boundaries. An unfinished subagent is
 * parked rather than discarded, its conversation kept whole in its own arena,
 * and the parent polls it with another `task` call. Nothing is re-run, and
 * each poll is a cheap parent request that re-warms the prefix.
 */
typedef struct {
    Arena a;
    Conv conv;
    u32 id, rounds, tool_calls, slices;
    size_t prompt_tokens, completion_tokens;
    f64 started;
    f64 slowest_round_s;
    Str last_tool;
    b8 live;
    b8 kept;
    b8 small;
    char label[AGENT_TASK_LABEL_MAX];
    char model[AGENT_TASK_MODEL_MAX];
    char provider[AGENT_TASK_PROVIDER_MAX];
} Subagent;

typedef struct {
    const Config *cfg;
    const ToolRegistry *tools;
    Arena *scratch;
    f64 deadline_s;
    const volatile sig_atomic_t *interrupt_flag;
    i32 idle_fd;
    void (*on_idle)(void *ud);
    void (*on_retry)(i32 attempt, i32 attempts, i32 delay_ms, Str reason,
                     void *ud);
    void (*on_step)(const Conv *c, size_t slot, u32 round, void *ud);
    void (*on_result)(const Conv *c, size_t slot, u32 ms, void *ud);
    void (*on_round_begin)(u32 round, void *ud);
    void (*on_round_end)(const Conv *c, size_t first, void *ud);
    void (*on_text)(Str delta, void *ud);
    void (*on_reason)(Str delta, void *ud);
    void *ud;
} SubRun;

typedef enum {
    SUB_REPORTED,
    SUB_PARKED,
    SUB_INTERRUPTED,
    SUB_EXHAUSTED,
    SUB_FAILED
} SubOutcome;

void subagent_release(Subagent *s);
void subagent_retire(Subagent *s);
void subagent_set_model(Subagent *s, Str model, Str provider, b8 small);
b8 subagent_begin(Subagent *s, void *mem, size_t cap, u32 id, Str system,
                  Str task, Str label, char *err, size_t err_cap);
SubOutcome subagent_slice(Subagent *s, const SubRun *r, Buf *out, char *err,
                          size_t err_cap);
void subagent_cost(Buf *out, const Subagent *s);
void subagent_label(Buf *out, const Subagent *s);
void subagent_progress(Buf *out, const Subagent *s);

/* ---- task workers ------------------------------------------------------ */
typedef enum {
    TASK_EV_NONE,
    TASK_EV_START,
    TASK_EV_ROUND,
    TASK_EV_MSG,
    TASK_EV_CALL,
    TASK_EV_RESULT,
    TASK_EV_USAGE,
    TASK_EV_END,
} TaskEventKind;

typedef struct {
    TaskEventKind kind;
    u32 id, n, slot, ms, rounds, tool_calls;
    size_t prompt_tokens, completion_tokens;
    b8 small, assistant;
    Str label, model, provider, task, name, args, text, outcome;
} TaskEvent;

typedef struct {
    i32 fd;
    size_t written;
    b8 full;
} TaskLog;

typedef struct {
    i32 fd;
    size_t line_n;
    b8 overflow;
    char line[AGENT_TASK_LINE_MAX];
} TaskReader;

typedef void (*TaskOnEvent)(const TaskEvent *e, void *ud);

void tasklog_init(TaskLog *l, i32 fd);
void tasklog_write(TaskLog *l, const TaskEvent *e, Arena *scratch);

void tasklog_reader_init(TaskReader *r, i32 fd);
b8 tasklog_parse(Str line, Arena *scratch, TaskEvent *out);
size_t tasklog_read(TaskReader *r, Arena *scratch, TaskOnEvent on, void *ud);

/* INVARIANT: `reader` and `report` sit last, and a slot is cleared only once
 * it has run. A slot is 72KB, so clearing all of them would dirty 580KB of
 * `.bss` in a session that never delegates. */
typedef struct {
    Subagent sub;
    pid_t pid;
    i32 lifeline;
    f64 started;
    b8 running, ended;
    b8 fallback;
    SubOutcome outcome;
    u32 round;
    size_t report_n;
    char path[AGENT_SPILL_PATH_MAX];
    TaskReader reader;
    char report[AGENT_TOOL_RESULT_BYTES];
} TaskWorker;

size_t provider_models(const Config *cfg, Arena *scratch, Str *out, size_t max,
                       char *err, size_t err_cap);

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
    Str *provider;
    Str *model;
    size_t n, cap;
    Str failed[AGENT_MAX_ENDPOINTS];
    Str reason[AGENT_MAX_ENDPOINTS];
    size_t n_failed;
    b8 full;
} Catalog;


b8 catalog_init(Catalog *c, size_t cap, Arena *a);
b8 catalog_add(Catalog *c, Str provider, Str model);

size_t catalog_endpoints(const Config *cfg, const Endpoints *e, Str *out,
                         size_t max);
typedef void (*CatalogProgress)(Str provider, void *ud);


size_t catalog_load(Catalog *c, const Config *cfg, const Endpoints *e,
                    size_t cap, Arena *out, CatalogProgress progress, void *ud);

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
    f64 slope;
    f64 offset;
    size_t fit_tokens;

    f64 fit_text;
    f64 fit_media;
    f64 fit_bytes;
    size_t exact_slots;
    size_t window;
    const ToolRegistry *tools;
    b8 measured;
    b8 basis;
} CtxGauge;


void ctx_init(CtxGauge *g);
void ctx_set_tools(CtxGauge *g, const ToolRegistry *tools);
void ctx_note_usage(CtxGauge *g, const Conv *c, size_t prompt_tokens);

void ctx_model_changed(CtxGauge *g);

void ctx_set_window(CtxGauge *g, size_t window);

b8 ctx_over(const CtxGauge *g, const Conv *c, u32 percent);

size_t ctx_compact_split(const CtxGauge *g, const Conv *c);

b8 ctx_compact_worth(const CtxGauge *g, const Conv *c, size_t keep);

size_t ctx_elide_gain(const CtxGauge *g, const Conv *c);


/* ---- prompt cache guard --------------------------------------------------
 * The provider replays the whole conversation on every request and keeps the
 * prefix of the previous one, matched from the first byte. Anything that
 * rewrites text already sent throws that prefix away and rebuilds it at
 * 12.5x what reading it back would have cost, so every rewrite is deliberate
 * and every miss has a name.
 *
 * A miss whose cause was stamped is the price of a rewrite that paid for
 * itself. A miss with no cause is a defect: something is rewriting the
 * conversation that nobody declared, and the tool loop stops rather than
 * spend a session's budget rebuilding the same prefix round after round.
 */
typedef enum {
    CACHE_CAUSE_NONE = 0,
    CACHE_CAUSE_FIRST,
    CACHE_CAUSE_ELIDE,
    CACHE_CAUSE_COMPACT,
    CACHE_CAUSE_MODE,
    CACHE_CAUSE_TOOLS,
    CACHE_CAUSE_MODEL,
    CACHE_CAUSE_MEDIA,
    CACHE_CAUSE_RESUME,
    CACHE_CAUSE_TTL,
    CACHE_CAUSE_TRAIL,
} CacheCause;

typedef struct {
    size_t expect_tokens;
    size_t older_tokens[AGENT_CACHE_HISTORY];
    f64 last_send_s;
    CacheCause cause;
    size_t freed_tokens;
    size_t misses;
    size_t wasted_tokens;
    b8 armed;
} CacheGuard;

Str cache_cause_name(CacheCause cause);

void ctx_sync(const CtxGauge *g, const Conv *c);


typedef enum {
    TUI_PLAIN = 0,
    TUI_HEADING,
    TUI_CODE,
    TUI_QUOTE,
    TUI_BOLD,
    TUI_EMPH,
    TUI_MONO,
    TUI_MARKER,
    TUI_STRIKE
} TuiStyle;

typedef enum {
    YHL_HINT_MARKDOWN_ALIAS = YHL_HINT_ALIAS,
    YHL_HINT_PATH = YHL_HINT_FILENAME,
} YhlHintKind;

typedef struct {
    u32 start, end;
    u8 semantic;
} YhlRun;
typedef struct {
    YhlRun run[YHL_RUN_MAX];
    size_t n;
} YhlResult;

void highlight_init(const char *argv0);
b8 highlight_request(YhlHintKind kind, Str hint, Str source, YhlResult *result);
void highlight_close(void);
typedef struct {
    Str name;
    Str desc;
} TuiCmd;

TuiCmd tui_separator(Str label);
typedef struct {
    size_t off, n;
} TuiMark;
void tui_set_commands(const TuiCmd *cmds, size_t n);

typedef struct {
    Str alias;
    Str name;
} TuiAlias;
void tui_set_aliases(const TuiAlias *aliases, size_t n);

typedef enum { TUI_PICK_FIRST = 0, TUI_PICK_LAST } TuiPickAnchor;

typedef enum {
    TUI_STATUS_STATE,
    TUI_STATUS_MODEL,
    TUI_STATUS_REASONING,
    TUI_STATUS_THINKING,
    TUI_STATUS_MODE,
    TUI_STATUS_PROVIDER,
    TUI_STATUS_CWD,
    TUI_STATUS_CONTEXT,
    TUI_STATUS_COPY,
    TUI_STATUS_PERMISSIONS,
    TUI_STATUS_TODO,
    TUI_STATUS_N
} TuiStatusItem;

#define TUI_PICK_NONE ((size_t)-1)

void tui_keep_visible(size_t off);
b8 tui_pick(Str title, const TuiCmd *items, size_t n, TuiPickAnchor anchor,
            size_t start, size_t *out);

b8 tui_pick_notice(Str title, Str notice, const TuiCmd *items, size_t n,
                   TuiPickAnchor anchor, size_t start, size_t *out);
b8 tui_pick_timed(Str title, Str notice, const TuiCmd *items, size_t n,
                  TuiPickAnchor anchor, size_t start, i32 timeout_ms,
                  size_t *out, b8 *expired);

b8 tui_pick_search_count(Str title, const TuiCmd *items, size_t n,
                         size_t search_n, TuiPickAnchor anchor, size_t start,
                         size_t *out);
typedef struct {
    size_t (*act)(void *ud, size_t row, size_t *moved);
    void *ud;
    i32 key;
} TuiPickBinding;
typedef struct {
    TuiCmd *rows;
    size_t max;
    const TuiPickBinding *bindings;
    size_t n_bindings;
    Str hint;
} TuiPickAction;

b8 tui_pick_action(Str title, size_t n, size_t search_n, TuiPickAnchor anchor,
                   size_t start, const TuiPickAction *act, size_t *out);
typedef struct {
    TuiCmd *rows;
    TuiMark *marks;
    size_t max;
    size_t (*build)(void *ud);
    void (*act)(void *ud, size_t row, i32 delta);
    void *ud;
} TuiSettings;
void tui_settings(Str title, const TuiSettings *set);
b8 tui_settings_open(Str title, const TuiSettings *set);
void tui_info(Str title, const TuiCmd *rows, size_t n);

b8 tui_info_open(Str title, const TuiCmd *rows, size_t n);

typedef struct {
    Str text;
    const YhlResult *syntax;
} TuiViewPart;
b8 tui_view_open(Str title, const TuiViewPart *parts, size_t n, size_t start);
size_t tui_key_rows(TuiCmd *rows, size_t max);

b8 tui_screen_open(void);

b8 tui_ask(Str question, b8 secret, char *out, size_t cap);

b8 tui_ask_edit(Str question, b8 allow_empty, char *inout, size_t cap);

void tui_set_show_ignored(b8 on);
b8 tui_show_ignored(void);

void tui_set_justify(b8 on);
b8 tui_justify(void);

void tui_set_history(History *h);

void tui_start(Str model, Str base_url, b8 missing_key, b8 setup,
               size_t tool_count, b8 show_ignored, b8 justify,
               u64 status_fields, AgentMode mode, b8 plain);
void tui_set_model(Str model);
void tui_set_mode(AgentMode mode);
void tui_set_permissions(PermissionPolicy policy);
void tui_set_provider(Str name);
void tui_set_reasoning(Str effort, Str thinking_budget);

void tui_set_setup(b8 on);

b8 tui_status_visible(TuiStatusItem item);
void tui_set_status_visible(TuiStatusItem item, b8 visible);

#define NO_PROVIDER_HINT \
    STR("no provider yet: type /provider, then \"+ add a provider\"")
#define NO_MODEL_HINT STR("no model yet: type /model and pick one")
void tui_set_setup_hint(Str hint);
b8 tui_copy(Str text);

b8 tui_clipboard_via_tmux(void);
#define AGENT_TMUX_COPY_NOTICE \
    STR("copied; tmux needs `set -s set-clipboard on` to pass it on")
void tui_desktop_notify(Str text);
void tui_bell(void);
void tui_stop(void);
void tui_set_status(const char *status);
void tui_set_context(size_t tokens, b8 known, b8 exact, size_t window);
void tui_set_todo(size_t done, size_t total);
void tui_clear(void);

void tui_clear_transcript(void);

void tui_transcript_detach(void);
void tui_transcript_attach(void);
b8 tui_transcript_detached(void);

void tui_zone_begin(u32 id);
void tui_zone_end(void);

void tui_pin(u32 id);

void tui_anchor_zone(u32 id);
void tui_anchor_view(void);
void tui_restore_anchor(void);

void tui_scroll_to_bottom(void);
void tui_batch_begin(void);
void tui_batch_end(void);
void tui_notice(Str msg);

void tui_find_open(void);

void tui_set_find_expand(void (*fn)(void *ud), void *ud);
void tui_width_fitted(void);
void tui_set_reflow(void (*fn)(void *ud), void *ud);
void tui_block(void);
void tui_write(Str s);

void tui_write_text(Str s);

void tui_write_source(Str s);
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

b8 tui_highlight_enabled(void);
size_t tui_transcript_pos(void);
u64 tui_transcript_epoch(void);
void tui_syntax_add(size_t start, size_t end, u8 semantic);
void tui_syntax_commit(void);

void tui_set_interrupt_flag(volatile sig_atomic_t *flag);
void tui_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

b8 tui_readline(const char *prompt, char *buf, size_t cap, size_t *out_n);

void tui_set_input(Str s);
Str tui_input(void);
void tui_set_busy(b8 busy);
b8 tui_busy(void);
b8 tui_queued_pending(void);
Str tui_queued_take(void);
void tui_set_busy_command(b8 (*fn)(Str line, void *ud), void *ud);
void tui_activity(Str label);
void tui_activity_end(void);
void tui_poll_input(void);
void tui_set_tick(b8 (*fn)(void *ud), void *ud);
i32 tui_input_fd(void);

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
b8 md_raw(void);
b8 md_muted(void);


void render_tool_call(Str name, Str args, Arena *scratch, u32 id, b8 expanded,
                      const Conv *c, size_t slot);

void render_shell_call(Str cmd, u32 id, b8 expanded);

void render_tool_result(Str name, Str args, Str result, Arena *scratch, u32 id,
                        b8 expanded, u32 ms);

void render_plan(Str plan);
void render_question(Str question);
void render_task_header(u32 id, Str label, Str model, Str provider, b8 small,
                        b8 live);
Str render_call_text(Str name, Str args, Arena *scratch, size_t *shown,
                     YhlResult *syntax);
Str render_result_text(Str name, Str args, Str result, Arena *scratch,
                       size_t *shown, YhlResult *syntax);
Str render_shell_text(Str cmd, size_t *shown, YhlResult *syntax);

void render_set_verbose(b8 on);
b8 render_verbose(void);

#endif
