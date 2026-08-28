#include "agent.h"
#include "../vendor/lexbor/bridge.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <curl/curl.h>

#define WEB_EXTRACT_BYTES (4u << 20)
#define WEB_TITLE_BYTES   512u
#define WEB_SNIPPET_BYTES 1024u

typedef struct {
    void (*idle)(void *ud);
    void *idle_ud;
    i32 idle_fd;
    const volatile sig_atomic_t *interrupt_flag;
} WebHooks;

static WebHooks g_web_hooks = {NULL, NULL, -1, NULL};

void web_set_idle(void (*fn)(void *ud), void *ud, i32 idle_fd,
                  const volatile sig_atomic_t *interrupt_flag) {
    g_web_hooks = (WebHooks){fn, ud, idle_fd, interrupt_flag};
}

static i32 search_interval_ms(void) {
#ifdef AGENT_TESTING
    const char *value = getenv(AGENT_ENV_PREFIX "TEST_WEB_SEARCH_INTERVAL_MS");
    if (value && *value) {
        b8 ok = false;
        i64 ms = str_int(str_c(value), &ok);
        if (ok && ms >= 0 && ms <= AGENT_WEB_SEARCH_INTERVAL_MS) return (i32)ms;
    }
#endif
    return AGENT_WEB_SEARCH_INTERVAL_MS;
}


static b8 search_wait_until(f64 until) {
    enum { SLICE_MS = 50 };
    for (;;) {
        if (g_web_hooks.interrupt_flag && *g_web_hooks.interrupt_flag)
            return false;
        f64 left = until - agent_now_seconds();
        if (left <= 0) return true;
        if (g_web_hooks.idle) g_web_hooks.idle(g_web_hooks.idle_ud);
        i32 ms =
            left * 1000.0 < (f64)SLICE_MS ? (i32)(left * 1000.0) + 1 : SLICE_MS;
        struct timespec ts = {0, (long)ms * 1000000L};
        nanosleep(&ts, NULL);
    }
}


static void search_attempts_add(char *dst, size_t cap, size_t *n,
                                const char *msg) {
    if (*n + 1 >= cap) return;
    int w = snprintf(dst + *n, cap - *n, "%s%s", *n ? "; " : "", msg);
    if (w < 0) return;
    *n = (size_t)w >= cap - *n ? cap - 1 : *n + (size_t)w;
}

static b8 web_arg_cstr(Str s, char *z, size_t cap, const char *what, char *err,
                       size_t err_cap) {
    if (!s.p) {
        snprintf(err, err_cap, "missing %s", what);
        return false;
    }
    if (!s.n) {
        snprintf(err, err_cap, "%s is empty", what);
        return false;
    }
    if (s.n >= cap) {
        snprintf(err, err_cap, "%s too long: %zu bytes, limit %zu", what, s.n,
                 cap - 1);
        return false;
    }
    if (memchr(s.p, '\0', s.n)) {
        snprintf(err, err_cap, "%s contains a nul byte", what);
        return false;
    }
    memcpy(z, s.p, s.n);
    z[s.n] = '\0';
    return true;
}

static b8 web_arg_count(const JVal *j, Str key, size_t dflt, size_t max,
                        size_t *out, char *err, size_t err_cap) {
    const JVal *v = json_get(j, key);
    if (!v || v->type == J_NULL) {
        *out = dflt;
        return true;
    }
    if (v->type != J_NUM || v->u.n < 1 || v->u.n > (f64)max
        || v->u.n != (f64)(u64)v->u.n) {
        snprintf(err, err_cap, "%.*s must be a whole number in 1..%zu",
                 (i32)key.n, key.p, max);
        return false;
    }
    *out = (size_t)v->u.n;
    return true;
}

static JVal *web_args(Str args, Arena *scratch, char *err, size_t err_cap) {
    JVal *j = json_parse(scratch, args);
    if (!j || j->type != J_OBJ) {
        snprintf(err, err_cap, "bad args json");
        return NULL;
    }
    return j;
}

static b8 web_public_only(void) {
#ifdef AGENT_TESTING
    const char *allow = getenv(AGENT_ENV_PREFIX "TEST_WEB_ALLOW_PRIVATE");
    if (allow && !strcmp(allow, "1")) return false;
#endif
    return true;
}

static i32 web_request(const char *url, const char *operation, Buf *body,
                       const char *header, HttpUrlReq *req) {
    *req = (HttpUrlReq){
        .url = url,
        .operation = operation,
        .out = body,
        .max_bytes = AGENT_WEB_BODY_BYTES,
        .connect_timeout_ms = 5000,
        .timeout_ms = 20000,
        .max_redirects = 5,
        .public_only = web_public_only(),
        .header = {header, NULL},
        .interrupt_flag = g_web_hooks.interrupt_flag,
        .idle_fd = g_web_hooks.idle_fd,
        .on_idle = g_web_hooks.idle,
        .idle_ud = g_web_hooks.idle_ud,
    };
    return http_url_get(req);
}

static b8 tag_is(AgentHtmlNode *node, const char *name) {
    size_t n = 0;
    const char *tag = agent_html_tag(node, &n);
    size_t want = strlen(name);
    return tag && n == want && !memcmp(tag, name, n);
}

static b8 ignored_tag(AgentHtmlNode *node) {
    static const char *const ignored[] = {
        "script", "style", "noscript", "template", "svg",    "canvas",
        "iframe", "form",  "button",   "nav",      "footer", "aside",
    };
    for (size_t i = 0; i < sizeof ignored / sizeof ignored[0]; i++)
        if (tag_is(node, ignored[i])) return true;
    return false;
}

typedef b8 (*HtmlEnter)(AgentHtmlNode *node, void *ud);
typedef void (*HtmlLeave)(AgentHtmlNode *node, void *ud);

static void html_walk(AgentHtmlNode *root, HtmlEnter enter, HtmlLeave leave,
                      void *ud) {
    if (!root) return;
    AgentHtmlNode *node = root;
    b8 entering = true;
    for (;;) {
        if (entering) {
            b8 descend = !enter || enter(node, ud);
            AgentHtmlNode *child =
                descend ? agent_html_first_child(node) : NULL;
            if (child) {
                node = child;
                continue;
            }
        }
        if (leave) leave(node, ud);
        if (node == root) break;
        AgentHtmlNode *next = agent_html_next(node);
        if (next) {
            node = next;
            entering = true;
        } else {
            node = agent_html_parent(node);
            entering = false;
        }
    }
}

typedef struct {
    Buf *buf;
    size_t max;
    b8 pending_space;
    b8 too_large;
} Normal;

static void normal_put(Normal *n, const char *p, size_t len) {
    if (n->too_large) return;
    if (n->buf->n > n->max || len > n->max - n->buf->n) {
        n->too_large = true;
        return;
    }
    buf_put(n->buf, p, len);
    if (!buf_ok(n->buf)) n->too_large = true;
}

static size_t utf8_char(const u8 *p, size_t n) {
    if (!n) return 0;
    if (p[0] < 0x80) return 1;
    size_t need = p[0] >= 0xc2 && p[0] <= 0xdf   ? 2
                  : p[0] >= 0xe0 && p[0] <= 0xef ? 3
                  : p[0] >= 0xf0 && p[0] <= 0xf4 ? 4
                                                 : 0;
    if (!need || need > n) return 0;
    for (size_t i = 1; i < need; i++)
        if ((p[i] & 0xc0u) != 0x80u) return 0;
    if (need == 3
        && ((p[0] == 0xe0 && p[1] < 0xa0) || (p[0] == 0xed && p[1] >= 0xa0)))
        return 0;
    if (need == 4
        && ((p[0] == 0xf0 && p[1] < 0x90) || (p[0] == 0xf4 && p[1] >= 0x90)))
        return 0;
    return need;
}

static void normal_words(Normal *n, Str s, b8 pre) {
    for (size_t i = 0; i < s.n;) {
        u8 c = (u8)s.p[i];
        if (!pre && c < 0x80 && isspace(c)) {
            n->pending_space = n->buf->n != 0;
            i++;
            continue;
        }
        if (pre && c == '\r') {
            normal_put(n, "\n", 1);
            i += i + 1 < s.n && s.p[i + 1] == '\n' ? 2 : 1;
            continue;
        }
        if (n->pending_space) {
            if (n->buf->n && n->buf->p[n->buf->n - 1] != '\n'
                && n->buf->p[n->buf->n - 1] != ' ')
                normal_put(n, " ", 1);
            n->pending_space = false;
        }
        size_t take = utf8_char((const u8 *)s.p + i, s.n - i);
        if (!take) {
            normal_put(n, "\xef\xbf\xbd", 3);
            i++;
        } else {
            normal_put(n, s.p + i, take);
            i += take;
        }
    }
}

static void normal_break(Normal *n, size_t lines) {
    n->pending_space = false;
    while (n->buf->n
           && (n->buf->p[n->buf->n - 1] == ' '
               || n->buf->p[n->buf->n - 1] == '\t'))
        n->buf->n--;
    size_t have = 0;
    for (size_t i = n->buf->n; i && n->buf->p[i - 1] == '\n'; i--) have++;
    while (have++ < lines) normal_put(n, "\n", 1);
}

static Str normal_finish(Normal *n) {
    n->pending_space = false;
    while (n->buf->n && isspace((u8)n->buf->p[n->buf->n - 1])) n->buf->n--;
    return buf_finish(n->buf);
}

typedef struct {
    Normal norm;
} VisibleCtx;

static b8 visible_enter(AgentHtmlNode *node, void *ud) {
    VisibleCtx *v = (VisibleCtx *)ud;
    if (ignored_tag(node)) return false;
    size_t n = 0;
    const char *text = agent_html_text(node, &n);
    if (text) normal_words(&v->norm, (Str){text, n}, false);
    return true;
}

static Str visible_text(AgentHtmlNode *node, Arena *scratch, size_t max,
                        b8 *too_large) {
    Buf b;
    buf_init(&b, scratch, max < 256 ? max + 1 : 256);
    VisibleCtx v = {{&b, max, false, false}};
    html_walk(node, visible_enter, NULL, &v);
    Str out = normal_finish(&v.norm);
    *too_large = !buf_ok(&b);
    return out;
}

typedef struct {
    size_t chars;
} CountCtx;

static b8 count_enter(AgentHtmlNode *node, void *ud) {
    CountCtx *c = (CountCtx *)ud;
    if (ignored_tag(node)) return false;
    size_t n = 0;
    const char *text = agent_html_text(node, &n);
    for (size_t i = 0; text && i < n && c->chars < 200; i++)
        if (!isspace((u8)text[i]) || (i && !isspace((u8)text[i - 1])))
            c->chars++;
    return c->chars < 200;
}

static b8 qualifying_text(AgentHtmlNode *node) {
    CountCtx c = {0};
    html_walk(node, count_enter, NULL, &c);
    return c.chars >= 200;
}

typedef struct {
    const char *tag;
    b8 qualify;
    AgentHtmlNode *found;
} FindCtx;

static b8 find_enter(AgentHtmlNode *node, void *ud) {
    FindCtx *f = (FindCtx *)ud;
    if (ignored_tag(node)) return false;
    if (!f->found && tag_is(node, f->tag)
        && (!f->qualify || qualifying_text(node)))
        f->found = node;
    return f->found == NULL;
}

static AgentHtmlNode *find_tag(AgentHtmlNode *root, const char *tag,
                               b8 qualify) {
    FindCtx f = {tag, qualify, NULL};
    html_walk(root, find_enter, NULL, &f);
    return f.found;
}

static b8 http_url_ok(const char *url) {
    if (!curl_load(NULL, 0)) return false;
    CURLU *u = curl_url();
    if (!u) return false;
    CURLUcode rc = curl_url_set(u, CURLUPART_URL, url, 0);
    char *scheme = NULL, *host = NULL, *user = NULL, *pass = NULL;
    if (rc == CURLUE_OK) rc = curl_url_get(u, CURLUPART_SCHEME, &scheme, 0);
    if (rc == CURLUE_OK) rc = curl_url_get(u, CURLUPART_HOST, &host, 0);
    CURLUcode urc = curl_url_get(u, CURLUPART_USER, &user, 0);
    CURLUcode prc = curl_url_get(u, CURLUPART_PASSWORD, &pass, 0);
    b8 ok = rc == CURLUE_OK && scheme && host && *host
            && (!strcasecmp(scheme, "http") || !strcasecmp(scheme, "https"))
            && !(urc == CURLUE_OK && user && *user)
            && !(prc == CURLUE_OK && pass && *pass);
    curl_free(scheme);
    curl_free(host);
    curl_free(user);
    curl_free(pass);
    curl_url_cleanup(u);
    return ok;
}

static b8 resolve_url(const char *base, Str ref,
                      char out[AGENT_WEB_URL_BYTES]) {
    if (!ref.n || ref.n >= AGENT_WEB_URL_BYTES || memchr(ref.p, '\0', ref.n))
        return false;
    char z[AGENT_WEB_URL_BYTES];
    memcpy(z, ref.p, ref.n);
    z[ref.n] = '\0';
    if (!curl_load(NULL, 0)) return false;
    CURLU *u = curl_url();
    if (!u) return false;
    CURLUcode rc = curl_url_set(u, CURLUPART_URL, base, 0);
    if (rc == CURLUE_OK) rc = curl_url_set(u, CURLUPART_URL, z, 0);
    char *url = NULL;
    if (rc == CURLUE_OK) rc = curl_url_get(u, CURLUPART_URL, &url, 0);
    b8 ok = rc == CURLUE_OK && url && strlen(url) < AGENT_WEB_URL_BYTES
            && http_url_ok(url);
    if (ok) memcpy(out, url, strlen(url) + 1);
    curl_free(url);
    curl_url_cleanup(u);
    return ok;
}

typedef struct {
    Normal norm;
    const char *base;
    size_t pre_depth;
} ExtractCtx;

static b8 heading_level(AgentHtmlNode *node, size_t *level) {
    size_t n = 0;
    const char *tag = agent_html_tag(node, &n);
    if (tag && n == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
        *level = (size_t)(tag[1] - '0');
        return true;
    }
    return false;
}

static b8 extract_enter(AgentHtmlNode *node, void *ud) {
    ExtractCtx *x = (ExtractCtx *)ud;
    if (ignored_tag(node)) return false;
    size_t tn = 0;
    const char *text = agent_html_text(node, &tn);
    if (text) {
        normal_words(&x->norm, (Str){text, tn}, x->pre_depth != 0);
        return true;
    }
    size_t level = 0;
    if (heading_level(node, &level)) {
        normal_break(&x->norm, 2);
        for (size_t i = 0; i < level; i++) normal_put(&x->norm, "#", 1);
        normal_put(&x->norm, " ", 1);
    } else if (tag_is(node, "p") || tag_is(node, "blockquote")
               || tag_is(node, "table") || tag_is(node, "section")
               || tag_is(node, "article") || tag_is(node, "main")) {
        normal_break(&x->norm, 2);
        if (tag_is(node, "blockquote")) normal_put(&x->norm, "> ", 2);
    } else if (tag_is(node, "br")) {
        normal_break(&x->norm, 1);
    } else if (tag_is(node, "li")) {
        normal_break(&x->norm, 1);
        normal_put(&x->norm, "- ", 2);
    } else if (tag_is(node, "pre")) {
        normal_break(&x->norm, 2);
        normal_put(&x->norm, "```\n", 4);
        x->pre_depth++;
    } else if (tag_is(node, "code") && !x->pre_depth) {
        normal_put(&x->norm, "`", 1);
    } else if (tag_is(node, "tr")) {
        normal_break(&x->norm, 1);
    } else if (tag_is(node, "td") || tag_is(node, "th")) {
        if (x->norm.buf->n && x->norm.buf->p[x->norm.buf->n - 1] != '\n')
            normal_put(&x->norm, " | ", 3);
    }
    return true;
}

static void extract_leave(AgentHtmlNode *node, void *ud) {
    ExtractCtx *x = (ExtractCtx *)ud;
    if (ignored_tag(node)) return;
    size_t level = 0;
    if (heading_level(node, &level) || tag_is(node, "p")
        || tag_is(node, "blockquote") || tag_is(node, "section")
        || tag_is(node, "article") || tag_is(node, "main")) {
        normal_break(&x->norm, 2);
    } else if (tag_is(node, "li") || tag_is(node, "tr")) {
        normal_break(&x->norm, 1);
    } else if (tag_is(node, "pre")) {
        if (x->pre_depth) x->pre_depth--;
        normal_break(&x->norm, 1);
        normal_put(&x->norm, "```", 3);
        normal_break(&x->norm, 2);
    } else if (tag_is(node, "code") && !x->pre_depth) {
        normal_put(&x->norm, "`", 1);
    } else if (tag_is(node, "a")) {
        size_t n = 0;
        const char *href = agent_html_attr(node, "href", 4, &n);
        char resolved[AGENT_WEB_URL_BYTES];
        if (href && resolve_url(x->base, (Str){href, n}, resolved)) {
            normal_put(&x->norm, " (", 2);
            normal_put(&x->norm, resolved, strlen(resolved));
            normal_put(&x->norm, ")", 1);
        }
    }
}

static Str html_extract(AgentHtmlDoc *doc, const char *effective,
                        Arena *scratch, Str *title,
                        char base[AGENT_WEB_URL_BYTES], b8 *too_large) {
    AgentHtmlNode *root = agent_html_root(doc);
    AgentHtmlNode *title_node = find_tag(root, "title", false);
    b8 title_large = false;
    *title = title_node ? visible_text(title_node, scratch, WEB_TITLE_BYTES,
                                       &title_large)
                        : (Str){0};
    (void)title_large;

    memcpy(base, effective, strlen(effective) + 1);
    AgentHtmlNode *base_node = find_tag(root, "base", false);
    if (base_node) {
        size_t n = 0;
        const char *href = agent_html_attr(base_node, "href", 4, &n);
        char resolved[AGENT_WEB_URL_BYTES];
        if (href && resolve_url(effective, (Str){href, n}, resolved))
            memcpy(base, resolved, strlen(resolved) + 1);
    }

    AgentHtmlNode *selected = find_tag(root, "main", true);
    if (!selected) selected = find_tag(root, "article", true);
    if (!selected) selected = agent_html_body(doc);
    if (!selected) selected = root;
    Buf b;
    buf_init(&b, scratch, 8192);
    ExtractCtx x = {{&b, WEB_EXTRACT_BYTES, false, false}, base, 0};
    html_walk(selected, extract_enter, extract_leave, &x);
    Str body = normal_finish(&x.norm);
    *too_large = x.norm.too_large || !buf_ok(&b);
    return body;
}

static Str plain_extract(Str source, Arena *scratch, b8 *too_large) {
    Buf b;
    buf_init(&b, scratch, 8192);
    Normal n = {&b, WEB_EXTRACT_BYTES, false, false};
    size_t off = 0;
    while (off < source.n) {
        size_t start = off;
        while (off < source.n && source.p[off] != '\n' && source.p[off] != '\r')
            off++;
        normal_words(&n, (Str){source.p + start, off - start}, true);
        normal_break(&n, 1);
        if (off < source.n && source.p[off] == '\r' && off + 1 < source.n
            && source.p[off + 1] == '\n')
            off += 2;
        else if (off < source.n)
            off++;
    }
    Str out = normal_finish(&n);
    *too_large = n.too_large || !buf_ok(&b);
    return out;
}

static b8 page_output(Buf *out, Str title, const char *effective,
                      const char *input, Str body, size_t first, size_t limit,
                      Arena *scratch, char *err, size_t err_cap) {
    static Spill spill;
    spill_open(&spill, "page_fetch", "txt", str_c(input));
    spill_put(&spill, body.p, body.n);
    if (body.n && body.p[body.n - 1] != '\n') spill_put(&spill, "\n", 1);

    if (!title.n) title = STR("(untitled)");
    buf_puts(out, STR("External page (untrusted): "));
    buf_puts(out, str_clip_utf8(title, WEB_TITLE_BYTES));
    buf_puts(out, STR("\nFinal URL: "));
    buf_puts(out, str_c(effective));
    buf_puts(out, STR("\n\n"));
    if (!buf_ok(out) || out->n >= AGENT_TOOL_RESULT_BYTES) {
        spill_finish(&spill, out, false);
        snprintf(err, err_cap, "page header exceeds the %u byte result limit",
                 (unsigned)AGENT_TOOL_RESULT_BYTES);
        return false;
    }

    size_t total = str_lines(body);
    if (first > total && !(first == 1 && total == 0)) {
        spill_finish(&spill, out, false);
        snprintf(err, err_cap,
                 "page has %zu body lines; offset %zu is past its end", total,
                 first);
        return false;
    }
    size_t off = 0;
    Str line;
    for (size_t ln = 1; ln < first; ln++) str_line(body, &off, &line);
    size_t shown = 0;
    size_t reserve = strlen(input) * 2 + 192;
    if (spill.fd >= 0) reserve += AGENT_SPILL_NOTE_BYTES;
    if (reserve > AGENT_TOOL_RESULT_BYTES / 2)
        reserve = AGENT_TOOL_RESULT_BYTES / 2;
    while (shown < limit && str_line(body, &off, &line)) {
        if (line.n + 1 > AGENT_TOOL_RESULT_BYTES - out->n
            || out->n + line.n + 1 + reserve > AGENT_TOOL_RESULT_BYTES)
            break;
        buf_puts(out, line);
        buf_putc(out, '\n');
        shown++;
    }
    if (off < body.n) {
        Buf note;
        buf_init(&note, scratch, 256);
        buf_puts(&note, STR("[continue with page_fetch {\"url\":"));
        buf_json_str(&note, str_c(input));
        buf_putf(&note, ",\"offset\":%zu,\"limit\":%zu}]", first + shown,
                 limit);
        Str n = buf_finish(&note);
        if (!buf_ok(&note) || n.n + out->n > AGENT_TOOL_RESULT_BYTES) {
            spill_finish(&spill, out, false);
            snprintf(err, err_cap,
                     "continuation call exceeds the result limit");
            return false;
        }
        buf_puts(out, n);
    }
    spill_finish(&spill, out, first > 1 || off < body.n);
    if (!buf_ok(out) || out->n > AGENT_TOOL_RESULT_BYTES) {
        snprintf(err, err_cap, "page result exceeds the %u byte limit",
                 (unsigned)AGENT_TOOL_RESULT_BYTES);
        return false;
    }
    return true;
}

static void media_type(char type[AGENT_WEB_TYPE_BYTES]) {
    char *semi = strchr(type, ';');
    if (semi) *semi = '\0';
    size_t n = strlen(type);
    while (n && isspace((u8)type[n - 1])) type[--n] = '\0';
    size_t start = 0;
    while (type[start] && isspace((u8)type[start])) start++;
    if (start) memmove(type, type + start, strlen(type + start) + 1);
    for (size_t i = 0; type[i]; i++) type[i] = (char)tolower((u8)type[i]);
}

static b8 type_suffix(const char *type, const char *suffix) {
    size_t tn = strlen(type), sn = strlen(suffix);
    return tn >= sn && !memcmp(type + tn - sn, suffix, sn);
}

b8 page_fetch_run(Str args, Arena *scratch, Buf *out, char *err,
                  size_t err_cap) {
    JVal *j = web_args(args, scratch, err, err_cap);
    if (!j) return false;
    char url[AGENT_WEB_URL_BYTES];
    if (!web_arg_cstr(json_str(j, STR("url")), url, sizeof url, "URL", err,
                      err_cap))
        return false;
    if (!http_url_ok(url)) {
        snprintf(err, err_cap,
                 "URL must be a well-formed HTTP(S) URL without credentials");
        return false;
    }
    size_t first, limit;
    if (!web_arg_count(j, STR("offset"), 1, 1u << 30, &first, err, err_cap)
        || !web_arg_count(j, STR("limit"), AGENT_READ_LINES, AGENT_READ_LINES,
                          &limit, err, err_cap))
        return false;

    Buf source;
    buf_init(&source, scratch, 65536);
    HttpUrlReq req;
    i32 rc = web_request(url, "page_fetch", &source, NULL, &req);
    if (rc != 0) {
        if (rc < 0)
            snprintf(err, err_cap, "HTTP status %lld", (long long)-rc);
        else
            snprintf(err, err_cap, "%s",
                     req.failure[0] ? req.failure : "web request failed");
        return false;
    }
    Str raw = buf_finish(&source);
    if (!buf_ok(&source)) {
        snprintf(err, err_cap, "response does not fit in memory");
        return false;
    }
    media_type(req.content_type);
    const char *effective = req.effective_url[0] ? req.effective_url : url;
    Str title = {0}, body = {0};
    b8 too_large = false;
    if (!strcmp(req.content_type, "text/html")
        || !strcmp(req.content_type, "application/xhtml+xml")) {
        AgentHtmlDoc *doc = agent_html_parse(raw.p, raw.n);
        if (!doc) {
            snprintf(err, err_cap, "HTML parsing failed");
            return false;
        }
        char base[AGENT_WEB_URL_BYTES];
        body = html_extract(doc, effective, scratch, &title, base, &too_large);
        agent_html_destroy(doc);
    } else if (!strncmp(req.content_type, "text/", 5)
               || !strcmp(req.content_type, "application/json")
               || !strcmp(req.content_type, "application/xml")
               || type_suffix(req.content_type, "+json")
               || type_suffix(req.content_type, "+xml")) {
        body = plain_extract(raw, scratch, &too_large);
    } else {
        snprintf(err, err_cap, "unsupported media type: %s", req.content_type);
        return false;
    }
    if (too_large) {
        snprintf(err, err_cap, "extracted text exceeds %u bytes",
                 (unsigned)WEB_EXTRACT_BYTES);
        return false;
    }
    return page_output(out, title, effective, url, body, first, limit, scratch,
                       err, err_cap);
}

static b8 class_has(AgentHtmlNode *node, const char *want) {
    size_t n = 0;
    const char *value = agent_html_attr(node, "class", 5, &n);
    size_t wn = strlen(want), off = 0;
    while (value && off < n) {
        while (off < n && isspace((u8)value[off])) off++;
        size_t start = off;
        while (off < n && !isspace((u8)value[off])) off++;
        if (off - start == wn && !memcmp(value + start, want, wn)) return true;
    }
    return false;
}

static i32 hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static Str percent_decode(Str s, Arena *scratch, b8 *ok) {
    char *p = arena_new(scratch, char, s.n + 1);
    if (!p) {
        *ok = false;
        return (Str){0};
    }
    size_t n = 0;
    for (size_t i = 0; i < s.n; i++) {
        if (s.p[i] == '%') {
            if (i + 2 >= s.n) {
                *ok = false;
                return (Str){0};
            }
            i32 hi = hex_digit(s.p[i + 1]), lo = hex_digit(s.p[i + 2]);
            if (hi < 0 || lo < 0) {
                *ok = false;
                return (Str){0};
            }
            p[n++] = (char)((hi << 4) | lo);
            i += 2;
        } else {
            p[n++] = s.p[i] == '+' ? ' ' : s.p[i];
        }
    }
    p[n] = '\0';
    *ok = memchr(p, '\0', n) == NULL;
    return (Str){p, n};
}

static Str result_url(Str href, Arena *scratch, b8 *ok) {
    *ok = false;
    const char *q = memchr(href.p, '?', href.n);
    if (q) {
        size_t off = (size_t)(q - href.p) + 1;
        while (off < href.n) {
            size_t start = off;
            while (off < href.n && href.p[off] != '=' && href.p[off] != '&')
                off++;
            Str key = {href.p + start, off - start};
            if (off < href.n && href.p[off] == '=') {
                size_t val = ++off;
                while (off < href.n && href.p[off] != '&') off++;
                Str raw = {href.p + val, off - val};
                if (str_eq(key, STR("uddg"))) {
                    Str decoded = percent_decode(raw, scratch, ok);
                    if (*ok && decoded.n < AGENT_WEB_URL_BYTES
                        && http_url_ok(decoded.p))
                        return decoded;
                    *ok = false;
                    return (Str){0};
                }
            }
            if (off < href.n) off++;
        }
    }
    if (href.n >= AGENT_WEB_URL_BYTES) return (Str){0};
    Str copy = str_dup(scratch, href);
    if (copy.p && http_url_ok(copy.p)) *ok = true;
    return *ok ? copy : (Str){0};
}

static b8 contains_ci(Str haystack, const char *needle) {
    size_t nn = strlen(needle);
    if (!nn || nn > haystack.n) return false;
    for (size_t i = 0; i + nn <= haystack.n; i++) {
        size_t k = 0;
        while (k < nn
               && tolower((u8)haystack.p[i + k]) == tolower((u8)needle[k]))
            k++;
        if (k == nn) return true;
    }
    return false;
}

typedef struct {
    Str title;
    Str url;
    Str snippet;
} SearchResult;

typedef struct {
    const char *result;
    const char *link[2];
    const char *title;
    const char *snippet[2];
    b8 link_in_heading;
} SearchLayout;

typedef struct {
    Arena *scratch;
    const SearchLayout *layout;
    SearchResult result[10];
    size_t n;
    size_t layout_links;
    size_t current;
    size_t heading;
    b8 in_result;
    b8 linked;
    b8 oom;
    b8 challenge;
    b8 explicit_empty;
} SearchCtx;

static b8 class_any(AgentHtmlNode *node, const char *const tokens[2]) {
    for (size_t i = 0; i < 2; i++)
        if (tokens[i] && class_has(node, tokens[i])) return true;
    return false;
}

static b8 search_link_here(const SearchCtx *s, AgentHtmlNode *node) {
    const SearchLayout *lay = s->layout;
    if (lay->link[0]) return class_any(node, lay->link);
    if (!s->in_result || s->linked) return false;
    return !lay->link_in_heading || s->heading > 0;
}

static Str search_text(SearchCtx *s, AgentHtmlNode *node, size_t max) {
    b8 large = false;
    Str text = visible_text(node, s->scratch, max, &large);
    if (large) s->oom = true;
    return text;
}

static b8 search_enter(AgentHtmlNode *node, void *ud) {
    SearchCtx *s = (SearchCtx *)ud;
    const SearchLayout *lay = s->layout;
    if (class_has(node, "challenge-form") || class_has(node, "anomaly-modal"))
        s->challenge = true;
    if (class_has(node, "no-results")) s->explicit_empty = true;
    if (tag_is(node, "h2")) s->heading++;
    if (lay->result && class_has(node, lay->result)) {
        s->in_result = true;
        s->linked = false;
        s->current = SIZE_MAX;
    }
    if (tag_is(node, "a") && search_link_here(s, node)) {
        s->layout_links++;
        s->linked = true;
        s->current = SIZE_MAX;
        size_t hn = 0;
        const char *href = agent_html_attr(node, "href", 4, &hn);
        b8 ok = false;
        Str url =
            href ? result_url((Str){href, hn}, s->scratch, &ok) : (Str){0};

        Str title =
            lay->title ? (Str){0} : search_text(s, node, WEB_TITLE_BYTES);
        if (!ok || (!lay->title && !title.n) || s->n == 10) return true;
        for (size_t i = 0; i < s->n; i++)
            if (str_eq(s->result[i].url, url)) return true;
        s->result[s->n] = (SearchResult){title, url, {0}};
        s->current = s->n++;
    } else if (lay->title && class_has(node, lay->title) && s->current < s->n
               && !s->result[s->current].title.n) {
        s->result[s->current].title = search_text(s, node, WEB_TITLE_BYTES);
    } else if (class_any(node, lay->snippet) && s->current < s->n
               && !s->result[s->current].snippet.n) {
        s->result[s->current].snippet = search_text(s, node, WEB_SNIPPET_BYTES);
    }
    return true;
}

static void search_leave(AgentHtmlNode *node, void *ud) {
    SearchCtx *s = (SearchCtx *)ud;
    if (tag_is(node, "h2") && s->heading) s->heading--;
    if (s->layout->result && class_has(node, s->layout->result))
        s->in_result = false;
}


static void search_compact(SearchCtx *s) {
    size_t n = 0;
    for (size_t i = 0; i < s->n; i++)
        if (s->result[i].title.n) s->result[n++] = s->result[i];
    s->n = n;
    s->current = SIZE_MAX;
}

static b8 encode_query(Str query, Buf *url) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < query.n; i++) {
        u8 c = (u8)query.p[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            buf_putc(url, (char)c);
        } else {
            char escaped[3] = {'%', hex[c >> 4], hex[c & 15u]};
            buf_put(url, escaped, sizeof escaped);
        }
    }
    return buf_ok(url);
}


typedef enum {
    ENGINE_DDG_LITE,
    ENGINE_DDG_HTML,
    ENGINE_BRAVE,
    ENGINE_BRAVE_API,
    ENGINE_GOOGLE,
    ENGINE_SEARXNG,
    ENGINE_N
} SearchEngine;

typedef struct {
    const char *object;
    const char *array;
    const char *title, *url, *snippet;
} SearchShape;

enum { NEED_KEY = 1u, NEED_ENGINE_ID = 2u, NEED_ENDPOINT = 4u };

typedef struct {
    const char *label;
    const char *base;
    const char *path;
    const char *header;
    u8 needs;
    b8 json;
    SearchLayout layout;
    SearchShape shape;
} SearchEngineSpec;

static const SearchEngineSpec k_engine[ENGINE_N] = {
    [ENGINE_DDG_LITE] = {"lite",
                         "https://lite.duckduckgo.com",
                         "/lite/?q=",
                         NULL,
                         0,
                         false,
                         {NULL,
                          {"result-link", "result__a"},
                          NULL,
                          {"result-snippet", "result__snippet"},
                          false},
                         {0}},
    [ENGINE_DDG_HTML] = {"html",
                         "https://html.duckduckgo.com",
                         "/html/?q=",
                         NULL,
                         0,
                         false,
                         {NULL,
                          {"result-link", "result__a"},
                          NULL,
                          {"result-snippet", "result__snippet"},
                          false},
                         {0}},
    [ENGINE_BRAVE] = {"brave",
                      "https://search.brave.com",
                      "/search?q=",
                      NULL,
                      0,
                      false,
                      {"snippet",
                       {NULL, NULL},
                       "search-snippet-title",
                       {"generic-snippet", "snippet-description"},
                       false},
                      {0}},
    [ENGINE_BRAVE_API] = {"brave_api",
                          "https://api.search.brave.com",
                          "/res/v1/web/search?count=10&q=",
                          "X-Subscription-Token: %k",
                          NEED_KEY,
                          true,
                          {0},
                          {"web", "results", "title", "url", "description"}},
    [ENGINE_GOOGLE] = {"google",
                       "https://www.googleapis.com",
                       "/customsearch/v1?key=%k&cx=%c&num=10&q=",
                       NULL,
                       NEED_KEY | NEED_ENGINE_ID,
                       true,
                       {0},
                       {NULL, "items", "title", "link", "snippet"}},
    [ENGINE_SEARXNG] = {"searxng",
                        "",
                        "/search?format=json&language=en&q=",
                        NULL,
                        NEED_ENDPOINT,
                        true,
                        {0},
                        {NULL, "results", "title", "url", "content"}},
};

static struct {
    SearchEngine chain[ENGINE_N];
    size_t chain_n;
    Str endpoint, api_key, engine_id;
    f64 started[ENGINE_N];
    f64 paused[ENGINE_N];
} g_search = {{ENGINE_DDG_LITE, ENGINE_DDG_HTML, ENGINE_BRAVE, 0, 0, 0},
              3,
              {0},
              {0},
              {0},
              {0},
              {0}};

static b8 search_admit(SearchEngine e, const char *label, char *err,
                       size_t err_cap) {
    f64 now = agent_now_seconds();
    if (g_search.paused[e] > now) {
        i64 seconds = (i64)(g_search.paused[e] - now) + 1;
        snprintf(err, err_cap,
                 "the %s search endpoint is paused for %llds "
                 "after a challenge or refusal",
                 label, (long long)seconds);
        return false;
    }
    if (g_search.started[e] > 0) {
        f64 next = g_search.started[e] + (f64)search_interval_ms() / 1000.0;
        if (!search_wait_until(next)) {
            snprintf(err, err_cap, "interrupted while pacing searches");
            return false;
        }
    }
    g_search.started[e] = agent_now_seconds();
    return true;
}

static void search_pause(SearchEngine e) {
    f64 until = agent_now_seconds() + (f64)AGENT_WEB_SEARCH_PAUSE_MS / 1000.0;
    if (until > g_search.paused[e]) g_search.paused[e] = until;
}


static size_t search_chain_for(Str name, SearchEngine out[ENGINE_N]) {
    if (str_eq(name, STR("ddg"))) {
        out[0] = ENGINE_DDG_LITE;
        out[1] = ENGINE_DDG_HTML;
        return 2;
    }
    for (SearchEngine e = 0; e < ENGINE_N; e++) {
        if (!str_eq(name, str_c(k_engine[e].label))) continue;
        out[0] = e;
        return 1;
    }
    out[0] = ENGINE_DDG_LITE;
    out[1] = ENGINE_DDG_HTML;
    out[2] = ENGINE_BRAVE;
    return 3;
}

#ifdef AGENT_TESTING

static const char *search_test_prefix(size_t slot) {
    static const char *const env[] = {
        AGENT_ENV_PREFIX "TEST_WEB_SEARCH_URL",
        AGENT_ENV_PREFIX "TEST_WEB_SEARCH_FALLBACK_URL",
        AGENT_ENV_PREFIX "TEST_WEB_SEARCH_BRAVE_URL",
    };
    if (slot >= sizeof env / sizeof env[0]) return NULL;
    const char *value = getenv(env[slot]);
    return value && *value ? value : NULL;
}
#endif


static Str search_setting(Arena *persist, Str value) {
    return str_dup_opt(persist, str_trim(value));
}

void web_search_init(const Conf *c, Arena *persist) {
    g_search.chain_n =
        search_chain_for(conf_str(c, CONF_SEARCH_BACKEND), g_search.chain);
    g_search.endpoint =
        search_setting(persist, conf_str(c, CONF_SEARCH_ENDPOINT));
    g_search.api_key =
        search_setting(persist, conf_str(c, CONF_SEARCH_API_KEY));
    g_search.engine_id =
        search_setting(persist, conf_str(c, CONF_SEARCH_ENGINE_ID));
    if (g_search.chain_n == 1) {
        const SearchEngineSpec *spec = &k_engine[g_search.chain[0]];
        const char *missing = NULL;
        if ((spec->needs & NEED_KEY) && !g_search.api_key.n)
            missing = "search_api_key";
        else if ((spec->needs & NEED_ENGINE_ID) && !g_search.engine_id.n)
            missing = "search_engine_id";
        else if ((spec->needs & NEED_ENDPOINT) && !g_search.endpoint.n)
            missing = "search_endpoint";
        if (missing) {
            agent_log(AGENT_LOG_WARN,
                      "ignoring search_backend %s: it needs %s; "
                      "searching the keyless engines instead",
                      spec->label, missing);
            g_search.chain_n = search_chain_for(STR("auto"), g_search.chain);
        }
    } else if (g_search.endpoint.n) {
        agent_log(AGENT_LOG_WARN, "ignoring search_endpoint: it belongs to one "
                                  "engine, so search_backend must name one");
        g_search.endpoint = (Str){0};
    }
#ifdef AGENT_TESTING
    if (search_test_prefix(0)) {
        size_t n = 0;
        while (n < g_search.chain_n && search_test_prefix(n)) n++;
        g_search.chain_n = n;
    }
#endif
}


static b8 search_url(const SearchEngineSpec *spec, size_t slot,
                     const char *query, Arena *scratch, Str *out, char *err,
                     size_t err_cap) {
    Buf url;
    buf_init(&url, scratch, 2048);
    const char *prefix = NULL;
#ifdef AGENT_TESTING
    prefix = search_test_prefix(slot);
#else
    (void)slot;
#endif
    if (prefix) {
        buf_puts(&url, str_c(prefix));
    } else {
        Str base = g_search.endpoint.n ? g_search.endpoint : str_c(spec->base);
        while (base.n && base.p[base.n - 1] == '/') base.n--;
        buf_puts(&url, base);
        for (const char *p = spec->path; *p; p++) {
            if (*p != '%' || (p[1] != 'k' && p[1] != 'c')) {
                buf_putc(&url, *p);
                continue;
            }
            encode_query(p[1] == 'k' ? g_search.api_key : g_search.engine_id,
                         &url);
            p++;
        }
    }
    if (!encode_query(str_c(query), &url)) {
        snprintf(err, err_cap, "search URL does not fit in memory");
        return false;
    }
    *out = buf_finish(&url);
    if (!buf_ok(&url) || out->n >= AGENT_WEB_URL_BYTES) {
        snprintf(err, err_cap, "encoded search URL is too long");
        return false;
    }
    return true;
}

static const char *search_header(const SearchEngineSpec *spec, Arena *scratch) {
    if (!spec->header) return NULL;
    Buf h;
    buf_init(&h, scratch, 256);
    for (const char *p = spec->header; *p; p++) {
        if (*p == '%' && p[1] == 'k') {
            buf_puts(&h, g_search.api_key);
            p++;
        } else
            buf_putc(&h, *p);
    }
    Str s = buf_finish(&h);
    return buf_ok(&h) ? s.p : NULL;
}

typedef enum {
    SEARCH_OK,
    SEARCH_BLOCKED,
    SEARCH_UNKNOWN,
    SEARCH_ERROR,
} SearchOutcome;


static SearchOutcome search_json(Str raw, const SearchEngineSpec *spec,
                                 Arena *scratch, SearchCtx *found, char *err,
                                 size_t err_cap) {
    JVal *root = json_parse(scratch, raw);
    if (!root) {
        snprintf(err, err_cap, "the %s search response was not JSON",
                 spec->label);
        return SEARCH_UNKNOWN;
    }
    const JVal *holder = root;
    if (spec->shape.object) holder = json_get(root, str_c(spec->shape.object));
    const JVal *arr =
        holder ? json_get(holder, str_c(spec->shape.array)) : NULL;
    if (!arr || arr->type != J_ARR) {
        snprintf(err, err_cap,
                 "the %s search response carried no results "
                 "array; the service may have changed",
                 spec->label);
        return SEARCH_UNKNOWN;
    }
    for (size_t i = 0; i < arr->u.arr.n && found->n < 10; i++) {
        const JVal *item = json_at(arr, i);
        if (!item || item->type != J_OBJ) continue;
        Str url = json_str(item, str_c(spec->shape.url));
        Str title = json_str(item, str_c(spec->shape.title));
        if (!url.n || !title.n || url.n >= AGENT_WEB_URL_BYTES) continue;
        url = str_dup(scratch, url);
        if (!url.p || !http_url_ok(url.p)) continue;
        found->layout_links++;
        b8 seen = false;
        for (size_t k = 0; k < found->n; k++)
            if (str_eq(found->result[k].url, url)) seen = true;
        if (seen) continue;
        found->result[found->n++] = (SearchResult){
            title, url, json_str(item, str_c(spec->shape.snippet))};
    }
    found->explicit_empty = arr->u.arr.n == 0;
    return SEARCH_OK;
}

static SearchOutcome search_backend_run(SearchEngine engine, size_t slot,
                                        const char *query, Arena *scratch,
                                        SearchCtx *found, char *err,
                                        size_t err_cap) {
    const SearchEngineSpec *spec = &k_engine[engine];
    Str request_url;
    if (!search_url(spec, slot, query, scratch, &request_url, err, err_cap))
        return SEARCH_ERROR;
    if (!search_admit(engine, spec->label, err, err_cap)) return SEARCH_BLOCKED;
    Buf source;
    buf_init(&source, scratch, 65536);
    HttpUrlReq req;
    i32 rc = web_request(request_url.p, "internet_search", &source,
                         search_header(spec, scratch), &req);
    if (req.status == 202 || req.status == 403 || req.status == 429) {
        snprintf(err, err_cap,
                 "the %s search endpoint refused the request with HTTP %lld",
                 spec->label, (long long)req.status);
        search_pause(engine);
        return SEARCH_BLOCKED;
    }
    if (rc != 0) {
        if (rc < 0)
            snprintf(err, err_cap, "the %s search endpoint returned HTTP %lld",
                     spec->label, (long long)-rc);
        else if ((spec->needs & NEED_KEY) || !req.failure[0])
            snprintf(err, err_cap, "the %s search request failed", spec->label);
        else
            snprintf(err, err_cap, "%s", req.failure);
        return SEARCH_UNKNOWN;
    }
    Str raw = buf_finish(&source);
    if (!buf_ok(&source)) {
        snprintf(err, err_cap, "search response does not fit in memory");
        return SEARCH_ERROR;
    }
    *found = (SearchCtx){
        .scratch = scratch, .layout = &spec->layout, .current = SIZE_MAX};
    if (spec->json) return search_json(raw, spec, scratch, found, err, err_cap);
    AgentHtmlDoc *doc = agent_html_parse(raw.p, raw.n);
    if (!doc) {
        snprintf(err, err_cap, "search HTML parsing failed");
        return SEARCH_ERROR;
    }
    html_walk(agent_html_root(doc), search_enter, search_leave, found);
    agent_html_destroy(doc);
    search_compact(found);
    if (found->challenge || contains_ci(raw, "verify you are human")) {
        snprintf(err, err_cap,
                 "the %s search endpoint returned a challenge page",
                 spec->label);
        search_pause(engine);
        return SEARCH_BLOCKED;
    }
    if (found->oom) {
        snprintf(err, err_cap, "search result text is too large");
        return SEARCH_ERROR;
    }
    if (!found->layout_links && !found->explicit_empty) {
        snprintf(err, err_cap,
                 "the %s search result layout was not recognized; the service "
                 "may have changed",
                 spec->label);
        return SEARCH_UNKNOWN;
    }
    return SEARCH_OK;
}

b8 internet_search_run(Str args, Arena *scratch, Buf *out, char *err,
                       size_t err_cap) {
    JVal *j = web_args(args, scratch, err, err_cap);
    if (!j) return false;
    char query[AGENT_WEB_QUERY_BYTES];
    if (!web_arg_cstr(json_str(j, STR("query")), query, sizeof query, "query",
                      err, err_cap))
        return false;
    size_t limit;
    if (!web_arg_count(j, STR("limit"), 8, 10, &limit, err, err_cap))
        return false;

    SearchCtx found = {.scratch = scratch, .current = SIZE_MAX};
    char attempts[512] = {0};
    size_t attempts_n = 0;
    b8 blocked = false;
    b8 answered = false;
    for (size_t i = 0; i < g_search.chain_n && !answered; i++) {
        SearchCtx attempt = {.scratch = scratch, .current = SIZE_MAX};
        SearchOutcome outcome = search_backend_run(
            g_search.chain[i], i, query, scratch, &attempt, err, err_cap);
        if (outcome == SEARCH_ERROR) return false;
        if (outcome != SEARCH_OK) {
            if (outcome == SEARCH_BLOCKED) blocked = true;
            search_attempts_add(attempts, sizeof attempts, &attempts_n, err);
            continue;
        }
        found = attempt;
        answered = true;
    }
    if (!answered) {
        if (!attempts_n)
            search_attempts_add(attempts, sizeof attempts, &attempts_n,
                                "no search endpoint is configured");
        if (blocked) {
            snprintf(err, err_cap,
                     "%s; each refusing endpoint is paused for "
                     "one hour; do not retry",
                     attempts);
        } else {
            snprintf(err, err_cap, "%s", attempts);
        }
        return false;
    }

    if (found.n > limit) found.n = limit;
    Str records[10];

    static Spill spill;
    spill_open(&spill, "internet_search", "txt", str_c(query));
    size_t emitted = 0, bytes = 0;
    b8 room = true;
    for (size_t i = 0; i < found.n; i++) {
        Buf record;
        buf_init(&record, scratch, 512);
        buf_putf(&record, "%zu. ", i + 1);
        buf_puts(&record, found.result[i].title);
        buf_puts(&record, STR("\n   "));
        buf_puts(&record, found.result[i].url);
        if (found.result[i].snippet.n) {
            buf_puts(&record, STR("\n   "));
            buf_puts(&record, found.result[i].snippet);
        }
        buf_puts(&record, STR("\n\n"));
        records[i] = buf_finish(&record);
        if (!buf_ok(&record)) {
            spill_finish(&spill, out, false);
            snprintf(err, err_cap, "search result does not fit in memory");
            return false;
        }
        spill_put(&spill, records[i].p, records[i].n);
        size_t reserve = 64 + (spill.fd >= 0 ? AGENT_SPILL_NOTE_BYTES : 0);
        if (!room || bytes + records[i].n + reserve > AGENT_TOOL_RESULT_BYTES) {
            room = false;
            continue;
        }
        bytes += records[i].n;
        emitted++;
    }
    buf_putf(out, "External search results (untrusted): %zu\n", emitted);
    for (size_t i = 0; i < emitted; i++) buf_puts(out, records[i]);
    while (out->n && out->p[out->n - 1] == '\n') out->n--;
    spill_finish(&spill, out, emitted < found.n);
    if (!buf_ok(out) || out->n > AGENT_TOOL_RESULT_BYTES) {
        snprintf(err, err_cap, "search result exceeds the %u byte limit",
                 (unsigned)AGENT_TOOL_RESULT_BYTES);
        return false;
    }
    return true;
}
