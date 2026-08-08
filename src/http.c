/* http.c: libcurl POST, streaming or not.
 *
 * A stream buffers into a small stack buffer and emits one line at a time to
 * on_line; a single reply accumulates whole into the caller's Buf.
 */
#include "yoke.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <curl/curl.h>

typedef struct {
    const HttpReq *r;
    char   line[8192];
    size_t llen;
    b8     aborted;   /* on_line asked us to stop */
    /* What no return code carries: how many lines arrived and the longest the
     * stream went silent between them, which is the "it froze" report. */
    size_t lines;
    size_t polls;
    f64    last_write;
    f64    stall;
} Ctx;

/* Accumulate into line[], dispatch on newline. False once a sink has asked
 * for the stream to end. */
static b8 dispatch_line(Ctx *c, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ch = p[i];
        if (ch == '\n') {
            if (c->llen > 0 && c->line[c->llen-1] == '\r') c->llen--;
            Str ln = { c->line, c->llen };
            c->llen = 0;
            c->lines++;
            if (c->r->on_line && !c->r->on_line(ln, c->r->ud)) {
                c->aborted = true;
                return false;
            }
        } else {
            if (c->llen < sizeof c->line - 1) c->line[c->llen++] = ch;
        }
    }
    return true;
}

static size_t write_cb(char *p, size_t sz, size_t n, void *ud) {
    Ctx *c = (Ctx *)ud;
    size_t total = sz * n;
    f64 now = yoke_now_seconds();
    if (c->last_write > 0 && now - c->last_write > c->stall)
        c->stall = now - c->last_write;
    c->last_write = now;
    /* Anything but `total` fails the transfer, which is what an aborting sink
     * wants. */
    return dispatch_line(c, p, total) ? total : 0;
}

static size_t body_cb(char *p, size_t sz, size_t n, void *ud) {
    Buf *b = (Buf *)ud;
    size_t total = sz * n;
    buf_put(b, p, total);
    /* A short buffer fails the transfer rather than truncating the document
     * the caller is about to parse. */
    return buf_ok(b) ? total : 0;
}

/* Neither request here has any use for the headers. */
static size_t drop_header_cb(char *p, size_t sz, size_t n, void *ud) {
    (void)p; (void)ud;
    return sz * n;
}

/* False when it does not fit, which is a config error rather than something
 * to grow a buffer for. */
static b8 build_url(char *url, size_t cap, const char *base_url,
                    const char *path) {
    size_t base_n = base_url ? strlen(base_url) : 0;
    size_t path_n = strlen(path);
    if (base_n == 0 || base_n + path_n + 1 > cap) return false;
    memcpy(url, base_url, base_n);
    memcpy(url + base_n, path, path_n + 1);
    return true;
}

/* Only when there is a key: "Bearer (null)" is not a request worth sending. */
static struct curl_slist *auth_header(struct curl_slist *hdrs,
                                      const char *api_key) {
    if (!api_key || !*api_key) return hdrs;
    char auth[1024];
    i32 an = snprintf(auth, sizeof auth, "Authorization: Bearer %s", api_key);
    if (an > 0 && (size_t)an < sizeof auth) return curl_slist_append(hdrs, auth);
    yoke_log(YOKE_LOG_WARN, "api key too long; sending no Authorization header");
    return hdrs;
}

/* The host of a URL, without the scheme, the port or anything after it. */
static Str url_host(const char *url) {
    Str s = str_c(url ? url : "");
    const char *sep = strstr(s.p, "://");
    if (sep) s = str_drop(s, (size_t)(sep - s.p) + 3);
    size_t n = 0;
    while (n < s.n && s.p[n] != '/' && s.p[n] != ':') n++;
    return (Str){ s.p, n };
}

static b8 host_is_loopback(Str host) {
    return str_eq(host, STR("localhost")) || str_eq(host, STR("::1"))
        || str_eq(host, STR("[::1]")) || str_starts(host, STR("127."));
}

/* curl reports its phases in microseconds; the record is in milliseconds. */
static i64 curl_ms(CURL *curl, CURLINFO info) {
    curl_off_t us = 0;
    if (curl_easy_getinfo(curl, info, &us) != CURLE_OK || us < 0) return -1;
    return (i64)(us / 1000);
}

/* curl's own timings and counters, which nothing else in yoke can reach. The
 * endpoint is a hash and a class rather than a URL, since a private host
 * names its owner the way a path does. */
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
    /* A fixed catalogue string of curl's, nothing of the conversation. */
    if (rc != CURLE_OK) tel_str(&e, "curl_error", str_c(curl_easy_strerror(rc)));
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
    /* Which family the connection ended up on, never the address itself. */
    char *ip = NULL;
    if (curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ip) == CURLE_OK && ip)
        tel_str(&e, "ip", strchr(ip, ':') ? STR("v6") : STR("v4"));

    if (sse) {
        tel_int(&e, "sse_lines", (i64)sse->lines);
        tel_int(&e, "polls", (i64)sse->polls);
        tel_int(&e, "stall_ms", (i64)(sse->stall * 1000.0));
        tel_bool(&e, "aborted", sse->aborted);
    }
    tel_send(&e);
}

i32 http_get(const char *base_url, const char *path, const char *api_key,
             Buf *out) {
    char url[2048];
    if (!build_url(url, sizeof url, base_url, path)) {
        yoke_log(YOKE_LOG_ERROR, "base_url is empty or too long");
        return 1;
    }
    CURL *curl = curl_easy_init();
    if (!curl) { yoke_log(YOKE_LOG_ERROR, "curl init failed"); return 1; }

    struct curl_slist *hdrs = curl_slist_append(NULL, "Accept: application/json");
    hdrs = auth_header(hdrs, api_key);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, drop_header_cb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    http_record("GET", path, url, curl, rc, (i64)http_code, NULL, false);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        yoke_log(YOKE_LOG_ERROR, "curl: %s", curl_easy_strerror(rc));
        return 2;
    }
    if (http_code < 200 || http_code >= 300) return -(i32)http_code;
    return 0;
}

/* Short enough that Ctrl-C feels immediate, long enough to stay idle between
 * events. */
#define HTTP_POLL_MS 100

i32 http_post(const HttpReq *r) {
    CURL *curl = curl_easy_init();
    if (!curl) { yoke_log(YOKE_LOG_ERROR, "curl init failed"); return 1; }

    char url[2048];
    if (!build_url(url, sizeof url, r->base_url, "/chat/completions")) {
        curl_easy_cleanup(curl);
        yoke_log(YOKE_LOG_ERROR, "base_url is empty or too long");
        return 1;
    }

    b8 stream = r->body_out == NULL;
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, stream ? "Accept: text/event-stream"
                                          : "Accept: application/json");
    hdrs = auth_header(hdrs, r->api_key);

    Ctx ctx = {0};
    ctx.r = r;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, r->body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream ? write_cb : body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, stream ? (void *)&ctx
                                                     : (void *)r->body_out);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, drop_header_cb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    /* SIGWINCH and SIGINT are ours, and curl's signal-based resolver timeouts
     * would fire into those handlers. */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);

    /* The multi interface so one wait covers the idle fd as well as curl's
     * sockets, which keeps the caller's UI live without a second thread. */
    CURLM *multi = curl_multi_init();
    if (!multi) {
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        yoke_log(YOKE_LOG_ERROR, "curl multi init failed");
        return 1;
    }
    curl_multi_add_handle(multi, curl);

    CURLcode rc = CURLE_OK;
    b8 interrupted = false;
    i32 running = 1;
    while (running) {
        CURLMcode mc = curl_multi_perform(multi, &running);
        if (mc == CURLM_OK && running) {
            struct curl_waitfd extra = {r->idle_fd, CURL_WAIT_POLLIN, 0};
            b8 watch = r->idle_fd >= 0;
            i32 numfds = 0;
            mc = curl_multi_poll(multi, watch ? &extra : NULL, watch ? 1u : 0u,
                                 HTTP_POLL_MS, &numfds);
            ctx.polls++;
        }
        if (mc != CURLM_OK) {
            yoke_log(YOKE_LOG_ERROR, "curl multi: %s", curl_multi_strerror(mc));
            rc = CURLE_RECV_ERROR;
            break;
        }
        if (r->on_idle) r->on_idle(r->idle_ud);
        if (r->interrupt_flag && *r->interrupt_flag) { interrupted = true; break; }
    }

    if (!interrupted && rc == CURLE_OK) {
        CURLMsg *msg;
        i32 left = 0;
        while ((msg = curl_multi_info_read(multi, &left)))
            if (msg->msg == CURLMSG_DONE) rc = msg->data.result;
    }

    /* A body not ending in a newline still has a last line, and for a single
     * JSON document that line is the whole reply. */
    if (stream && !interrupted && rc == CURLE_OK && ctx.llen)
        dispatch_line(&ctx, "\n", 1);

    /* curl writes a `long` through this pointer, whatever its width. */
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    i64 http = (i64)http_code;
    http_record("POST", "/chat/completions", url, curl, rc, http,
                stream ? &ctx : NULL, interrupted);

    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (interrupted) return 3;
    if (rc != CURLE_OK) {
        yoke_log(YOKE_LOG_ERROR, "curl: %s", curl_easy_strerror(rc));
        if (r->fail_out && r->fail_cap)
            snprintf(r->fail_out, r->fail_cap, "%s", curl_easy_strerror(rc));
        return 2;
    }
    if (http < 200 || http >= 300) {
        return -(i32)http;   /* negative HTTP code signals the error */
    }
    return 0;
}
