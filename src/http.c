/* http.c: libcurl streaming POST (SSE).
 *
 * We hand libcurl a write callback that buffers into a small stack buffer and
 * emits one line at a time to on_line. No heap use on our side.
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
} Ctx;

/* Accumulate into line[], dispatch on newline. Returns false once a sink has
 * asked for the stream to end, which is the contract HttpReq.on_line states. */
static b8 dispatch_line(Ctx *c, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ch = p[i];
        if (ch == '\n') {
            if (c->llen > 0 && c->line[c->llen-1] == '\r') c->llen--;
            Str ln = { c->line, c->llen };
            c->llen = 0;
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
    /* Anything other than `total` tells curl to fail the transfer, which is
     * exactly what an aborting sink wants. */
    return dispatch_line(c, p, total) ? total : 0;
}

static size_t body_cb(char *p, size_t sz, size_t n, void *ud) {
    Buf *b = (Buf *)ud;
    size_t total = sz * n;
    buf_put(b, p, total);
    /* A short buffer fails the transfer rather than silently truncating the
     * document the caller is about to parse. */
    return buf_ok(b) ? total : 0;
}

/* Headers curl hands us are of no use to either request we make. */
static size_t drop_header_cb(char *p, size_t sz, size_t n, void *ud) {
    (void)p; (void)ud;
    return sz * n;
}

/* base_url + path into `url`; false when it does not fit, which is a config
 * error rather than something to grow a buffer for. */
static b8 build_url(char *url, size_t cap, const char *base_url,
                    const char *path) {
    size_t base_n = base_url ? strlen(base_url) : 0;
    size_t path_n = strlen(path);
    if (base_n == 0 || base_n + path_n + 1 > cap) return false;
    memcpy(url, base_url, base_n);
    memcpy(url + base_n, path, path_n + 1);
    return true;
}

/* "Authorization: Bearer <key>" when there is a key: passing NULL to "%s" is
 * undefined, and "Bearer (null)" is not a request worth sending. */
static struct curl_slist *auth_header(struct curl_slist *hdrs,
                                      const char *api_key) {
    if (!api_key || !*api_key) return hdrs;
    char auth[1024];
    i32 an = snprintf(auth, sizeof auth, "Authorization: Bearer %s", api_key);
    if (an > 0 && (size_t)an < sizeof auth) return curl_slist_append(hdrs, auth);
    yoke_log(YOKE_LOG_WARN, "api key too long; sending no Authorization header");
    return hdrs;
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
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        yoke_log(YOKE_LOG_ERROR, "curl: %s", curl_easy_strerror(rc));
        return 2;
    }
    if (http_code < 200 || http_code >= 300) return -(i32)http_code;
    return 0;
}

/* How long a wait may last before we re-check the interrupt flag. Short enough
 * that Ctrl-C feels immediate, long enough to stay idle between events. */
#define HTTP_POLL_MS 100

i32 http_sse_post(const HttpReq *r) {
    CURL *curl = curl_easy_init();
    if (!curl) { yoke_log(YOKE_LOG_ERROR, "curl init failed"); return 1; }

    char url[2048];
    if (!build_url(url, sizeof url, r->base_url, "/chat/completions")) {
        curl_easy_cleanup(curl);
        yoke_log(YOKE_LOG_ERROR, "base_url is empty or too long");
        return 1;
    }

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
    hdrs = auth_header(hdrs, r->api_key);

    Ctx ctx = { r, {0}, 0, false };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, r->body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, drop_header_cb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    /* We own SIGWINCH/SIGINT and run single-threaded; curl's signal-based
     * resolver timeouts would fire into our handlers. */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);

    /* Driven through the multi interface so the wait covers our idle fd as
     * well as curl's sockets: the caller's UI stays live for the whole
     * request without a second thread. */
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

    /* curl writes a `long` through this pointer, whatever its width. */
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    i64 http = (i64)http_code;

    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (interrupted) return 3; /* expected user cancellation */
    if (rc != CURLE_OK) {
        yoke_log(YOKE_LOG_ERROR, "curl: %s", curl_easy_strerror(rc));
        return 2;
    }
    if (http < 200 || http >= 300) {
        return -(i32)http;   /* negative HTTP code signals the error */
    }
    return 0;
}
