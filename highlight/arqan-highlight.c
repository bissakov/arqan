// arqan-highlight: bounded Tree-sitter highlighting companion.
#define _POSIX_C_SOURCE 200809L

#include "highlight_protocol.h"
#include "queries.h"
#include <tree_sitter/api.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define YHL_LANG_COUNT 12u
#define YHL_WORK_MAX   (8u << 20)

typedef const TSLanguage *(*LanguageFn)(void);

const TSLanguage *tree_sitter_c(void);
const TSLanguage *tree_sitter_cpp(void);
const TSLanguage *tree_sitter_rust(void);
const TSLanguage *tree_sitter_go(void);
const TSLanguage *tree_sitter_python(void);
const TSLanguage *tree_sitter_javascript(void);
const TSLanguage *tree_sitter_typescript(void);
const TSLanguage *tree_sitter_tsx(void);
const TSLanguage *tree_sitter_bash(void);
const TSLanguage *tree_sitter_json(void);
const TSLanguage *tree_sitter_toml(void);
const TSLanguage *tree_sitter_yaml(void);

typedef struct {
    const char *name;
    const char *aliases;
    const char *extensions;
    LanguageFn language;
    TSQuery *query;
    int broken;
} Language;

static Language languages[YHL_LANG_COUNT] = {
    {"c", "c", ".c .h", tree_sitter_c, NULL, 0},
    {"cpp", "cpp c++ cxx", ".cc .cpp .cxx .hh .hpp .hxx", tree_sitter_cpp, NULL,
     0},
    {"rust", "rust rs", ".rs", tree_sitter_rust, NULL, 0},
    {"go", "go golang", ".go", tree_sitter_go, NULL, 0},
    {"python", "python py", ".py .pyw", tree_sitter_python, NULL, 0},
    {"javascript", "javascript js jsx node", ".js .jsx .mjs .cjs",
     tree_sitter_javascript, NULL, 0},
    {"typescript", "typescript ts", ".ts .mts .cts", tree_sitter_typescript,
     NULL, 0},
    {"tsx", "tsx", ".tsx", tree_sitter_tsx, NULL, 0},
    {"bash", "bash sh shell", ".sh .bash .bashrc", tree_sitter_bash, NULL, 0},
    {"json", "json", ".json", tree_sitter_json, NULL, 0},
    {"toml", "toml", ".toml Cargo.lock", tree_sitter_toml, NULL, 0},
    {"yaml", "yaml yml", ".yaml .yml", tree_sitter_yaml, NULL, 0},
};

static unsigned char source[YHL_SOURCE_MAX];
static char hint[YHL_FILENAME_MAX + 1u];
static uint8_t byte_kind[YHL_SOURCE_MAX];
static uint32_t byte_width[YHL_SOURCE_MAX];
static uint32_t byte_pattern[YHL_SOURCE_MAX];

static uint32_t get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
           | (uint32_t)p[3] << 24;
}

static void put_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static int read_all(void *out, size_t n) {
    unsigned char *p = out;
    size_t off = 0;
    while (off < n) {
        ssize_t got = read(STDIN_FILENO, p + off, n - off);
        if (got > 0) {
            off += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        return 0;
    }
    return 1;
}

static int write_all(const void *in, size_t n) {
    const unsigned char *p = in;
    size_t off = 0;
    while (off < n) {
        ssize_t put = write(STDOUT_FILENO, p + off, n - off);
        if (put > 0) {
            off += (size_t)put;
            continue;
        }
        if (put < 0 && errno == EINTR) continue;
        return 0;
    }
    return 1;
}

static int discard(size_t n) {
    unsigned char buf[4096];
    while (n) {
        size_t take = n < sizeof buf ? n : sizeof buf;
        if (!read_all(buf, take)) return 0;
        n -= take;
    }
    return 1;
}

static int token_has(const char *list, const char *value) {
    size_t value_n = strlen(value);
    for (size_t i = 0; list[i];) {
        while (list[i] == ' ') i++;
        size_t start = i;
        while (list[i] && list[i] != ' ') i++;
        if (i - start == value_n && memcmp(list + start, value, value_n) == 0)
            return 1;
    }
    return 0;
}

static void lower_ascii(char *s) {
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}

static Language *language_for(uint8_t kind) {
    lower_ascii(hint);
    if (kind == YHL_HINT_ALIAS) {
        for (size_t i = 0; i < YHL_LANG_COUNT; i++)
            if (token_has(languages[i].aliases, hint)) return &languages[i];
        return NULL;
    }
    const char *base = strrchr(hint, '/');
    base = base ? base + 1 : hint;
    if (strcmp(base, "cargo.lock") == 0) return &languages[10];
    const char *dot = strrchr(base, '.');
    if (!dot) return NULL;
    for (size_t i = 0; i < YHL_LANG_COUNT; i++)
        if (token_has(languages[i].extensions, dot)) return &languages[i];
    return NULL;
}

static int starts(const char *s, uint32_t n, const char *prefix) {
    size_t p = strlen(prefix);
    return p <= n && memcmp(s, prefix, p) == 0;
}

static uint8_t semantic(const char *name, uint32_t n) {
    if (starts(name, n, "comment")) return YHL_SEM_COMMENT;
    if (starts(name, n, "string") || starts(name, n, "character"))
        return YHL_SEM_STRING;
    if (starts(name, n, "number") || starts(name, n, "constant")
        || starts(name, n, "boolean") || starts(name, n, "float"))
        return YHL_SEM_NUMBER;
    if (starts(name, n, "keyword") || starts(name, n, "operator")
        || starts(name, n, "conditional") || starts(name, n, "repeat")
        || starts(name, n, "exception") || starts(name, n, "include"))
        return YHL_SEM_KEYWORD;
    if (starts(name, n, "type.builtin") || starts(name, n, "function.builtin")
        || starts(name, n, "variable.builtin") || starts(name, n, "builtin")
        || starts(name, n, "attribute"))
        return YHL_SEM_BUILTIN;
    if (starts(name, n, "type") || starts(name, n, "tag")
        || starts(name, n, "constructor"))
        return YHL_SEM_TYPE;
    if (starts(name, n, "function") || starts(name, n, "method")
        || starts(name, n, "property"))
        return YHL_SEM_FUNCTION;
    return 0;
}

static int queries_named(void) {
    if (yhl_query_source_count != YHL_LANG_COUNT) return 0;
    for (size_t i = 0; i < YHL_LANG_COUNT; i++)
        if (strcmp(languages[i].name, yhl_query_sources[i].name) != 0) return 0;
    return 1;
}

static TSQuery *language_query(Language *lang) {
    if (lang->query || lang->broken) return lang->query;
    const YhlQuerySource *src = &yhl_query_sources[lang - languages];
    uint32_t error_offset = 0;
    TSQueryError error_type = TSQueryErrorNone;
    TSQuery *query =
        ts_query_new(lang->language(), (const char *)src->text,
                     (uint32_t)src->size, &error_offset, &error_type);
    uint32_t useful = 0;
    if (query) {
        uint32_t patterns = ts_query_pattern_count(query);
        for (uint32_t pattern = 0; pattern < patterns; pattern++) {
            uint32_t predicate_n = 0;
            (void)ts_query_predicates_for_pattern(query, pattern, &predicate_n);
            if (predicate_n) {
                ts_query_disable_pattern(query, pattern);
                continue;
            }
            useful++;
        }
    }
    if (!query || !useful) {
        if (query) ts_query_delete(query);
        lang->broken = 1;
        return NULL;
    }
    lang->query = query;
    return query;
}

static int queries_all_compile(void) {
    for (size_t i = 0; i < YHL_LANG_COUNT; i++)
        if (!language_query(&languages[i])) return 0;
    return 1;
}

static void queries_delete(void) {
    for (size_t i = 0; i < YHL_LANG_COUNT; i++)
        if (languages[i].query) ts_query_delete(languages[i].query);
}

static uint8_t make_runs(Language *lang, TSQuery *query, uint32_t source_n,
                         uint32_t *run_count) {
    *run_count = 0;
    if (!source_n) return YHL_STATUS_OK;
    memset(byte_kind, 0, source_n);
    for (uint32_t i = 0; i < source_n; i++) byte_width[i] = UINT32_MAX;
    memset(byte_pattern, 0, source_n * sizeof byte_pattern[0]);

    TSParser *parser = ts_parser_new();
    TSQueryCursor *cursor = ts_query_cursor_new();
    if (!parser || !cursor
        || !ts_parser_set_language(parser, lang->language())) {
        if (cursor) ts_query_cursor_delete(cursor);
        if (parser) ts_parser_delete(parser);
        return YHL_STATUS_INTERNAL;
    }
    TSTree *tree =
        ts_parser_parse_string(parser, NULL, (const char *)source, source_n);
    if (!tree) {
        ts_query_cursor_delete(cursor);
        ts_parser_delete(parser);
        return YHL_STATUS_INTERNAL;
    }
    ts_query_cursor_set_match_limit(cursor, YHL_RUN_MAX);
    ts_query_cursor_exec(cursor, query, ts_tree_root_node(tree));
    TSQueryMatch match;
    uint32_t capture_index = 0;
    uint32_t captures = 0;
    uint32_t work = 0;
    uint8_t status = YHL_STATUS_OK;
    while (ts_query_cursor_next_capture(cursor, &match, &capture_index)) {
        if (++captures > YHL_RUN_MAX) {
            status = YHL_STATUS_TOO_COMPLEX;
            break;
        }
        TSQueryCapture capture = match.captures[capture_index];
        uint32_t name_n = 0;
        const char *name =
            ts_query_capture_name_for_id(query, capture.index, &name_n);
        uint8_t kind = semantic(name, name_n);
        if (!kind) continue;
        uint32_t a = ts_node_start_byte(capture.node);
        uint32_t b = ts_node_end_byte(capture.node);
        if (a >= b || b > source_n) continue;
        uint32_t width = b - a;
        if (work > YHL_WORK_MAX - width) {
            status = YHL_STATUS_TOO_COMPLEX;
            break;
        }
        work += width;
        for (uint32_t i = a; i < b; i++) {
            if (width < byte_width[i]
                || (width == byte_width[i]
                    && match.pattern_index >= byte_pattern[i])) {
                byte_kind[i] = kind;
                byte_width[i] = width;
                byte_pattern[i] = match.pattern_index;
            }
        }
    }
    int exceeded = ts_query_cursor_did_exceed_match_limit(cursor);
    ts_tree_delete(tree);
    ts_query_cursor_delete(cursor);
    ts_parser_delete(parser);
    if (status != YHL_STATUS_OK) return status;
    if (exceeded) return YHL_STATUS_TOO_COMPLEX;

    uint32_t runs = 0;
    for (uint32_t i = 0; i < source_n;) {
        uint8_t kind = byte_kind[i];
        uint32_t end = i + 1;
        while (end < source_n && byte_kind[end] == kind) end++;
        if (kind && ++runs > YHL_RUN_MAX) return YHL_STATUS_TOO_COMPLEX;
        i = end;
    }
    *run_count = runs;
    return YHL_STATUS_OK;
}

static int send_response(uint32_t id, uint8_t status, uint32_t count,
                         uint32_t source_n) {
    unsigned char header[YHL_RESPONSE_HEADER] = {0};
    memcpy(header, YHL_MAGIC, 4);
    put_u32(header + 4, id);
    header[8] = status;
    put_u32(header + 12, count);
    if (!write_all(header, sizeof header)) return 0;
    if (status != YHL_STATUS_OK) return 1;
    if (!count) return 1;
    for (uint32_t i = 0; i < source_n;) {
        uint8_t kind = byte_kind[i];
        uint32_t end = i + 1;
        while (end < source_n && byte_kind[end] == kind) end++;
        if (kind) {
            unsigned char run[YHL_RUN_BYTES] = {0};
            put_u32(run, i);
            put_u32(run + 4, end);
            run[8] = kind;
            if (!write_all(run, sizeof run)) return 0;
            if (!--count) break;
        }
        i = end;
    }
    return count == 0;
}

static int serve(void) {
    for (;;) {
        unsigned char header[YHL_REQUEST_HEADER];
        ssize_t first = read(STDIN_FILENO, header, sizeof header);
        if (first == 0) return 1;
        if (first < 0 && errno == EINTR) continue;
        if (first <= 0
            || !read_all(header + (size_t)first, sizeof header - (size_t)first))
            return 0;
        if (memcmp(header, YHL_MAGIC, 4) != 0) return 0;
        uint32_t id = get_u32(header + 4);
        uint8_t hint_kind = header[8];
        uint32_t hint_n = get_u32(header + 12);
        uint32_t source_n = get_u32(header + 16);
        uint32_t hint_max = hint_kind == YHL_HINT_ALIAS      ? YHL_ALIAS_MAX
                            : hint_kind == YHL_HINT_FILENAME ? YHL_FILENAME_MAX
                                                             : 0;
        if (!hint_max || hint_n > hint_max || source_n > YHL_SOURCE_MAX) {
            if (!discard(hint_n) || !discard(source_n)
                || !send_response(id, YHL_STATUS_TOO_LARGE, 0, 0))
                return 0;
            continue;
        }
        if (!read_all(hint, hint_n) || !read_all(source, source_n)) return 0;
        hint[hint_n] = '\0';
        Language *lang = language_for(hint_kind);
        TSQuery *query = lang ? language_query(lang) : NULL;
        if (!query) {
            if (!send_response(id, YHL_STATUS_UNKNOWN, 0, 0)) return 0;
            continue;
        }
        uint32_t run_count = 0;
        uint8_t status = make_runs(lang, query, source_n, &run_count);
        if (!send_response(id, status, status == YHL_STATUS_OK ? run_count : 0,
                           source_n))
            return 0;
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("arqan-highlight 1 (tree-sitter 0.26.11)");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--list-languages") == 0) {
        for (size_t i = 0; i < YHL_LANG_COUNT; i++) puts(languages[i].name);
        return 0;
    }
    if (argc > 1 && !(argc == 2 && strcmp(argv[1], "--self-test") == 0)) {
        fprintf(stderr,
                "usage: arqan-highlight [--version|--list-languages]\n");
        return 2;
    }
    if (!queries_named() || (argc == 2 && !queries_all_compile())) {
        fputs("arqan-highlight: bundled grammar/query validation failed\n",
              stderr);
        queries_delete();
        return 1;
    }
    if (argc == 2) {
        queries_delete();
        return 0;
    }
    int ok = serve();
    queries_delete();
    return ok ? 0 : 1;
}
