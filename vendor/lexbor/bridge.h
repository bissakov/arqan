#ifndef YOKE_LEXBOR_BRIDGE_H
#define YOKE_LEXBOR_BRIDGE_H

#include <stddef.h>

typedef struct YokeHtmlDoc YokeHtmlDoc;
typedef struct YokeHtmlNode YokeHtmlNode;

enum {
    YOKE_HTML_ELEMENT = 1,
    YOKE_HTML_TEXT = 3,
    YOKE_HTML_CDATA = 4,
};

YokeHtmlDoc  *yoke_html_parse(const char *html, size_t len);
void          yoke_html_destroy(YokeHtmlDoc *doc);
YokeHtmlNode *yoke_html_root(YokeHtmlDoc *doc);
YokeHtmlNode *yoke_html_body(YokeHtmlDoc *doc);
YokeHtmlNode *yoke_html_first_child(YokeHtmlNode *node);
YokeHtmlNode *yoke_html_next(YokeHtmlNode *node);
YokeHtmlNode *yoke_html_parent(YokeHtmlNode *node);
int           yoke_html_type(YokeHtmlNode *node);
const char   *yoke_html_tag(YokeHtmlNode *node, size_t *len);
const char   *yoke_html_text(YokeHtmlNode *node, size_t *len);
const char   *yoke_html_attr(YokeHtmlNode *node, const char *name,
                             size_t name_len, size_t *len);

#endif
