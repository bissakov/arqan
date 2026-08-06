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

/* How long a wait may last before we re-check the interrupt flag. Short enough
 * that Ctrl-C feels immediate, long enough to stay idle between events. */
#define HTTP_POLL_MS 100

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
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    /* Driven through the multi interface so the wait covers our idle fd as
     * well as curl's sockets: the caller's UI stays live for the whole
     * request without a second thread. */
    CURLM *multi = curl_multi_init();
    if (!multi) {
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        free(url);
        ah_log(AH_LOG_ERROR, "curl multi init failed");
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
            ah_log(AH_LOG_ERROR, "curl multi: %s", curl_multi_strerror(mc));
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

    i64 http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);

    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    free(url);

    if (interrupted) return 3; /* expected user cancellation */
    if (rc != CURLE_OK) {
        ah_log(AH_LOG_ERROR, "curl: %s", curl_easy_strerror(rc));
        return 2;
    }
    if (http < 200 || http >= 300) {
        return -(i32)http;   /* negative HTTP code signals the error */
    }
    return 0;
}
