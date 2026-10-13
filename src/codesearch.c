/* The code_search tool: structural search over the project through ast-grep,
 * offered only when ast-grep is on PATH.
 *
 * ast-grep runs with no shell, under the sandbox of a read-only command, with
 * any sgconfig.yml ignored and native grammars refused, so a project cannot
 * change what is parsed or load code into the search. outline, definition and
 * callers build their rules from a table per language; pattern and rule pass
 * the model's query through. The JSON is read one match at a time, and only a
 * short line per match reaches the model, sorted so a page is the same on
 * every call. callers also lists every other whole-word mention of the name,
 * so a call inside a macro, or in code tree-sitter cannot parse, still shows.
 */
#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CS_TIMEOUT_MS       30000
#define CS_MAX_RECORDS      20000u
#define CS_POOL_BYTES       (8u << 20)
#define CS_LINE_BYTES       (4u << 20)
#define CS_DIAG_BYTES       1536u
#define CS_NAME_MAX         128u
#define CS_PATTERN_MAX      4096u
#define CS_RULE_MAX         8192u
#define CS_TEXT_CLIP        160u
#define CS_MENTION_FILE_MAX (32u << 20)
#define CS_SKIP_BYTES       3000000u
#define CS_SKIP_LINES       200000u
#define CS_BIG_LISTED       4u
#define CS_DEFAULT_LINES    400u
#define CS_MAX_LINES        2000u
#define CS_ARGV_MAX         24
#define CS_MID_BYTES        4096u
#define CS_BIG_STRING       (64u << 10)

typedef struct {
    char path[AGENT_MAX_PATH];
    b8 probed;
    b8 found;
} CodeSearchProgram;

static CodeSearchProgram g_code_search;

/* ---- languages ---- */

typedef struct {
    const char *label;
    const char *rule;
    b8 scope;
} CsDef;

typedef struct {
    const char *name;
    const char *aliases;
    const char *root;
    const char *call;
    const CsDef *defs;
    size_t defs_n;
    b8 call_context;
} CsLang;

#define CS_C_TOP "not: {inside: {kind: compound_statement, stopBy: end}}"
#define CS_C_DECLARATOR                                                        \
    "has: {field: declarator, any: [{kind: function_declarator, has: {field: " \
    "declarator, pattern: $N}}, {has: {kind: function_declarator, stopBy: "    \
    "end, has: {field: declarator, pattern: $N}}}]}"
#define CS_C_FN "{kind: function_definition, " CS_C_TOP ", " CS_C_DECLARATOR "}"
#define CS_C_BODY(kind, body)                                          \
    "{kind: " kind ", all: [{has: {field: name, pattern: $N}}, {has: " \
    "{field: body, kind: " body "}}]}"
#define CS_C_TYPEDEF                                                        \
    "{kind: type_definition, has: {field: declarator, any: [{kind: "        \
    "type_identifier, pattern: $N}, {has: {kind: type_identifier, stopBy: " \
    "end, pattern: $N}}]}}"
#define CS_C_MACRO                                                             \
    "{any: [{kind: preproc_def}, {kind: preproc_function_def}], has: {field: " \
    "name, pattern: $N}}"
#define CS_NAMED(kind) "{kind: " kind ", has: {field: name, pattern: $N}}"
#define CS_NAMED_ANY(a, b) \
    "{any: [{kind: " a "}, {kind: " b "}], has: {field: name, pattern: $N}}"
#define CS_JS_FN_VALUE                                                      \
    "{kind: variable_declarator, all: [{has: {field: name, pattern: $N}}, " \
    "{has: {field: value, any: [{kind: arrow_function}, {kind: "            \
    "function_expression}]}}]}"
#define CS_CALL(kind, field) \
    "{kind: " kind ", has: {field: " field ", regex: @R}}"
#define CS_JS_CALL                                                 \
    "{any: [" CS_CALL("call_expression", "function") ", " CS_CALL( \
        "new_expression", "constructor") "]}"
#define CS_CSHARP_CALL                                                   \
    "{any: [" CS_CALL("invocation_expression", "function") ", " CS_CALL( \
        "object_creation_expression", "type") "]}"

static const CsDef k_cs_c[] = {
    {"fn", CS_C_FN, true},
    {"proto", "{kind: declaration, " CS_C_TOP ", " CS_C_DECLARATOR "}", false},
    {"var",
     "{kind: declaration, not: {any: [{inside: {kind: compound_statement, "
     "stopBy: end}}, {has: {kind: function_declarator, stopBy: end}}]}, has: "
     "{field: declarator, any: [{kind: identifier, pattern: $N}, {has: {kind: "
     "identifier, stopBy: end, pattern: $N}}]}}",
     false},
    {"struct", CS_C_BODY("struct_specifier", "field_declaration_list"), false},
    {"union", CS_C_BODY("union_specifier", "field_declaration_list"), false},
    {"enum", CS_C_BODY("enum_specifier", "enumerator_list"), false},
    {"typedef", CS_C_TYPEDEF, false},
    {"macro", CS_C_MACRO, false},
};

static const CsDef k_cs_cpp[] = {
    {"fn", CS_C_FN, true},
    {"class", CS_C_BODY("class_specifier", "field_declaration_list"), false},
    {"struct", CS_C_BODY("struct_specifier", "field_declaration_list"), false},
    {"union", CS_C_BODY("union_specifier", "field_declaration_list"), false},
    {"enum", CS_C_BODY("enum_specifier", "enumerator_list"), false},
    {"namespace", CS_NAMED("namespace_definition"), false},
    {"type", CS_NAMED("alias_declaration"), false},
    {"typedef", CS_C_TYPEDEF, false},
    {"macro", CS_C_MACRO, false},
};

static const CsDef k_cs_python[] = {
    {"fn", CS_NAMED("function_definition"), true},
    {"class", CS_NAMED("class_definition"), false},
};

static const CsDef k_cs_rust[] = {
    {"fn", CS_NAMED_ANY("function_item", "function_signature_item"), true},
    {"struct", CS_NAMED_ANY("struct_item", "union_item"), false},
    {"enum", CS_NAMED("enum_item"), false},
    {"trait", CS_NAMED("trait_item"), false},
    {"impl", "{kind: impl_item, has: {field: type, pattern: $N}}", false},
    {"mod", CS_NAMED("mod_item"), false},
    {"macro", CS_NAMED("macro_definition"), false},
    {"type", CS_NAMED("type_item"), false},
    {"const", CS_NAMED_ANY("const_item", "static_item"), false},
};

static const CsDef k_cs_go[] = {
    {"fn", CS_NAMED("function_declaration"), true},
    {"method", CS_NAMED("method_declaration"), true},
    {"type", CS_NAMED("type_spec"), false},
};

static const CsDef k_cs_js[] = {
    {"fn",
     CS_NAMED_ANY("function_declaration", "generator_function_declaration"),
     true},
    {"fn", CS_JS_FN_VALUE, true},
    {"class", CS_NAMED("class_declaration"), false},
    {"method", CS_NAMED("method_definition"), true},
};

static const CsDef k_cs_ts[] = {
    {"fn",
     "{any: [{kind: function_declaration}, {kind: "
     "generator_function_declaration}, {kind: function_signature}], has: "
     "{field: name, pattern: $N}}",
     true},
    {"fn", CS_JS_FN_VALUE, true},
    {"class", CS_NAMED_ANY("class_declaration", "abstract_class_declaration"),
     false},
    {"method", CS_NAMED("method_definition"), true},
    {"interface", CS_NAMED("interface_declaration"), false},
    {"type", CS_NAMED("type_alias_declaration"), false},
    {"enum", CS_NAMED("enum_declaration"), false},
};

static const CsDef k_cs_csharp[] = {
    {"method", CS_NAMED("method_declaration"), true},
    {"fn", CS_NAMED("local_function_statement"), true},
    {"ctor", CS_NAMED("constructor_declaration"), true},
    {"class", CS_NAMED("class_declaration"), false},
    {"interface", CS_NAMED("interface_declaration"), false},
    {"struct", CS_NAMED("struct_declaration"), false},
    {"record", CS_NAMED("record_declaration"), false},
    {"enum", CS_NAMED("enum_declaration"), false},
    {"delegate", CS_NAMED("delegate_declaration"), false},
    {"namespace",
     CS_NAMED_ANY("namespace_declaration", "file_scoped_namespace_declaration"),
     false},
    {"property", CS_NAMED("property_declaration"), false},
};

#define CS_LANG(nm, aliases, root, call, defs, context) \
    {nm, aliases, root, call, defs, sizeof(defs) / sizeof *(defs), context}

/* NOTE: the aliases are the ones ast-grep accepts for the same language. */
static const CsLang k_cs_langs[] = {
    CS_LANG("c", "", "translation_unit", CS_CALL("call_expression", "function"),
            k_cs_c, true),
    CS_LANG("cpp", "c++ cc cxx", "translation_unit",
            CS_CALL("call_expression", "function"), k_cs_cpp, true),
    CS_LANG("csharp", "cs", "compilation_unit", CS_CSHARP_CALL, k_cs_csharp,
            false),
    CS_LANG("python", "py", "module", CS_CALL("call", "function"), k_cs_python,
            false),
    CS_LANG("rust", "rs", "source_file", CS_CALL("call_expression", "function"),
            k_cs_rust, false),
    CS_LANG("go", "golang", "source_file",
            CS_CALL("call_expression", "function"), k_cs_go, false),
    CS_LANG("javascript", "js jsx", "program", CS_JS_CALL, k_cs_js, false),
    CS_LANG("typescript", "ts", "program", CS_JS_CALL, k_cs_ts, false),
    CS_LANG("tsx", "", "program", CS_JS_CALL, k_cs_ts, false),
};

#define CS_LANGS_N (sizeof k_cs_langs / sizeof *k_cs_langs)

static b8 cs_lang_named(Str name, const char *word, size_t n) {
    if (name.n != n) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name.p[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != word[i]) return false;
    }
    return true;
}

static const CsLang *cs_lang_find(Str name) {
    for (size_t i = 0; i < CS_LANGS_N; i++) {
        const CsLang *lang = &k_cs_langs[i];
        if (cs_lang_named(name, lang->name, strlen(lang->name))) return lang;
        for (const char *a = lang->aliases; *a;) {
            size_t n = strcspn(a, " ");
            if (cs_lang_named(name, a, n)) return lang;
            a += n + (a[n] == ' ');
        }
    }
    return NULL;
}

static b8 cs_lang_valid(Str name) {
    if (!name.n || name.n > 32) return false;
    for (size_t i = 0; i < name.n; i++) {
        char c = name.p[i];
        b8 ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '+'
                || c == '#';
        if (!ok) return false;
    }
    return true;
}

/* ---- finding ast-grep ---- */

static b8 cs_probe(void) {
#ifdef AGENT_TESTING
    const char *on = getenv(AGENT_ENV_PREFIX "TEST_CODE_SEARCH");
    if (!on || strcmp(on, "1") != 0) return false;
#endif
    const char *path = getenv("PATH");
    while (path && *path) {
        const char *end = strchr(path, ':');
        size_t n = end ? (size_t)(end - path) : strlen(path);
        char *z = g_code_search.path;
        if (n && path[0] == '/'
            && n + sizeof "/ast-grep" <= sizeof g_code_search.path) {
            memcpy(z, path, n);
            memcpy(z + n, "/ast-grep", sizeof "/ast-grep");
            struct stat st;
            if (stat(z, &st) == 0 && S_ISREG(st.st_mode)
                && access(z, X_OK) == 0)
                return true;
        }
        path = end ? end + 1 : NULL;
    }
    g_code_search.path[0] = '\0';
    return false;
}

b8 code_search_available(void) {
    if (!g_code_search.probed) {
        g_code_search.probed = true;
        g_code_search.found = cs_probe();
    }
    return g_code_search.found;
}

/* ---- rules ---- */

static void cs_put_template(Buf *b, const char *tpl, Str value) {
    for (const char *p = tpl; *p; p++) {
        if (p[0] == '@' && p[1] == 'R') {
            buf_puts(b, value);
            p++;
        } else {
            buf_putc(b, *p);
        }
    }
}

static void cs_doc(Buf *b, const char *id, Str lang) {
    if (b->n) buf_puts(b, STR("---\n"));
    buf_putf(b, "id: %s\nlanguage: %.*s\nrule: ", id, (i32)lang.n, lang.p);
}

static b8 cs_name_valid(Str name) {
    if (!name.n || name.n > CS_NAME_MAX) return false;
    if (name.p[0] >= '0' && name.p[0] <= '9') return false;
    for (size_t i = 0; i < name.n; i++) {
        char c = name.p[i];
        b8 word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                  || (c >= '0' && c <= '9') || c == '_' || c == '$';
        if (!word) return false;
    }
    return true;
}

static b8 cs_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
           || (c >= '0' && c <= '9') || c == '_' || c == '$';
}

/* A regex for `name` between non-word characters, as a JSON string, which
 * YAML reads as a double-quoted scalar. */
static Str cs_name_regex(Arena *a, Str name, const char *after) {
    Buf raw;
    buf_init(&raw, a, name.n * 2 + 64);
    buf_puts(&raw, STR("(^|[^A-Za-z0-9_$])"));
    for (size_t i = 0; i < name.n; i++) {
        if (name.p[i] == '$') buf_putc(&raw, '\\');
        buf_putc(&raw, name.p[i]);
    }
    buf_puts(&raw, str_c(after));
    Buf quoted;
    buf_init(&quoted, a, raw.n * 2 + 8);
    buf_json_str(&quoted, (Str){raw.p, raw.n});
    if (!buf_ok(&raw) || !buf_ok(&quoted)) return (Str){0};
    return buf_finish(&quoted);
}

static void cs_rules_outline(Buf *b, const CsLang *only) {
    for (size_t l = 0; l < CS_LANGS_N; l++) {
        const CsLang *lang = &k_cs_langs[l];
        if (only && only != lang) continue;
        for (size_t d = 0; d < lang->defs_n; d++) {
            cs_doc(b, lang->defs[d].label, str_c(lang->name));
            buf_puts(b, str_c(lang->defs[d].rule));
            buf_putc(b, '\n');
        }
    }
}

static void cs_rules_definition(Buf *b, const CsLang *only, Str def_re) {
    for (size_t l = 0; l < CS_LANGS_N; l++) {
        const CsLang *lang = &k_cs_langs[l];
        if (only && only != lang) continue;
        for (size_t d = 0; d < lang->defs_n; d++) {
            cs_doc(b, lang->defs[d].label, str_c(lang->name));
            buf_puts(b, str_c(lang->defs[d].rule));
            buf_puts(b, STR("\nconstraints: {N: {regex: "));
            buf_puts(b, def_re);
            buf_puts(b, STR("}}\n"));
        }
    }
}

static void cs_rules_callers(Buf *b, const CsLang *only, Str call_re) {
    for (size_t l = 0; l < CS_LANGS_N; l++) {
        const CsLang *lang = &k_cs_langs[l];
        if (only && only != lang) continue;
        cs_doc(b, "call", str_c(lang->name));
        cs_put_template(b, lang->call, call_re);
        buf_putc(b, '\n');
        for (size_t d = 0; d < lang->defs_n; d++) {
            if (!lang->defs[d].scope) continue;
            cs_doc(b, "in", str_c(lang->name));
            buf_puts(b, STR("{all: ["));
            buf_puts(b, str_c(lang->defs[d].rule));
            buf_puts(b, STR(", {has: {stopBy: end, all: ["));
            cs_put_template(b, lang->call, call_re);
            buf_puts(b, STR("]}}]}\n"));
        }
    }
}

static void cs_rules_mentions(Buf *b, const CsLang *only, Str text_re) {
    for (size_t l = 0; l < CS_LANGS_N; l++) {
        const CsLang *lang = &k_cs_langs[l];
        if (only && only != lang) continue;
        cs_doc(b, "text", str_c(lang->name));
        buf_putf(b, "{kind: %s, regex: ", lang->root);
        buf_puts(b, text_re);
        buf_puts(b, STR("}\n"));
    }
}

/* A C or C++ call with one argument, or with only `$$$`, parses on its own
 * as a macro type, and then matches nothing. In an initializer it parses as
 * the call it is meant to be. */
static b8 cs_call_shaped(Str pattern) {
    pattern = str_trim(pattern);
    size_t i = 0;
    while (i < pattern.n && cs_word_char(pattern.p[i])) i++;
    if (!i || i >= pattern.n || pattern.p[i] != '(') return false;
    if (pattern.p[pattern.n - 1] != ')') return false;
    i32 depth = 0;
    for (size_t k = i; k < pattern.n; k++) {
        if (pattern.p[k] == '(') depth++;
        if (pattern.p[k] == ')' && --depth == 0 && k + 1 != pattern.n)
            return false;
    }
    return depth == 0;
}

/* Returns the pattern as searched, which may be the call in its context. */
static Str cs_rules_pattern(Buf *b, Str lang, b8 call_context, Str pattern,
                            Arena *a) {
    cs_doc(b, "match", lang);
    if (call_context && cs_call_shaped(pattern)) {
        Buf context;
        buf_init(&context, a, pattern.n + 32);
        buf_puts(&context, STR("int agent_match = "));
        buf_puts(&context, pattern);
        buf_putc(&context, ';');
        buf_puts(b, STR("{pattern: {context: "));
        buf_json_str(b, (Str){context.p, context.n});
        buf_puts(b, STR(", selector: call_expression}}\n"));
        if (!buf_ok(&context)) {
            b->oom = true;
            return pattern;
        }
        return buf_finish(&context);
    }
    buf_puts(b, STR("{pattern: "));
    buf_json_str(b, pattern);
    buf_puts(b, STR("}\n"));
    return pattern;
}

/* ---- results ---- */

typedef enum { CS_DEF, CS_MATCH, CS_SCOPE, CS_CALL_SITE, CS_MENTION } CsKind;

typedef struct {
    Str file;
    Str label;
    Str name;
    Str text;
    Str scope;
    u32 start_line, end_line;
    u32 start_byte, end_byte;
    u8 kind;
} CsRecord;

typedef struct {
    CsRecord *rec;
    size_t n;
    size_t dropped;
    char *pool;
    size_t pool_n;
    b8 pool_full;
    char *line;
    size_t line_n;
    size_t line_cap;
    b8 line_over;
    size_t too_large;
    b8 in_string, escaped;
    size_t string_n;
    u32 big_closed;
    char mid[2][CS_MID_BYTES];
    size_t mid_n[2];
    char str_head[16];
    b8 rule_id_next;
    char rule_id[32];
    size_t rule_id_n;
    char diag[CS_DIAG_BYTES];
    size_t diag_n;
    b8 full_text;
    b8 no_match_exit;
    Buf *paths;
    Buf *entities;
    size_t skipped;
    Str name;
    Arena *scratch;
} CsRun;

static Str cs_keep(CsRun *r, Str s) {
    if (s.n > CS_POOL_BYTES - r->pool_n) {
        r->pool_full = true;
        return (Str){0};
    }
    char *at = r->pool + r->pool_n;
    if (s.n) memcpy(at, s.p, s.n);
    r->pool_n += s.n;
    return (Str){at, s.n};
}

static Str cs_first_line(Str s) {
    size_t n = 0;
    while (n < s.n && s.p[n] != '\n') n++;
    return str_clip_utf8(str_trim((Str){s.p, n}), CS_TEXT_CLIP);
}

static Str cs_take_lines(Str s, u32 count) {
    size_t n = 0;
    while (n < s.n && count) {
        if (s.p[n] == '\n') count--;
        n++;
    }
    return (Str){s.p, n};
}

static u32 cs_num(const JVal *obj, Str key) {
    const JVal *v = json_get(obj, key);
    if (!v || v->type != J_NUM || v->u.n < 0 || v->u.n > 4294967295.0) return 0;
    return (u32)v->u.n;
}

static CsRecord *cs_add(CsRun *r) {
    if (r->n >= CS_MAX_RECORDS) {
        r->dropped++;
        return NULL;
    }
    CsRecord *rec = &r->rec[r->n++];
    *rec = (CsRecord){0};
    return rec;
}

static void cs_mentions(CsRun *r, Str file, Str text) {
    Str name = r->name;
    u32 line = 1;
    size_t line_start = 0;
    u32 last_line = 0;
    for (size_t i = 0; i + name.n <= text.n; i++) {
        if (text.p[i] == '\n') {
            line++;
            line_start = i + 1;
            continue;
        }
        if (memcmp(text.p + i, name.p, name.n) != 0) continue;
        if (i > 0 && cs_word_char(text.p[i - 1])) continue;
        if (i + name.n < text.n && cs_word_char(text.p[i + name.n])) continue;
        if (line == last_line) continue;
        last_line = line;
        CsRecord *rec = cs_add(r);
        if (!rec) return;
        rec->kind = CS_MENTION;
        rec->file = file;
        rec->start_line = rec->end_line = line;
        rec->start_byte = i > UINT32_MAX ? UINT32_MAX : (u32)i;
        rec->text = cs_keep(
            r, cs_first_line((Str){text.p + line_start, text.n - line_start}));
    }
}

static void cs_take_match(CsRun *r, const JVal *j) {
    Str id = json_str(j, STR("ruleId"));
    const JVal *range = json_get(j, STR("range"));
    const JVal *bytes = json_get(range, STR("byteOffset"));
    const JVal *start = json_get(range, STR("start"));
    const JVal *end = json_get(range, STR("end"));
    Str file = cs_keep(r, json_str(j, STR("file")));
    if (!file.n) return;
    u32 start_line = cs_num(start, STR("line")) + 1;
    u32 end_line = cs_num(end, STR("line"));
    if (cs_num(end, STR("column")) > 0) end_line++;
    if (end_line < start_line) end_line = start_line;
    u32 start_byte = cs_num(bytes, STR("start"));
    CsRecord *rec = cs_add(r);
    if (!rec) return;
    rec->file = file;
    rec->start_line = start_line;
    rec->end_line = end_line;
    rec->start_byte = start_byte;
    rec->end_byte = cs_num(bytes, STR("end"));
    const JVal *meta =
        json_get(json_get(j, STR("metaVariables")), STR("single"));
    rec->name = cs_keep(r, json_str(json_get(meta, STR("N")), STR("text")));
    Str lines = json_str(j, STR("lines"));
    rec->kind = str_eq(id, STR("call"))    ? CS_CALL_SITE
                : str_eq(id, STR("in"))    ? CS_SCOPE
                : str_eq(id, STR("match")) ? CS_MATCH
                                           : CS_DEF;
    if (rec->kind == CS_DEF) rec->label = cs_keep(r, id);
    if (rec->kind == CS_SCOPE) return;
    if (!json_get(j, STR("lines"))) {
        rec->text = STR("[too long to show; read these lines]");
        return;
    }
    if (!r->full_text) {
        rec->text = cs_keep(r, cs_first_line(lines));
        return;
    }
    rec->text = cs_keep(r, cs_take_lines(lines, end_line - start_line + 1));
}

/* `--inspect` lines: a summary with the count of files ast-grep skipped,
 * and with `entity`, one line per file it considered. */
static b8 cs_take_inspect(CsRun *r, Str line) {
    if (!str_starts(line, STR("sg: "))) return false;
    Str skipped = STR("skippedFileCount=");
    Str entity = STR("sg: entity|file|");
    if (str_starts(line, STR("sg: summary|file: "))) {
        for (size_t i = 0; i + skipped.n <= line.n; i++) {
            if (memcmp(line.p + i, skipped.p, skipped.n) != 0) continue;
            b8 ok;
            i64 n = str_int(str_trim(str_drop(line, i + skipped.n)), &ok);
            if (ok && n > 0) r->skipped = (size_t)n;
        }
    } else if (r->entities && str_starts(line, entity)) {
        Str rest = str_drop(line, entity.n);
        Str mark = STR(": language=");
        size_t at = rest.n;
        while (at >= mark.n
               && memcmp(rest.p + at - mark.n, mark.p, mark.n) != 0)
            at--;
        if (at < mark.n) return true;
        Str lang = str_drop(rest, at);
        size_t n = 0;
        while (n < lang.n && lang.p[n] != ',') n++;
        buf_put(r->entities, lang.p, n);
        buf_putc(r->entities, '\t');
        buf_put(r->entities, rest.p, at - mark.n);
        buf_putc(r->entities, '\n');
    }
    return true;
}

static void cs_take_line(CsRun *r, Str line) {
    if (cs_take_inspect(r, line)) return;
    if (r->paths && line.n && line.p[0] != '{') {
        buf_puts(r->paths, line);
        buf_putc(r->paths, '\n');
        return;
    }
    if (line.n && line.p[0] == '{') {
        size_t mark = r->scratch->off;
        JVal *j = json_parse(r->scratch, line);
        if (j && j->type == J_OBJ) cs_take_match(r, j);
        r->scratch->off = mark;
        return;
    }
    while (line.n && (line.p[line.n - 1] == ' ' || line.p[line.n - 1] == '\r'))
        line.n--;
    if (!str_trim(line).n || r->diag_n + line.n + 1 > sizeof r->diag) return;
    memcpy(r->diag + r->diag_n, line.p, line.n);
    r->diag_n += line.n;
    r->diag[r->diag_n++] = '\n';
}

static Str cs_find(Str s, Str needle) {
    for (size_t i = 0; i + needle.n <= s.n; i++)
        if (memcmp(s.p + i, needle.p, needle.n) == 0) return str_drop(s, i);
    return (Str){0};
}

/* A match whose JSON line is too long to hold. ast-grep writes the keys in
 * a fixed order: text, range, file, lines, charCount, language,
 * metaVariables, ruleId, labels; text, lines, the multi metavariables and
 * labels each repeat the source. Range and file follow the first long
 * string, the name N follows the second, and ruleId is caught as it
 * passes. */
static void cs_take_oversized(CsRun *r) {
    Str head = cs_find((Str){r->mid[0], r->mid_n[0]}, STR("\"range\":"));
    Str head_end = cs_find(head, STR(",\"lines\":"));
    Str name = cs_find((Str){r->mid[1], r->mid_n[1]},
                       STR("\"single\":{\"N\":{\"text\":\""));
    if (name.n) {
        name = str_drop(name, sizeof "\"single\":{\"N\":{\"text\":" - 1);
        size_t end = 1;
        while (end < name.n && name.p[end] != '"')
            end += name.p[end] == '\\' ? 2 : 1;
        name = end < name.n ? str_take(name, end + 1) : (Str){0};
    }
    if (!head_end.n || !r->rule_id_n) {
        r->too_large++;
        return;
    }
    head.n -= head_end.n;
    size_t mark = r->scratch->off;
    Buf b;
    buf_init(&b, r->scratch, head.n + name.n + 128);
    buf_putc(&b, '{');
    buf_puts(&b, head);
    buf_puts(&b, STR(",\"ruleId\":"));
    buf_json_str(&b, (Str){r->rule_id, r->rule_id_n});
    if (name.n) {
        buf_puts(&b, STR(",\"metaVariables\":{\"single\":{\"N\":{\"text\":"));
        buf_puts(&b, name);
        buf_puts(&b, STR("}}}"));
    }
    buf_putc(&b, '}');
    JVal *j = buf_ok(&b) ? json_parse(r->scratch, (Str){b.p, b.n}) : NULL;
    if (j && j->type == J_OBJ)
        cs_take_match(r, j);
    else
        r->too_large++;
    r->scratch->off = mark;
}

/* The first string named ruleId is the top-level key: every string that
 * comes earlier is a key of the range or the file, or holds the source. */
static void cs_track_rule_id(CsRun *r) {
    if (r->rule_id_n) return;
    size_t n =
        r->string_n < sizeof r->str_head ? r->string_n : sizeof r->str_head;
    if (r->rule_id_next) {
        r->rule_id_next = false;
        if (r->string_n > n || !n) return;
        memcpy(r->rule_id, r->str_head, n);
        r->rule_id_n = n;
        return;
    }
    r->rule_id_next = r->string_n == 6 && memcmp(r->str_head, "ruleId", 6) == 0;
}

static void cs_track_json(CsRun *r, char c) {
    if (r->big_closed == 1 || r->big_closed == 2) {
        u32 k = r->big_closed - 1;
        if (r->mid_n[k] < CS_MID_BYTES) r->mid[k][r->mid_n[k]++] = c;
    }
    b8 closing = r->in_string && !r->escaped && c == '"';
    if (r->in_string && !closing && r->string_n < sizeof r->str_head)
        r->str_head[r->string_n] = c;
    if (r->escaped) {
        r->escaped = false;
        r->string_n++;
    } else if (r->in_string) {
        if (c == '\\') r->escaped = true;
        if (closing) {
            r->in_string = false;
            if (r->string_n > CS_BIG_STRING) r->big_closed++;
            cs_track_rule_id(r);
            return;
        }
        r->string_n++;
    } else if (c == '"') {
        r->in_string = true;
        r->string_n = 0;
    }
}

static void cs_put(void *ud, const char *p, size_t n) {
    CsRun *r = ud;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '\n') {
            if (r->line_over)
                cs_take_oversized(r);
            else
                cs_take_line(r, (Str){r->line, r->line_n});
            r->line_n = 0;
            r->line_over = false;
            r->in_string = r->escaped = false;
            r->string_n = 0;
            r->big_closed = 0;
            r->mid_n[0] = r->mid_n[1] = 0;
            r->rule_id_next = false;
            r->rule_id_n = 0;
            continue;
        }
        if (r->line_over) {
            cs_track_json(r, c);
            continue;
        }
        if (r->line_n == r->line_cap) {
            r->line_over = true;
            for (size_t k = 0; k < r->line_n; k++) cs_track_json(r, r->line[k]);
            cs_track_json(r, c);
            continue;
        }
        r->line[r->line_n++] = c;
    }
}

static i32 cs_str_cmp(Str a, Str b) {
    size_t n = a.n < b.n ? a.n : b.n;
    i32 c = n ? memcmp(a.p, b.p, n) : 0;
    if (c) return c;
    return a.n < b.n ? -1 : a.n > b.n ? 1 : 0;
}

static int cs_record_cmp(const void *pa, const void *pb) {
    const CsRecord *a = pa, *b = pb;
    i32 c = cs_str_cmp(a->file, b->file);
    if (c) return c;
    if (a->start_line != b->start_line)
        return a->start_line < b->start_line ? -1 : 1;
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    if (a->start_byte != b->start_byte)
        return a->start_byte < b->start_byte ? -1 : 1;
    return cs_str_cmp(a->label, b->label);
}

static int cs_path_cmp(const void *pa, const void *pb) {
    return cs_str_cmp(*(const Str *)pa, *(const Str *)pb);
}

/* ---- running ---- */

/* ast-grep's messages without its help lines, links and box glyphs. */
static void cs_diag_clean(Str diag, Buf *out) {
    static const char *const k_drop[] = {
        "Help:",
        "See also:",
        "Warning:",
        "Debug AST:",
        "Error: Cannot parse rule INLINE_RULES",
        "\xe2\x9c\x96 Caused by"};
    size_t off = 0;
    Str line;
    while (str_line(diag, &off, &line)) {
        while (line.n
               && (line.p[line.n - 1] == ' ' || line.p[line.n - 1] == '\r'))
            line.n--;
        if (!str_trim(line).n) continue;
        b8 drop = false;
        for (size_t i = 0; i < sizeof k_drop / sizeof *k_drop && !drop; i++)
            drop = str_starts(line, str_c(k_drop[i]));
        if (drop) continue;
        if (str_starts(line, STR("\xe2\x95\xb0\xe2\x96\xbb ")))
            line = str_drop(line, 7);
        if (out->n) buf_putc(out, '\n');
        buf_puts(out, line);
    }
}

static b8 cs_exec(const char *const *argv, Str reads, CsRun *r, Arena *scratch,
                  char *err, size_t err_cap) {
    i32 code = -1;
    size_t before = r->n;
    ProgramEnd end = shell_run_program(argv, cs_put, r, reads, CS_TIMEOUT_MS,
                                       &code, scratch, err, err_cap);
    if (r->line_n && !r->line_over) cs_take_line(r, (Str){r->line, r->line_n});
    r->line_n = 0;
    switch (end) {
        case PROGRAM_DONE: break;
        case PROGRAM_FAILED: return false;
        case PROGRAM_INTERRUPTED:
            snprintf(err, err_cap, "code_search was interrupted");
            return false;
        case PROGRAM_STOPPED:
            snprintf(err, err_cap,
                     "ast-grep did not finish in %ds; search a smaller path",
                     CS_TIMEOUT_MS / 1000);
            return false;
        case PROGRAM_YIELDED:
            snprintf(err, err_cap, "code_search stopped for a user message");
            return false;
    }
    if (code == 127) {
        snprintf(err, err_cap, "could not run %s", argv[0]);
        return false;
    }
    if (code != 0 && r->n == before && !(code == 1 && r->no_match_exit)) {
        Str raw = r->paths ? (Str){r->paths->p, r->paths->n}
                           : (Str){r->diag, r->diag_n};
        Buf clean;
        buf_init(&clean, scratch, raw.n + 1);
        cs_diag_clean(raw, &clean);
        Str diag = buf_ok(&clean) ? (Str){clean.p, clean.n} : (Str){0};
        if (diag.n)
            snprintf(err, err_cap, "ast-grep failed with exit code %d:\n%.*s",
                     code, (i32)diag.n, diag.p);
        else
            snprintf(err, err_cap, "ast-grep failed with exit code %d", code);
        return false;
    }
    return true;
}

static b8 cs_has_error_node(Str ast) {
    size_t off = 0;
    Str line;
    while (str_line(ast, &off, &line))
        if (str_starts(str_trim(line), STR("ERROR "))) return true;
    return false;
}

typedef struct {
    Str big[CS_BIG_LISTED];
    size_t big_n;
    size_t big_more;
} CsMentionScan;

/* ast-grep names the files that mention the word, and the tool reads each
 * one: ast-grep would send a whole file through one JSON line, twice. */
static b8 cs_scan_mentions(CsRun *r, const CsLang *lang, const char *path,
                           CsMentionScan *scan, char *err, size_t err_cap) {
    Arena *scratch = r->scratch;
    Buf rules, paths;
    buf_init(&rules, scratch, 4096);
    buf_init(&paths, scratch, 64u << 10);
    cs_rules_mentions(&rules, lang,
                      cs_name_regex(scratch, r->name, "([^A-Za-z0-9_$]|$)"));
    buf_putc(&rules, '\0');
    if (!buf_ok(&rules)) {
        snprintf(err, err_cap, "out of memory for code_search");
        return false;
    }
    const char *argv[CS_ARGV_MAX] = {g_code_search.path,
                                     "scan",
                                     "-c",
                                     "/dev/null",
                                     "--custom-languages",
                                     "ignore",
                                     "--color",
                                     "never",
                                     "--files-with-matches",
                                     "--inline-rules",
                                     rules.p,
                                     "--",
                                     path,
                                     NULL};
    size_t found = r->n;
    r->paths = &paths;
    b8 ok = cs_exec(argv, str_c(path), r, scratch, err, err_cap);
    r->paths = NULL;
    if (!ok) return false;
    if (r->n != found || !buf_ok(&paths)) {
        snprintf(err, err_cap,
                 "could not list the files that mention %.*s; search a "
                 "smaller path",
                 (i32)r->name.n, r->name.p);
        return false;
    }
    size_t off = 0;
    Str line;
    Str list = {paths.p, paths.n};
    while (str_line(list, &off, &line)) {
        if (!line.n || line.n >= AGENT_MAX_PATH) continue;
        Str file = cs_keep(r, line);
        char z[AGENT_MAX_PATH];
        memcpy(z, line.p, line.n);
        z[line.n] = '\0';
        size_t mark = scratch->off;
        Str text;
        FileStatus st =
            file_read(scratch, z, CS_MENTION_FILE_MAX, 0, &text, NULL);
        if (st == FILE_OK) cs_mentions(r, file, text);
        scratch->off = mark;
        if (st != FILE_TOO_LARGE) continue;
        if (scan->big_n < CS_BIG_LISTED)
            scan->big[scan->big_n++] = file;
        else
            scan->big_more++;
    }
    return true;
}

typedef struct {
    Str big[CS_BIG_LISTED];
    b8 big_read[CS_BIG_LISTED];
    size_t big_n;
    size_t big_more;
    size_t not_utf8;
} CsSkipped;

static b8 cs_lang_searched(Str entity_lang, b8 any) {
    if (any) return true;
    for (size_t i = 0; i < CS_LANGS_N; i++)
        if (str_eq_ci(entity_lang, str_c(k_cs_langs[i].name))) return true;
    return false;
}

static Str cs_entity_lang(Str line, Str *file) {
    size_t tab = 0;
    while (tab < line.n && line.p[tab] != '\t') tab++;
    *file = tab == line.n ? (Str){0} : str_drop(line, tab + 1);
    return str_take(line, tab);
}

/* NOTE: only `run` lists a file before reading it; `scan` lists only files
 * it read. Each line of `entities` is the language, a tab and the path. */
static b8 cs_list_files(CsRun *r, Str lang, const char *path, Buf *entities,
                        char *err, size_t err_cap) {
    Arena *scratch = r->scratch;
    Buf files;
    buf_init(&files, scratch, 4096);
    char *lang_z = arena_alloc(scratch, lang.n + 1, 1);
    if (!lang_z) {
        snprintf(err, err_cap, "out of memory for code_search");
        return false;
    }
    if (lang.n) memcpy(lang_z, lang.p, lang.n);
    lang_z[lang.n] = '\0';
    const char *argv[CS_ARGV_MAX] = {g_code_search.path,
                                     "run",
                                     "-c",
                                     "/dev/null",
                                     "--custom-languages",
                                     "ignore",
                                     "--color",
                                     "never",
                                     "--inspect",
                                     "entity",
                                     "--files-with-matches",
                                     "-p",
                                     "agent_skipped_files"};
    size_t argc = 13;
    if (lang.n) {
        argv[argc++] = "-l";
        argv[argc++] = lang_z;
    }
    argv[argc++] = "--";
    argv[argc++] = path;
    argv[argc] = NULL;
    size_t skipped = r->skipped, found = r->n;
    r->entities = entities;
    r->no_match_exit = true;
    r->paths = &files;
    b8 ok = cs_exec(argv, str_c(path), r, scratch, err, err_cap);
    r->entities = NULL;
    r->no_match_exit = false;
    r->paths = NULL;
    r->skipped = skipped;
    if (!ok) return false;
    if (r->n != found || !buf_ok(entities)) {
        snprintf(err, err_cap,
                 "could not list the files at %s; search a smaller path", path);
        return false;
    }
    return true;
}

static b8 cs_any_file_searched(Str entities, b8 any) {
    size_t off = 0;
    Str line, file;
    while (str_line(entities, &off, &line))
        if (cs_lang_searched(cs_entity_lang(line, &file), any) && file.n)
            return true;
    return false;
}

/* NOTE: ast-grep 0.50 skips, without a word, a file over 3,000,000 bytes
 * that also has over 200,000 lines, an empty file, and a file that is not
 * UTF-8; `--inspect summary` gives only the count. When it is not zero, the
 * files listed are checked and the large ones are named. In callers, the
 * tool reads the large files for mentions itself. */
static b8 cs_find_skipped(CsRun *r, Str lang, Str list, b8 mentions,
                          CsSkipped *sk, char *err, size_t err_cap) {
    Arena *scratch = r->scratch;
    size_t empty = 0, big = 0, off = 0, count = 0;
    Str line, file;
    size_t total = str_lines(list);
    Str *sorted = arena_new(scratch, Str, total + 1);
    if (!sorted) {
        snprintf(err, err_cap, "out of memory for code_search");
        return false;
    }
    while (str_line(list, &off, &line) && count < total) {
        Str entity_lang = cs_entity_lang(line, &file);
        if (!file.n || !cs_lang_searched(entity_lang, lang.n > 0)) continue;
        sorted[count++] = file;
    }
    qsort(sorted, count, sizeof *sorted, cs_path_cmp);
    for (size_t e = 0; e < count; e++) {
        line = sorted[e];
        if (!line.n || line.n >= AGENT_MAX_PATH) continue;
        char z[AGENT_MAX_PATH];
        memcpy(z, line.p, line.n);
        z[line.n] = '\0';
        struct stat st;
        if (stat(z, &st) != 0) continue;
        if (st.st_size == 0) empty++;
        if (st.st_size <= (off_t)CS_SKIP_BYTES) continue;
        size_t mark = scratch->off;
        Str text;
        FileStatus fs =
            file_read(scratch, z, CS_MENTION_FILE_MAX, 0, &text, NULL);
        b8 read = fs == FILE_OK;
        if ((read && str_lines(text) <= CS_SKIP_LINES)
            || (!read && fs != FILE_TOO_LARGE)) {
            scratch->off = mark;
            continue;
        }
        big++;
        Str file = cs_keep(r, line);
        if (read && mentions) cs_mentions(r, file, text);
        scratch->off = mark;
        if (sk->big_n < CS_BIG_LISTED) {
            sk->big_read[sk->big_n] = read && mentions;
            sk->big[sk->big_n++] = file;
        } else {
            sk->big_more++;
        }
    }
    sk->not_utf8 = r->skipped > empty + big ? r->skipped - empty - big : 0;
    return true;
}

/* Shows how the pattern parsed, but only when the parse holds an ERROR node:
 * a clean parse that matches nothing means the code is not there. */
static void cs_debug_pattern(Str lang, Str pattern, Buf *out, Arena *scratch) {
    char *z = arena_alloc(scratch, pattern.n + 1, 1);
    char *lang_z = arena_alloc(scratch, lang.n + 1, 1);
    CsRun *r = arena_new(scratch, CsRun, 1);
    char *line = arena_alloc(scratch, CS_DIAG_BYTES, 1);
    if (!z || !lang_z || !r || !line) return;
    memcpy(z, pattern.p, pattern.n);
    z[pattern.n] = '\0';
    memcpy(lang_z, lang.p, lang.n);
    lang_z[lang.n] = '\0';
    *r = (CsRun){.line = line, .line_cap = CS_DIAG_BYTES, .scratch = scratch};
    const char *argv[] = {g_code_search.path,
                          "run",
                          "-c",
                          "/dev/null",
                          "--custom-languages",
                          "ignore",
                          "--color",
                          "never",
                          "-l",
                          lang_z,
                          "-p",
                          z,
                          "--debug-query=ast",
                          "/dev/null",
                          NULL};
    char err[AGENT_TOOL_ERR];
    i32 code;
    if (shell_run_program(argv, cs_put, r, STR("."), CS_TIMEOUT_MS, &code,
                          scratch, err, sizeof err)
        != PROGRAM_DONE)
        return;
    if (r->line_n) cs_take_line(r, (Str){r->line, r->line_n});
    Str diag = {r->diag, r->diag_n};
    if (!cs_has_error_node(diag)) return;
    Buf clean;
    buf_init(&clean, scratch, diag.n + 1);
    cs_diag_clean(diag, &clean);
    if (!buf_ok(&clean) || !clean.n) return;
    buf_putf(out, "The pattern does not parse cleanly as %.*s:\n", (i32)lang.n,
             lang.p);
    buf_put(out, clean.p, clean.n);
    buf_putc(out, '\n');
}

/* ast-grep answers a pattern that does not parse with a message about rules;
 * the parse is what the model can act on. */
static void cs_pattern_failed(Str lang, Str pattern, Arena *scratch, char *err,
                              size_t err_cap) {
    Buf hint;
    buf_init(&hint, scratch, 4096);
    cs_debug_pattern(lang, pattern, &hint, scratch);
    if (!buf_ok(&hint) || !hint.n) return;
    Str s = str_clip_utf8((Str){hint.p, hint.n}, err_cap - 1);
    while (s.n && s.p[s.n - 1] == '\n') s.n--;
    snprintf(err, err_cap, "%.*s", (i32)s.n, s.p);
}

/* ---- output ---- */

static void cs_put_range(Buf *b, const CsRecord *rec) {
    if (rec->end_line > rec->start_line)
        buf_putf(b, "%u-%u", rec->start_line, rec->end_line);
    else
        buf_putf(b, "%u", rec->start_line);
}

static size_t cs_files(const CsRun *r) {
    size_t files = 0;
    for (size_t i = 0; i < r->n; i++)
        if (!i || !str_eq(r->rec[i].file, r->rec[i - 1].file)) files++;
    return files;
}

/* Marks a file line in a grouped body, so a page can repeat it. */
#define CS_HEAD '\x1e'

static void cs_put_path(Buf *b, Str path) {
    for (size_t i = 0; i < path.n; i++) {
        u8 c = (u8)path.p[i];
        buf_putc(b, c < 0x20 || c == 0x7f ? '?' : (char)c);
    }
}

static void cs_file_head(Buf *b, const CsRun *r, size_t i) {
    if (i && str_eq(r->rec[i].file, r->rec[i - 1].file)) return;
    buf_putc(b, CS_HEAD);
    cs_put_path(b, r->rec[i].file);
    buf_putc(b, '\n');
}

static b8 cs_same_row(const CsRun *r, size_t i) {
    const CsRecord *a = &r->rec[i], *b = i ? &r->rec[i - 1] : NULL;
    return b && a->kind == b->kind && a->start_line == b->start_line
           && str_eq(a->file, b->file);
}

static size_t cs_row_span(const CsRun *r, size_t i) {
    size_t n = 1;
    while (i + n < r->n && cs_same_row(r, i + n)) n++;
    return n;
}

static void cs_body_outline(Buf *b, const CsRun *r) {
    for (size_t i = 0; i < r->n; i++) {
        const CsRecord *rec = &r->rec[i];
        cs_file_head(b, r, i);
        buf_puts(b, STR("  "));
        cs_put_range(b, rec);
        buf_putc(b, ' ');
        buf_puts(b, rec->label);
        buf_putc(b, ' ');
        buf_puts(b, cs_first_line(rec->name));
        buf_putc(b, '\n');
    }
}

static void cs_body_definition(Buf *b, const CsRun *r) {
    for (size_t i = 0; i < r->n; i++) {
        const CsRecord *rec = &r->rec[i];
        if (i) buf_putc(b, '\n');
        cs_put_path(b, rec->file);
        buf_putc(b, ':');
        cs_put_range(b, rec);
        buf_putc(b, ' ');
        buf_puts(b, rec->label);
        buf_putc(b, ' ');
        buf_puts(b, cs_first_line(rec->name));
        buf_putc(b, '\n');
        buf_puts(b, rec->text);
        if (rec->text.n && rec->text.p[rec->text.n - 1] != '\n')
            buf_putc(b, '\n');
    }
}

static void cs_body_matches(Buf *b, const CsRun *r) {
    for (size_t i = 0; i < r->n; i++) {
        const CsRecord *rec = &r->rec[i];
        if (cs_same_row(r, i)) continue;
        cs_file_head(b, r, i);
        buf_puts(b, STR("  "));
        cs_put_range(b, rec);
        size_t span = cs_row_span(r, i);
        if (span > 1) buf_putf(b, " (%zu matches)", span);
        buf_puts(b, STR(": "));
        buf_puts(b, rec->text);
        buf_putc(b, '\n');
    }
}

/* INVARIANT: runs on records sorted by cs_record_cmp, where a file's records
 * come in line order and a scope sorts before a call on its line. Open scopes
 * then form a stack, and each call takes the innermost one around it. The
 * stack holds copies: the records are compacted in place, over the scopes.
 * A mention on a line that holds a call is dropped, and so are the scopes,
 * leaving calls and mentions in line order. */
static b8 cs_resolve_callers(CsRun *r, size_t *calls, size_t *mentions) {
    CsRecord *open = arena_new(r->scratch, CsRecord, r->n + 1);
    if (!open) return false;
    size_t depth = 0, w = 0;
    Str file = {0};
    u32 call_line = 0;
    *calls = *mentions = 0;
    for (size_t i = 0; i < r->n; i++) {
        CsRecord rec = r->rec[i];
        if (!str_eq(rec.file, file)) {
            file = rec.file;
            depth = 0;
            call_line = 0;
        }
        if (rec.kind == CS_SCOPE) {
            while (depth && open[depth - 1].end_byte <= rec.start_byte) depth--;
            open[depth++] = rec;
            continue;
        }
        if (rec.kind == CS_CALL_SITE) {
            while (depth && open[depth - 1].end_byte <= rec.start_byte) depth--;
            if (depth) rec.scope = open[depth - 1].name;
            call_line = rec.start_line;
            ++*calls;
        } else if (rec.kind == CS_MENTION) {
            if (rec.start_line == call_line) continue;
            ++*mentions;
        }
        r->rec[w++] = rec;
    }
    r->n = w;
    return true;
}

static void cs_body_callers(Buf *b, const CsRun *r) {
    for (size_t i = 0; i < r->n; i++) {
        const CsRecord *rec = &r->rec[i];
        if (cs_same_row(r, i)) continue;
        cs_file_head(b, r, i);
        buf_putf(b, "  %u ", rec->start_line);
        if (rec->kind == CS_CALL_SITE) {
            buf_puts(b, STR("call"));
            if (rec->scope.n) {
                buf_puts(b, STR(" in "));
                buf_puts(b, cs_first_line(rec->scope));
            }
            size_t span = cs_row_span(r, i);
            if (span > 1) buf_putf(b, " (%zu calls)", span);
        } else {
            buf_puts(b, STR("other"));
        }
        buf_puts(b, STR(": "));
        buf_puts(b, rec->text);
        buf_putc(b, '\n');
    }
}

/* ---- arguments ---- */

typedef enum {
    CS_OUTLINE,
    CS_DEFINITION,
    CS_CALLERS,
    CS_PATTERN,
    CS_RULE
} CsMode;

static b8 cs_count(const JVal *j, Str key, size_t dflt, size_t max, size_t *out,
                   char *err, size_t err_cap) {
    const JVal *v = json_get(j, key);
    *out = dflt;
    if (!v || v->type == J_NULL) return true;
    if (v->type != J_NUM || v->u.n < 1 || v->u.n > (f64)max
        || v->u.n != (f64)(size_t)v->u.n) {
        snprintf(err, err_cap, "%.*s must be an integer from 1 to %zu",
                 (i32)key.n, key.p, max);
        return false;
    }
    *out = (size_t)v->u.n;
    return true;
}

static b8 cs_mode(Str s, CsMode *mode) {
    static const char *const names[] = {"outline", "definition", "callers",
                                        "pattern", "rule"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (str_eq(s, str_c(names[i]))) {
            *mode = (CsMode)i;
            return true;
        }
    return false;
}

static void cs_continue_note(Buf *out, const JVal *args, size_t offset,
                             size_t limit) {
    buf_puts(out, STR("[continue with code_search {"));
    b8 first = true;
    for (JVal *m = args->u.obj.head; m; m = m->next) {
        if (str_eq(m->key, STR("offset")) || str_eq(m->key, STR("limit")))
            continue;
        if (!first) buf_putc(out, ',');
        first = false;
        buf_json_str(out, m->key);
        buf_putc(out, ':');
        json_write(out, m);
    }
    buf_putf(out, "%s\"offset\":%zu,\"limit\":%zu}]\n", first ? "" : ",",
             offset, limit);
}

static b8 cs_is_head(Str line) {
    return line.n && line.p[0] == CS_HEAD;
}

static void cs_spill_body(Spill *spill, Str body) {
    size_t off = 0;
    Str line;
    while (str_line(body, &off, &line)) {
        if (cs_is_head(line)) line = str_drop(line, 1);
        spill_put(spill, line.p, line.n);
        spill_put(spill, "\n", 1);
    }
}

static void cs_spill_open(Spill *spill, const JVal *args, Arena *scratch) {
    Buf key;
    buf_init(&key, scratch, 256);
    for (JVal *m = args->u.obj.head; m; m = m->next) {
        if (str_eq(m->key, STR("offset")) || str_eq(m->key, STR("limit")))
            continue;
        buf_json_str(&key, m->key);
        json_write(&key, m);
    }
    spill_open(spill, "code_search", "txt", buf_finish(&key));
}

/* Offsets and limits count result lines; a file line is not one. A page
 * that starts inside a file repeats its file line, and a line too long for
 * the page is clipped so every page moves forward. */
static b8 cs_page(Buf *out, Str head, Str body, const JVal *args, size_t first,
                  size_t limit, Arena *scratch, char *err, size_t err_cap) {
    static Spill spill;
    cs_spill_open(&spill, args, scratch);
    spill_put(&spill, head.p, head.n);
    cs_spill_body(&spill, body);

    buf_puts(out, head);
    size_t total = 0, off = 0;
    Str line, file = {0};
    while (str_line(body, &off, &line)) total += !cs_is_head(line);
    if (first > total && !(first == 1 && total == 0)) {
        spill_finish(&spill, out, false);
        snprintf(err, err_cap,
                 "the result has %zu lines; offset %zu is past its end", total,
                 first);
        return false;
    }
    off = 0;
    for (size_t ln = 1; ln < first;) {
        str_line(body, &off, &line);
        if (cs_is_head(line))
            file = line;
        else
            ln++;
    }
    size_t reserve = 512 + AGENT_SPILL_NOTE_BYTES;
    size_t shown = 0;
    b8 head_due = file.n > 0;
    while (shown < limit && off < body.n) {
        size_t before = off;
        str_line(body, &off, &line);
        if (cs_is_head(line)) {
            file = line;
            head_due = true;
            continue;
        }
        size_t need = line.n + 1 + (head_due ? file.n : 0) + reserve;
        b8 clipped = false;
        if (out->n + need > AGENT_TOOL_RESULT_BYTES) {
            if (shown) {
                off = before;
                break;
            }
            size_t used = reserve + out->n + (head_due ? file.n : 0) + 32;
            size_t room = used < AGENT_TOOL_RESULT_BYTES
                              ? AGENT_TOOL_RESULT_BYTES - used
                              : 0;
            line = str_clip_utf8(line, room);
            clipped = true;
        }
        if (head_due) {
            buf_put(out, file.p + 1, file.n - 1);
            buf_putc(out, '\n');
            head_due = false;
        }
        buf_puts(out, line);
        if (clipped) buf_puts(out, STR(" [line clipped]"));
        buf_putc(out, '\n');
        shown++;
    }
    while (off < body.n) {
        size_t at = off;
        str_line(body, &off, &line);
        if (!cs_is_head(line)) {
            off = at;
            break;
        }
    }
    b8 more = off < body.n;
    if (more) cs_continue_note(out, args, first + shown, limit);
    spill_finish(&spill, out, first > 1 || more);
    if (!buf_ok(out) || out->n > AGENT_TOOL_RESULT_BYTES) {
        snprintf(err, err_cap, "code_search result exceeds the %u byte limit",
                 (unsigned)AGENT_TOOL_RESULT_BYTES);
        return false;
    }
    return true;
}

static const char *cs_path_arg(Str path, Arena *a, char *err, size_t err_cap) {
    if (!path.n) path = STR(".");
    if (memchr(path.p, '\0', path.n)) {
        snprintf(err, err_cap, "path holds a NUL byte");
        return NULL;
    }
    if (path.n >= AGENT_MAX_PATH) {
        snprintf(err, err_cap, "path is longer than %d bytes",
                 AGENT_MAX_PATH - 1);
        return NULL;
    }
    b8 dash = path.p[0] == '-';
    char *z = arena_alloc(a, path.n + 3, 1);
    if (!z) {
        snprintf(err, err_cap, "out of memory for code_search");
        return NULL;
    }
    size_t n = 0;
    if (dash) {
        z[n++] = '.';
        z[n++] = '/';
    }
    memcpy(z + n, path.p, path.n);
    z[n + path.n] = '\0';
    struct stat st;
    if (stat(z, &st) != 0) {
        snprintf(err, err_cap, "%.*s does not exist", (i32)path.n, path.p);
        return NULL;
    }
    return z;
}

static void cs_put_languages(Buf *b) {
    for (size_t i = 0; i < CS_LANGS_N; i++) {
        if (i) buf_puts(b, STR(", "));
        buf_puts(b, str_c(k_cs_langs[i].name));
    }
}

static void cs_put_count(Buf *b, size_t n, const char *one, const char *many) {
    buf_putf(b, "%zu %s", n, n == 1 ? one : many);
}

static void cs_put_in_files(Buf *b, size_t files) {
    if (!files) return;
    buf_puts(b, STR(" in "));
    cs_put_count(b, files, "file", "files");
}

/* ---- the tool ---- */

b8 code_search_run(Str args, Arena *scratch, Buf *out, char *err,
                   size_t err_cap) {
    if (!code_search_available()) {
        snprintf(err, err_cap, "ast-grep is not installed");
        return false;
    }
    JVal *j = json_parse_error(scratch, args, err, err_cap);
    if (!j) return false;
    if (j->type != J_OBJ) {
        snprintf(err, err_cap, "arguments must be an object");
        return false;
    }
    CsMode mode;
    Str mode_name = json_str(j, STR("mode"));
    if (!cs_mode(mode_name, &mode)) {
        snprintf(err, err_cap,
                 "mode must be outline, definition, callers, pattern or rule");
        return false;
    }
    size_t first, limit;
    if (!cs_count(j, STR("offset"), 1, (size_t)1 << 30, &first, err, err_cap)
        || !cs_count(j, STR("limit"), CS_DEFAULT_LINES, CS_MAX_LINES, &limit,
                     err, err_cap))
        return false;

    Str lang_name = json_str(j, STR("lang"));
    const CsLang *lang = NULL;
    if (lang_name.n) {
        if (!cs_lang_valid(lang_name)) {
            snprintf(err, err_cap,
                     "lang must be a language name such as c, csharp or "
                     "python");
            return false;
        }
        lang = cs_lang_find(lang_name);
        if (!lang && mode != CS_PATTERN && mode != CS_RULE) {
            Buf b;
            buf_init(&b, scratch, 256);
            cs_put_languages(&b);
            snprintf(err, err_cap,
                     "outline, definition and callers know %.*s; pattern "
                     "and rule take any language ast-grep supports",
                     (i32)b.n, b.p ? b.p : "");
            return false;
        }
    }
    Str ast_lang = lang ? str_c(lang->name) : lang_name;
    Str name = json_str(j, STR("name"));
    Str pattern = json_str(j, STR("pattern"));
    const JVal *rule = json_get(j, STR("rule"));
    if ((mode == CS_DEFINITION || mode == CS_CALLERS) && !cs_name_valid(name)) {
        snprintf(err, err_cap,
                 "%.*s needs name: an identifier of letters, digits, _ or $, "
                 "up to %u bytes",
                 (i32)mode_name.n, mode_name.p, CS_NAME_MAX);
        return false;
    }
    if ((mode == CS_PATTERN || mode == CS_RULE) && !ast_lang.n) {
        snprintf(err, err_cap, "%.*s needs lang", (i32)mode_name.n,
                 mode_name.p);
        return false;
    }
    if (mode == CS_PATTERN
        && (!str_trim(pattern).n || pattern.n > CS_PATTERN_MAX
            || memchr(pattern.p, '\0', pattern.n))) {
        snprintf(err, err_cap, "pattern needs pattern: up to %u bytes",
                 CS_PATTERN_MAX);
        return false;
    }
    Buf rule_json;
    buf_init(&rule_json, scratch, 1024);
    if (mode == CS_RULE) {
        if (!rule || rule->type != J_OBJ || !rule->u.obj.head) {
            snprintf(err, err_cap, "rule needs rule: an ast-grep rule object");
            return false;
        }
        json_write(&rule_json, rule);
        if (!buf_ok(&rule_json) || rule_json.n > CS_RULE_MAX) {
            snprintf(err, err_cap, "rule is larger than %u bytes", CS_RULE_MAX);
            return false;
        }
    }
    const char *path =
        cs_path_arg(json_str(j, STR("path")), scratch, err, err_cap);
    if (!path) return false;

    Buf rules;
    buf_init(&rules, scratch, 16u << 10);
    Str searched = pattern;
    switch (mode) {
        case CS_OUTLINE: cs_rules_outline(&rules, lang); break;
        case CS_DEFINITION:
            cs_rules_definition(&rules, lang,
                                cs_name_regex(scratch, name, "(<.*)?$"));
            break;
        case CS_CALLERS:
            cs_rules_callers(&rules, lang, cs_name_regex(scratch, name, "$"));
            break;
        case CS_PATTERN:
            searched = cs_rules_pattern(
                &rules, ast_lang, lang && lang->call_context, pattern, scratch);
            break;
        case CS_RULE:
            cs_doc(&rules, "match", ast_lang);
            buf_put(&rules, rule_json.p, rule_json.n);
            buf_putc(&rules, '\n');
            break;
    }
    buf_putc(&rules, '\0');
    CsRun *r = arena_new(scratch, CsRun, 1);
    CsRecord *rec = arena_new(scratch, CsRecord, CS_MAX_RECORDS);
    char *pool = arena_alloc(scratch, CS_POOL_BYTES, 1);
    char *line = arena_alloc(scratch, CS_LINE_BYTES, 1);
    if (!buf_ok(&rules) || !r || !rec || !pool || !line) {
        snprintf(err, err_cap, "out of memory for code_search");
        return false;
    }
    *r = (CsRun){.rec = rec,
                 .pool = pool,
                 .line = line,
                 .line_cap = CS_LINE_BYTES,
                 .full_text = mode == CS_DEFINITION,
                 .name = name,
                 .scratch = scratch};
    const char *argv[CS_ARGV_MAX] = {g_code_search.path,
                                     "scan",
                                     "-c",
                                     "/dev/null",
                                     "--custom-languages",
                                     "ignore",
                                     "--color",
                                     "never",
                                     "--json=stream",
                                     "--inspect",
                                     "summary",
                                     "--inline-rules",
                                     rules.p,
                                     "--",
                                     path,
                                     NULL};
    if (!cs_exec(argv, str_c(path), r, scratch, err, err_cap)) {
        if (!lang && strstr(err, "SgLang"))
            snprintf(err, err_cap, "ast-grep does not support lang %.*s",
                     (i32)lang_name.n, lang_name.p);
        else if (mode == CS_PATTERN)
            cs_pattern_failed(ast_lang, searched, scratch, err, err_cap);
        return false;
    }
    CsMentionScan scan = {0};
    if (mode == CS_CALLERS
        && !cs_scan_mentions(r, lang, path, &scan, err, err_cap))
        return false;
    b8 known_langs = mode != CS_PATTERN && mode != CS_RULE;
    Buf entities;
    buf_init(&entities, scratch, 64u << 10);
    if ((r->skipped || (known_langs && !r->n))
        && !cs_list_files(r, ast_lang, path, &entities, err, err_cap))
        return false;
    Str listed = {entities.p, entities.n};
    CsSkipped sk = {0};
    if (r->skipped
        && !cs_find_skipped(r, ast_lang, listed, mode == CS_CALLERS, &sk, err,
                            err_cap))
        return false;

    size_t calls = 0, mentions = 0;
    qsort(r->rec, r->n, sizeof *r->rec, cs_record_cmp);
    if (mode == CS_CALLERS && !cs_resolve_callers(r, &calls, &mentions)) {
        snprintf(err, err_cap, "out of memory for code_search");
        return false;
    }

    Buf head, body;
    buf_init(&head, scratch, 1024);
    buf_init(&body, scratch, 64u << 10);
    size_t files = cs_files(r);
    switch (mode) {
        case CS_OUTLINE:
            cs_put_count(&head, r->n, "definition", "definitions");
            cs_put_in_files(&head, files);
            buf_putc(&head, '\n');
            cs_body_outline(&body, r);
            break;
        case CS_DEFINITION:
            cs_put_count(&head, r->n, "definition", "definitions");
            buf_putf(&head, " of %.*s\n", (i32)name.n, name.p);
            cs_body_definition(&body, r);
            break;
        case CS_CALLERS:
            cs_put_count(&head, calls, "call", "calls");
            buf_putf(&head, " to %.*s and ", (i32)name.n, name.p);
            cs_put_count(&head, mentions, "other mention", "other mentions");
            cs_put_in_files(&head, files);
            buf_putc(&head, '\n');
            cs_body_callers(&body, r);
            break;
        case CS_PATTERN:
        case CS_RULE:
            cs_put_count(&head, r->n, "match", "matches");
            cs_put_in_files(&head, files);
            buf_putc(&head, '\n');
            cs_body_matches(&body, r);
            break;
    }
    if (!r->n && mode == CS_PATTERN)
        cs_debug_pattern(ast_lang, searched, &head, scratch);
    if (known_langs && !r->n
        && !cs_any_file_searched(listed, lang_name.n > 0)) {
        if (lang_name.n) {
            buf_putf(&head, "[no %.*s file at %s]\n", (i32)lang_name.n,
                     lang_name.p, path);
        } else {
            buf_puts(&head, STR("[no "));
            cs_put_languages(&head);
            buf_putf(&head,
                     " file at %s; pattern and rule take other languages]\n",
                     path);
        }
    }
    if (r->dropped)
        buf_putf(&head,
                 "[%zu more not listed: one search lists at most %u; search "
                 "a smaller path]\n",
                 r->dropped, CS_MAX_RECORDS);
    if (r->too_large == 1)
        buf_puts(&head, STR("[1 match not listed: its code is too large; "
                            "search with a narrower pattern]\n"));
    else if (r->too_large)
        buf_putf(&head,
                 "[%zu matches not listed: their code is too large; search "
                 "with a narrower pattern]\n",
                 r->too_large);
    for (size_t i = 0; i < scan.big_n; i++)
        buf_putf(&head,
                 "[other mentions not listed in %.*s: the file is over %u "
                 "MB]\n",
                 (i32)scan.big[i].n, scan.big[i].p, CS_MENTION_FILE_MAX >> 20);
    if (scan.big_more)
        buf_putf(&head,
                 "[other mentions not listed in %zu more files over "
                 "%u MB]\n",
                 scan.big_more, CS_MENTION_FILE_MAX >> 20);
    for (size_t i = 0; i < sk.big_n; i++) {
        if (sk.big_read[i])
            buf_putf(&head,
                     "[%.*s is over 3 MB and 200000 lines, too large for "
                     "ast-grep; its calls are listed as other]\n",
                     (i32)sk.big[i].n, sk.big[i].p);
        else
            buf_putf(&head,
                     "[not searched: %.*s is over 3 MB and 200000 lines]\n",
                     (i32)sk.big[i].n, sk.big[i].p);
    }
    if (sk.big_more)
        buf_putf(&head,
                 "[not searched: %zu more files over 3 MB and 200000 lines]\n",
                 sk.big_more);
    if (sk.not_utf8 == 1)
        buf_puts(&head, STR("[not searched: 1 file that is not UTF-8]\n"));
    else if (sk.not_utf8)
        buf_putf(&head, "[not searched: %zu files that are not UTF-8]\n",
                 sk.not_utf8);
    if (r->pool_full)
        buf_puts(&head, STR("[some lines not listed: the result is over 8 MB; "
                            "search a smaller path]\n"));
    if (!buf_ok(&head) || !buf_ok(&body)) {
        snprintf(err, err_cap, "out of memory for code_search");
        return false;
    }
    return cs_page(out, buf_finish(&head), buf_finish(&body), j, first, limit,
                   scratch, err, err_cap);
}
