/* http.c — libcurl streaming POST (SSE).
 *
 * We hand libcurl a write callback that buffers into a small stack buffer and
 * emits one line at a time to on_line. No heap use on our side.
 */
#include "ah.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <curl/curl.h>

typedef struct {
    const HttpReq *r;
    char   line[8192];
    size_t llen;
} Ctx;

static void dispatch_line(Ctx *c, const char *p, size_t n) {
    /* accumulate into line[], dispatch on newline */
    for (size_t i = 0; i < n; i++) {
        char ch = p[i];
        if (ch == '\n') {
            if (c->llen > 0 && c->line[c->llen-1] == '\r') c->llen--;
            Str ln = { c->line, c->llen };
            if (c->r->on_line) c->r->on_line(ln, c->r->ud);
            c->llen = 0;
        } else {
            if (c->llen < sizeof c->line - 1) c->line[c->llen++] = ch;
        }
    }
}

static size_t write_cb(char *p, size_t sz, size_t n, void *ud) {
    Ctx *c = (Ctx *)ud;
    size_t total = sz * n;
    dispatch_line(c, p, total);
    return total;
}

static size_t header_cb(char *p, size_t sz, size_t n, void *ud) {
    (void)ud;
    return sz * n; /* ignore */
}

static i32 xferinfo_cb(void *ud, curl_off_t dl_total, curl_off_t dl_now,
                       curl_off_t ul_total, curl_off_t ul_now) {
    (void)dl_total; (void)dl_now; (void)ul_total; (void)ul_now;
    const Ctx *c = (const Ctx *)ud;
    return c->r->interrupt_flag && *c->r->interrupt_flag ? 1 : 0;
}

i32 http_sse_post(const HttpReq *r) {
    CURL *curl = curl_easy_init();
    if (!curl) { ah_log(AH_LOG_ERROR, "curl init failed"); return 1; }

    /* build URL: base_url + "/chat/completions" */
    size_t url_len = strlen(r->base_url) + 32;
    char *url = (char *)malloc(url_len);
    snprintf(url, url_len, "%s/chat/completions", r->base_url);

    char auth[512];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", r->api_key);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
    hdrs = curl_slist_append(hdrs, auth);

    Ctx ctx = { r, {0}, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, r->body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);
    i64 http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    free(url);

    if (rc != CURLE_OK) {
        if (r->interrupt_flag && *r->interrupt_flag)
            return 3; /* expected user cancellation */
        ah_log(AH_LOG_ERROR, "curl: %s", curl_easy_strerror(rc));
        return 2;
    }
    if (http < 200 || http >= 300) {
        return -(i32)http;   /* negative HTTP code signals the error */
    }
    return 0;
}
