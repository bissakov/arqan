#include "agent.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#if AGENT_CURL_DLOPEN
#include <dlfcn.h>

CurlApi g_curl;


#define AGENT_CURL_SONAME "libcurl.so.4"

b8 curl_load(char *err, size_t err_cap) {
    static b8 loaded, tried;
    static char why[256];
    if (!tried) {
        tried = true;
        void *lib = dlopen(AGENT_CURL_SONAME, RTLD_LAZY | RTLD_LOCAL);
        if (!lib) {
            const char *e = dlerror();
            snprintf(why, sizeof why, "cannot load " AGENT_CURL_SONAME ": %s",
                     e ? e : "unknown error");
        } else {
            struct {
                void **slot;
                const char *name;
            } wanted[] = {
                {(void **)&g_curl.easy_init, "curl_easy_init"},
                {(void **)&g_curl.easy_setopt, "curl_easy_setopt"},
                {(void **)&g_curl.easy_getinfo, "curl_easy_getinfo"},
                {(void **)&g_curl.easy_perform, "curl_easy_perform"},
                {(void **)&g_curl.easy_cleanup, "curl_easy_cleanup"},
                {(void **)&g_curl.easy_strerror, "curl_easy_strerror"},
                {(void **)&g_curl.slist_append, "curl_slist_append"},
                {(void **)&g_curl.slist_free_all, "curl_slist_free_all"},
                {(void **)&g_curl.url, "curl_url"},
                {(void **)&g_curl.url_set, "curl_url_set"},
                {(void **)&g_curl.url_get, "curl_url_get"},
                {(void **)&g_curl.url_cleanup, "curl_url_cleanup"},
                {(void **)&g_curl.free, "curl_free"},
                {(void **)&g_curl.multi_init, "curl_multi_init"},
                {(void **)&g_curl.multi_add_handle, "curl_multi_add_handle"},
                {(void **)&g_curl.multi_remove_handle,
                 "curl_multi_remove_handle"},
                {(void **)&g_curl.multi_perform, "curl_multi_perform"},
                {(void **)&g_curl.multi_poll, "curl_multi_poll"},
                {(void **)&g_curl.multi_info_read, "curl_multi_info_read"},
                {(void **)&g_curl.multi_cleanup, "curl_multi_cleanup"},
                {(void **)&g_curl.multi_strerror, "curl_multi_strerror"},
            };
            loaded = true;
            for (size_t i = 0; i < sizeof wanted / sizeof *wanted; i++) {
                *wanted[i].slot = dlsym(lib, wanted[i].name);
                if (*wanted[i].slot) continue;
                snprintf(why, sizeof why, AGENT_CURL_SONAME " has no %s",
                         wanted[i].name);
                loaded = false;
                break;
            }
            if (!loaded) {
                CurlApi empty = {0};
                g_curl = empty;
                dlclose(lib);
            }
        }
    }
    if (!loaded && err && err_cap) snprintf(err, err_cap, "%s", why);
    return loaded;
}
#endif

typedef struct {
    const HttpReq *r;
    Buf line;
    b8 aborted;
    b8 oom;

    size_t lines;
    size_t polls;
    f64 last_write;
    f64 stall;
} Ctx;


static b8 dispatch_line(Ctx *c, const char *p, size_t n) {
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != '\n') continue;
        buf_put(&c->line, p + start, i - start);
        start = i + 1;
        if (!buf_ok(&c->line)) {
            c->oom = true;
            return false;
        }
        size_t len = c->line.n;
        if (len && c->line.p[len - 1] == '\r') len--;
        Str ln = {c->line.p, len};
        c->line.n = 0;
        c->lines++;
        if (c->r->on_line && !c->r->on_line(ln, c->r->ud)) {
            c->aborted = true;
            return false;
        }
    }
    buf_put(&c->line, p + start, n - start);
    if (!buf_ok(&c->line)) {
        c->oom = true;
        return false;
    }
    return true;
}

static size_t write_cb(char *p, size_t sz, size_t n, void *ud) {
    Ctx *c = (Ctx *)ud;
    size_t total = sz * n;
    f64 now = agent_now_seconds();
    if (c->last_write > 0 && now - c->last_write > c->stall)
        c->stall = now - c->last_write;
    c->last_write = now;
    b8 consumed = dispatch_line(c, p, total);

    if (c->aborted) return total;
    return consumed ? total : 0;
}

static size_t body_cb(char *p, size_t sz, size_t n, void *ud) {
    Buf *b = (Buf *)ud;
    size_t total = sz * n;
    buf_put(b, p, total);
    return buf_ok(b) ? total : 0;
}

typedef struct {
    Buf *out;
    size_t limit;
    b8 too_large;
    b8 blocked;
} UrlCtx;


#define HTTP_POLL_MS 100

static size_t url_body_cb(char *p, size_t sz, size_t n, void *ud) {
    UrlCtx *ctx = (UrlCtx *)ud;
    if (sz && n > SIZE_MAX / sz) return 0;
    size_t total = sz * n;
    if (ctx->out->n > ctx->limit || total > ctx->limit - ctx->out->n) {
        ctx->too_large = true;
        return 0;
    }
    buf_put(ctx->out, p, total);
    return buf_ok(ctx->out) ? total : 0;
}

static b8 ipv4_public(const struct in_addr *in) {
    const u8 *p = (const u8 *)&in->s_addr;
    if (p[0] == 0 || p[0] == 10 || p[0] == 127 || p[0] >= 224) return false;
    if (p[0] == 100 && (p[1] & 0xc0u) == 64) return false;
    if (p[0] == 169 && p[1] == 254) return false;
    if (p[0] == 172 && (p[1] & 0xf0u) == 16) return false;
    if (p[0] == 192 && p[1] == 0 && (p[2] == 0 || p[2] == 2)) return false;
    if (p[0] == 192 && p[1] == 168) return false;
    if (p[0] == 192 && p[1] == 88 && p[2] == 99) return false;
    if (p[0] == 198 && (p[1] == 18 || p[1] == 19)) return false;
    if (p[0] == 198 && p[1] == 51 && p[2] == 100) return false;
    if (p[0] == 203 && p[1] == 0 && p[2] == 113) return false;
    return true;
}

static b8 ipv6_public(const struct in6_addr *in) {
    const u8 *p = in->s6_addr;
    static const u8 mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (!memcmp(p, mapped, sizeof mapped)) {
        struct in_addr v4;
        memcpy(&v4.s_addr, p + 12, 4);
        return ipv4_public(&v4);
    }

    if ((p[0] & 0xe0u) != 0x20u) return false;
    if (p[0] == 0x20 && p[1] == 0x01) {
        if (p[2] < 0x02) return false;
        if (p[2] == 0x02 && p[3] == 0x00) return false;
        if (p[2] == 0x0d && p[3] == 0xb8) return false;
    }
    if (p[0] == 0x20 && p[1] == 0x02) return false;
    if (p[0] == 0x3f && p[1] == 0xff) return false;
    return true;
}

static curl_socket_t public_open_cb(void *ud, curlsocktype purpose,
                                    struct curl_sockaddr *address) {
    UrlCtx *ctx = (UrlCtx *)ud;
    (void)purpose;
    b8 allowed = false;
    if (address->family == AF_INET
        && address->addrlen >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *sa =
            (const struct sockaddr_in *)&address->addr;
        allowed = ipv4_public(&sa->sin_addr);
    } else if (address->family == AF_INET6
               && address->addrlen >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *sa =
            (const struct sockaddr_in6 *)&address->addr;
        allowed = ipv6_public(&sa->sin6_addr);
    }
    if (!allowed) {
        ctx->blocked = true;
        return CURL_SOCKET_BAD;
    }
    return socket(address->family, address->socktype, address->protocol);
}

static size_t drop_header_cb(char *p, size_t sz, size_t n, void *ud) {
    (void)p;
    (void)ud;
    return sz * n;
}

static b8 build_url(char *url, size_t cap, const char *base_url,
                    const char *path) {
    size_t base_n = base_url ? strlen(base_url) : 0;
    size_t path_n = strlen(path);
    if (base_n == 0 || base_n + path_n + 1 > cap) return false;
    memcpy(url, base_url, base_n);
    memcpy(url + base_n, path, path_n + 1);
    return true;
}


#define ANTHROPIC_VERSION "anthropic-version: 2023-06-01"

static struct curl_slist *auth_header(struct curl_slist *hdrs,
                                      const char *api_key, ApiKind api) {
    if (api == API_ANTHROPIC) hdrs = curl_slist_append(hdrs, ANTHROPIC_VERSION);
    if (!api_key || !*api_key) return hdrs;
    char auth[1024];
    const char *fmt =
        api == API_ANTHROPIC ? "x-api-key: %s" : "Authorization: Bearer %s";
    i32 an = snprintf(auth, sizeof auth, fmt, api_key);
    if (an > 0 && (size_t)an < sizeof auth)
        return curl_slist_append(hdrs, auth);
    agent_log(AGENT_LOG_WARN, "api key too long; sending no key header");
    return hdrs;
}

static const char *api_post_path(ApiKind api) {
    return api == API_ANTHROPIC ? "/messages" : "/chat/completions";
}

/* ---- TLS trust store -----------------------------------------------------
 * libcurl's CA location is chosen when libcurl is built, so it names the
 * machine that compiled it rather than the one running this binary. That
 * holds wherever the two differ: a relocated build, a container, and every
 * statically linked build. Resolve the store once per process and say so
 * explicitly, but only when libcurl's own answer would not work: an unset
 * option is how a working default keeps its behaviour. */

typedef struct {
    const char *file;
    const char *dir;
} CaTrust;

static b8 ca_present(const char *path, b8 want_dir) {
    struct stat st;
    if (!path || !*path || stat(path, &st) != 0) return false;
    return want_dir ? S_ISDIR(st.st_mode) != 0 : S_ISREG(st.st_mode) != 0;
}


static const char *ca_root(void) {
#ifdef AGENT_TESTING
    const char *root = getenv(AGENT_ENV_PREFIX "TEST_CA_ROOT");
    if (root && *root) return root;
#endif
    return "";
}


static const char *const k_ca_files[] = {
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/ca-bundle.pem",
    "/etc/pki/tls/cacert.pem",
    "/etc/ssl/cert.pem",
    "/usr/local/share/certs/ca-root-nss.crt",
    "/etc/certs/ca-certificates.crt",
};
static const char *const k_ca_dirs[] = {
    "/etc/ssl/certs",
    "/etc/pki/tls/certs",
};

static b8 ca_candidate(const char *path, char *buf, size_t cap,
                       const char **out) {
    const char *root = ca_root();
    if (!*root) {
        *out = path;
        return true;
    }
    i32 n = snprintf(buf, cap, "%s%s", root, path);
    if (n < 0 || (size_t)n >= cap) return false;
    *out = buf;
    return true;
}


static b8 ca_default_works(void) {
#if LIBCURL_VERSION_NUM >= 0x075400
    CURL *probe = curl_easy_init();
    if (!probe) return true;
    char *file = NULL, *dir = NULL;
    if (curl_easy_getinfo(probe, CURLINFO_CAINFO, &file) != CURLE_OK)
        file = NULL;
    if (curl_easy_getinfo(probe, CURLINFO_CAPATH, &dir) != CURLE_OK) dir = NULL;
    b8 has_file = file && *file, has_dir = dir && *dir;
    b8 ok = (has_file || has_dir) && (!has_file || ca_present(file, false))
            && (!has_dir || ca_present(dir, true));
    curl_easy_cleanup(probe);
    return ok;
#else
    return false;
#endif
}

static const CaTrust *ca_trust(void) {
    static CaTrust trust;
    static b8 resolved;
    static char file_buf[AGENT_MAX_PATH];
    static char dir_buf[AGENT_MAX_PATH];
    if (resolved) return &trust;
    resolved = true;

    const char *env = getenv("CURL_CA_BUNDLE");
    if (!env || !*env) env = getenv("SSL_CERT_FILE");
    const char *env_dir = getenv("SSL_CERT_DIR");
    if (env && *env) trust.file = env;
    if (env_dir && *env_dir) trust.dir = env_dir;
    if (trust.file || trust.dir) {
        agent_log(AGENT_LOG_DEBUG, "tls: trust store from the environment");
        return &trust;
    }

    if (!*ca_root() && ca_default_works()) return &trust;

    for (size_t i = 0; i < sizeof k_ca_files / sizeof k_ca_files[0]; i++) {
        const char *path = NULL;
        if (!ca_candidate(k_ca_files[i], file_buf, sizeof file_buf, &path))
            continue;
        if (!ca_present(path, false)) continue;
        trust.file = path;
        return &trust;
    }
    for (size_t i = 0; i < sizeof k_ca_dirs / sizeof k_ca_dirs[0]; i++) {
        const char *path = NULL;
        if (!ca_candidate(k_ca_dirs[i], dir_buf, sizeof dir_buf, &path))
            continue;
        if (!ca_present(path, true)) continue;
        trust.dir = path;
        return &trust;
    }

    agent_log(AGENT_LOG_WARN,
              "tls: no CA trust store found; HTTPS verification may fail");
    return &trust;
}


static void http_apply_ca(CURL *curl) {
    const CaTrust *t = ca_trust();
    if (!t->file && !t->dir) return;
    curl_easy_setopt(curl, CURLOPT_CAINFO, t->file);
    curl_easy_setopt(curl, CURLOPT_CAPATH, t->dir);
}

#ifdef AGENT_TESTING
void http_print_ca_trust(void) {
    char err[256] = {0};
    if (!curl_load(err, sizeof err)) {
        printf("ca-load: %s\n", err);
        return;
    }
    const CaTrust *t = ca_trust();
    printf("ca-file: %s\n", t->file ? t->file : "-");
    printf("ca-dir: %s\n", t->dir ? t->dir : "-");
    printf("ca-defaults: %s\n", (t->file || t->dir) ? "cleared" : "kept");
}
#endif

static Str url_host(const char *url) {
    Str s = str_c(url ? url : "");
    const char *sep = strstr(s.p, "://");
    if (sep) s = str_drop(s, (size_t)(sep - s.p) + 3);
    if (s.n && s.p[0] == '[') {
        const char *end = (const char *)memchr(s.p + 1, ']', s.n - 1);
        if (end) return (Str){s.p + 1, (size_t)(end - s.p) - 1};
    }
    size_t n = 0;
    while (n < s.n && s.p[n] != '/' && s.p[n] != ':' && s.p[n] != '?'
           && s.p[n] != '#')
        n++;
    return (Str){s.p, n};
}

static b8 host_is_loopback(Str host) {
    return str_eq(host, STR("localhost")) || str_eq(host, STR("::1"))
           || str_eq(host, STR("[::1]")) || str_starts(host, STR("127."));
}


static i64 curl_ms(CURL *curl, CURLINFO info) {
    curl_off_t us = 0;
    if (curl_easy_getinfo(curl, info, &us) != CURLE_OK || us < 0) return -1;
    return (i64)(us / 1000);
}

static void http_record(const char *method, const char *path, const char *url,
                        CURL *curl, CURLcode rc, i64 status, const Ctx *sse,
                        b8 interrupted) {
    TelEvent e;
    tel_open(&e, "http");
    tel_str(&e, "method", str_c(method));
    tel_str(&e, "path", str_c(path));
    Str host = url_host(url);
    tel_hash_field(&e, "host", host);
    tel_bool(&e, "loopback", host_is_loopback(host));
    tel_bool(&e, "tls", str_starts(str_c(url ? url : ""), STR("https://")));
    tel_int(&e, "status", status);
    tel_int(&e, "curl", (i64)rc);

    if (rc != CURLE_OK)
        tel_str(&e, "curl_error", str_c(curl_easy_strerror(rc)));
    tel_bool(&e, "interrupted", interrupted);

    tel_int(&e, "dns_ms", curl_ms(curl, CURLINFO_NAMELOOKUP_TIME_T));
    tel_int(&e, "connect_ms", curl_ms(curl, CURLINFO_CONNECT_TIME_T));
    tel_int(&e, "tls_ms", curl_ms(curl, CURLINFO_APPCONNECT_TIME_T));
    tel_int(&e, "ttfb_ms", curl_ms(curl, CURLINFO_STARTTRANSFER_TIME_T));
    tel_int(&e, "total_ms", curl_ms(curl, CURLINFO_TOTAL_TIME_T));

    curl_off_t up = 0, down = 0;
    curl_easy_getinfo(curl, CURLINFO_SIZE_UPLOAD_T, &up);
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &down);
    tel_int(&e, "up_bytes", (i64)up);
    tel_int(&e, "down_bytes", (i64)down);

    long version = 0, redirects = 0;
    curl_easy_getinfo(curl, CURLINFO_HTTP_VERSION, &version);
    curl_easy_getinfo(curl, CURLINFO_REDIRECT_COUNT, &redirects);
    tel_int(&e, "http_version", (i64)version);
    tel_int(&e, "redirects", (i64)redirects);

    char *ip = NULL;
    if (curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ip) == CURLE_OK && ip)
        tel_str(&e, "ip", strchr(ip, ':') ? STR("v6") : STR("v4"));

    if (sse) {
        tel_int(&e, "sse_lines", (i64)sse->lines);
        tel_int(&e, "polls", (i64)sse->polls);
        tel_int(&e, "stall_ms", (i64)(sse->stall * 1000.0));
        tel_bool(&e, "aborted", sse->aborted);
        tel_bool(&e, "line_oom", sse->oom);
    }
    tel_send(&e);
}

i32 http_get(const char *base_url, const char *path, const char *api_key,
             ApiKind api, Buf *out, char *fail_out, size_t fail_cap) {
    if (fail_out && fail_cap) fail_out[0] = 0;
    if (!curl_load(fail_out, fail_cap)) return 1;
    char url[2048];
    if (!build_url(url, sizeof url, base_url, path)) {
        if (fail_out && fail_cap)
            snprintf(fail_out, fail_cap, "base URL is empty or too long");
        return 1;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (fail_out && fail_cap)
            snprintf(fail_out, fail_cap, "curl init failed");
        return 1;
    }

    struct curl_slist *hdrs =
        curl_slist_append(NULL, "Accept: application/json");
    hdrs = auth_header(hdrs, api_key, api);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, drop_header_cb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    http_apply_ca(curl);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    http_record("GET", path, url, curl, rc, (i64)http_code, NULL, false);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        agent_log(AGENT_LOG_DEBUG, "curl: %s", curl_easy_strerror(rc));
        if (fail_out && fail_cap)
            snprintf(fail_out, fail_cap, "%s", curl_easy_strerror(rc));
        return 2;
    }
    if (http_code < 200 || http_code >= 300) return -(i32)http_code;
    return 0;
}

static b8 http_url_input_ok(const char *url, char *err, size_t err_cap) {
    if (!url || !*url) {
        snprintf(err, err_cap, "URL is empty");
        return false;
    }
    if (strlen(url) >= AGENT_WEB_URL_BYTES) {
        snprintf(err, err_cap, "URL is too long");
        return false;
    }
    CURLU *parts = curl_url();
    if (!parts) {
        snprintf(err, err_cap, "curl URL parser initialization failed");
        return false;
    }
    CURLUcode uc = curl_url_set(parts, CURLUPART_URL, url, 0);
    char *scheme = NULL, *host = NULL, *user = NULL, *password = NULL;
    if (uc == CURLUE_OK) uc = curl_url_get(parts, CURLUPART_SCHEME, &scheme, 0);
    if (uc == CURLUE_OK) uc = curl_url_get(parts, CURLUPART_HOST, &host, 0);
    CURLUcode user_rc = curl_url_get(parts, CURLUPART_USER, &user, 0);
    CURLUcode pass_rc = curl_url_get(parts, CURLUPART_PASSWORD, &password, 0);
    b8 credentials = (user_rc == CURLUE_OK && user && *user)
                     || (pass_rc == CURLUE_OK && password && *password);
    b8 scheme_ok =
        uc == CURLUE_OK && scheme
        && (!strcasecmp(scheme, "http") || !strcasecmp(scheme, "https"));
    b8 ok = uc == CURLUE_OK && scheme_ok && host && *host && !credentials;
    if (!ok) {
        if (credentials)
            snprintf(err, err_cap, "URL credentials are not allowed");
        else if (uc == CURLUE_OK && !scheme_ok)
            snprintf(err, err_cap, "URL scheme must be http or https");
        else
            snprintf(err, err_cap, "malformed URL");
    }
    curl_free(scheme);
    curl_free(host);
    curl_free(user);
    curl_free(password);
    curl_url_cleanup(parts);
    return ok;
}

i32 http_url_get(HttpUrlReq *r) {
    if (!r || !r->out) return 1;
    r->effective_url[0] = '\0';
    r->content_type[0] = '\0';
    r->failure[0] = '\0';
    r->status = 0;
    if (!curl_load(r->failure, sizeof r->failure)) return 1;
    if (!http_url_input_ok(r->url, r->failure, sizeof r->failure)) return 2;

    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(r->failure, sizeof r->failure, "curl init failed");
        return 1;
    }
    CURLM *multi = curl_multi_init();
    if (!multi) {
        curl_easy_cleanup(curl);
        snprintf(r->failure, sizeof r->failure, "curl multi init failed");
        return 1;
    }

    UrlCtx ctx = {r->out, r->max_bytes, false, false};
    char curl_err[CURL_ERROR_SIZE] = {0};
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(
        hdrs, "Accept: text/html, application/xhtml+xml, "
              "text/plain, application/json, application/xml;q=0.9, "
              "text/*;q=0.8, */*;q=0.1");
    hdrs = curl_slist_append(hdrs, "Accept-Language: en-US,en;q=0.9");
    for (size_t i = 0; i < sizeof r->header / sizeof r->header[0]; i++)
        if (r->header[i]) hdrs = curl_slist_append(hdrs, r->header[i]);
    curl_easy_setopt(curl, CURLOPT_URL, r->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, AGENT_WEB_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, url_body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, drop_header_cb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)r->max_redirects);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                     CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
#if LIBCURL_VERSION_NUM >= 0x073d00
    curl_easy_setopt(curl, CURLOPT_DISALLOW_USERNAME_IN_URL, 1L);
#endif
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_NONE);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 0L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     (long)r->connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)r->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_err);
    http_apply_ca(curl);
    if (r->public_only) {
        curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, public_open_cb);
        curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, &ctx);
    }

    CURLMcode mc = curl_multi_add_handle(multi, curl);
    CURLcode rc = CURLE_OK;
    b8 interrupted = false;
    i32 running = 1;
    while (mc == CURLM_OK && running) {
        mc = curl_multi_perform(multi, &running);
        if (mc == CURLM_OK && running) {
            struct curl_waitfd extra = {r->idle_fd, CURL_WAIT_POLLIN, 0};
            b8 watch = r->idle_fd >= 0;
            i32 numfds = 0;
            mc = curl_multi_poll(multi, watch ? &extra : NULL, watch ? 1u : 0u,
                                 HTTP_POLL_MS, &numfds);
        }
        if (r->on_idle) r->on_idle(r->idle_ud);
        if (r->interrupt_flag && *r->interrupt_flag) {
            interrupted = true;
            break;
        }
    }
    if (mc != CURLM_OK) rc = CURLE_RECV_ERROR;
    if (!interrupted && rc == CURLE_OK) {
        CURLMsg *msg;
        i32 left = 0;
        while ((msg = curl_multi_info_read(multi, &left)))
            if (msg->msg == CURLMSG_DONE) rc = msg->data.result;
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    r->status = (i64)status;
    char *effective = NULL, *type = NULL;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &type);
    if (effective) {
        size_t n = strlen(effective);
        if (n < sizeof r->effective_url)
            memcpy(r->effective_url, effective, n + 1);
        else if (rc == CURLE_OK)
            rc = CURLE_URL_MALFORMAT;
    }
    if (type) {
        size_t n = strlen(type);
        if (n >= sizeof r->content_type) n = sizeof r->content_type - 1;
        memcpy(r->content_type, type, n);
        r->content_type[n] = '\0';
    } else {
        snprintf(r->content_type, sizeof r->content_type, "(none)");
    }
    http_record("GET", r->operation ? r->operation : "web", r->url, curl, rc,
                r->status, NULL, interrupted);

    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (interrupted) {
        snprintf(r->failure, sizeof r->failure, "interrupted");
        r->out->n = 0;
        return 3;
    }
    if (ctx.too_large) {
        snprintf(r->failure, sizeof r->failure,
                 "decompressed response exceeds %u bytes",
                 (unsigned)r->max_bytes);
        r->out->n = 0;
        return 2;
    }
    if (!buf_ok(r->out)) {
        snprintf(r->failure, sizeof r->failure,
                 "response does not fit in memory");
        r->out->n = 0;
        return 2;
    }
    if (rc != CURLE_OK) {
        if (ctx.blocked)
            snprintf(r->failure, sizeof r->failure,
                     "destination resolved to a non-public address");
        else if (rc == CURLE_URL_MALFORMAT && !r->effective_url[0])
            snprintf(
                r->failure, sizeof r->failure,
                "redirect URL is malformed, unsupported, or contains credentials");
        else
            snprintf(r->failure, sizeof r->failure, "%s",
                     curl_err[0] ? curl_err : curl_easy_strerror(rc));
        r->out->n = 0;
        return 2;
    }
    if (status < 200 || status >= 300) return -(i32)status;
    return 0;
}

i32 http_post(const HttpReq *r) {
    if (r->body_out == NULL && !r->line_arena) {
        agent_log(AGENT_LOG_ERROR, "streaming request without a line arena");
        return 1;
    }
    char load_err[256] = {0};
    if (!curl_load(load_err, sizeof load_err)) {
        if (r->fail_out && r->fail_cap)
            snprintf(r->fail_out, r->fail_cap, "%s", load_err);
        agent_log(AGENT_LOG_ERROR, "%s", load_err);
        return 1;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        agent_log(AGENT_LOG_ERROR, "curl init failed");
        return 1;
    }

    const char *path = api_post_path(r->api);
    char url[2048];
    if (!build_url(url, sizeof url, r->base_url, path)) {
        curl_easy_cleanup(curl);
        agent_log(AGENT_LOG_ERROR, "base_url is empty or too long");
        return 1;
    }

    b8 stream = r->body_out == NULL;
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, stream ? "Accept: text/event-stream"
                                          : "Accept: application/json");
    hdrs = auth_header(hdrs, r->api_key, r->api);

    Ctx ctx = {0};
    ctx.r = r;

    if (stream) buf_init(&ctx.line, r->line_arena, 8192);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, r->body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream ? write_cb : body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,
                     stream ? (void *)&ctx : (void *)r->body_out);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, drop_header_cb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    http_apply_ca(curl);

    CURLM *multi = curl_multi_init();
    if (!multi) {
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        agent_log(AGENT_LOG_ERROR, "curl multi init failed");
        return 1;
    }
    curl_multi_add_handle(multi, curl);

    CURLcode rc = CURLE_OK;
    b8 interrupted = false;
    i32 running = 1;
    while (running) {
        CURLMcode mc = curl_multi_perform(multi, &running);
        if (ctx.aborted) break;
        if (mc == CURLM_OK && running) {
            struct curl_waitfd extra = {r->idle_fd, CURL_WAIT_POLLIN, 0};
            b8 watch = r->idle_fd >= 0;
            i32 numfds = 0;
            mc = curl_multi_poll(multi, watch ? &extra : NULL, watch ? 1u : 0u,
                                 HTTP_POLL_MS, &numfds);
            ctx.polls++;
        }
        if (mc != CURLM_OK) {
            agent_log(AGENT_LOG_ERROR, "curl multi: %s",
                      curl_multi_strerror(mc));
            rc = CURLE_RECV_ERROR;
            break;
        }
        if (r->on_idle) r->on_idle(r->idle_ud);
        if (r->interrupt_flag && *r->interrupt_flag) {
            interrupted = true;
            break;
        }
    }

    if (!interrupted && !ctx.aborted && rc == CURLE_OK) {
        CURLMsg *msg;
        i32 left = 0;
        while ((msg = curl_multi_info_read(multi, &left)))
            if (msg->msg == CURLMSG_DONE) rc = msg->data.result;
    }


    if (stream && !interrupted && !ctx.aborted && rc == CURLE_OK && ctx.line.n)
        dispatch_line(&ctx, "\n", 1);


    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    i64 http = (i64)http_code;
    http_record("POST", path, url, curl, rc, http, stream ? &ctx : NULL,
                interrupted);

    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (interrupted) return 3;
    if (ctx.oom) {
        agent_log(AGENT_LOG_ERROR, "an event did not fit in memory");
        if (r->fail_out && r->fail_cap)
            snprintf(r->fail_out, r->fail_cap,
                     "an event did not fit in memory");
        return 2;
    }
    if (rc != CURLE_OK) {
        agent_log(AGENT_LOG_ERROR, "curl: %s", curl_easy_strerror(rc));
        if (r->fail_out && r->fail_cap)
            snprintf(r->fail_out, r->fail_cap, "%s", curl_easy_strerror(rc));
        return 2;
    }
    if (http < 200 || http >= 300) { return -(i32)http; }
    return 0;
}
