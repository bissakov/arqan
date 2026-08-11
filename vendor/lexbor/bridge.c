#include "lexbor.c"
#include "bridge.h"

YokeHtmlDoc *
yoke_html_parse(const char *html, size_t len)
{
    lxb_html_document_t *doc = lxb_html_document_create();
    if (doc == NULL) {
        return NULL;
    }
    if (lxb_html_document_parse(doc, (const lxb_char_t *) html, len)
        != LXB_STATUS_OK)
    {
        lxb_html_document_destroy(doc);
        return NULL;
    }
    return (YokeHtmlDoc *) doc;
}

void
yoke_html_destroy(YokeHtmlDoc *doc)
{
    if (doc != NULL) {
        lxb_html_document_destroy((lxb_html_document_t *) doc);
    }
}

YokeHtmlNode *
yoke_html_root(YokeHtmlDoc *doc)
{
    return (YokeHtmlNode *) lxb_dom_interface_node((lxb_html_document_t *) doc);
}

YokeHtmlNode *
yoke_html_body(YokeHtmlDoc *doc)
{
    lxb_html_body_element_t *body =
        lxb_html_document_body_element((lxb_html_document_t *) doc);
    return (YokeHtmlNode *) lxb_dom_interface_node(body);
}

YokeHtmlNode *
yoke_html_first_child(YokeHtmlNode *node)
{
    return (YokeHtmlNode *) lxb_dom_node_first_child((lxb_dom_node_t *) node);
}

YokeHtmlNode *
yoke_html_next(YokeHtmlNode *node)
{
    return (YokeHtmlNode *) lxb_dom_node_next((lxb_dom_node_t *) node);
}

YokeHtmlNode *
yoke_html_parent(YokeHtmlNode *node)
{
    return (YokeHtmlNode *) lxb_dom_node_parent((lxb_dom_node_t *) node);
}

int
yoke_html_type(YokeHtmlNode *node)
{
    return (int) lxb_dom_node_type((lxb_dom_node_t *) node);
}

const char *
yoke_html_tag(YokeHtmlNode *node, size_t *len)
{
    if (yoke_html_type(node) != YOKE_HTML_ELEMENT) {
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    return (const char *) lxb_tag_name_by_id(
        lxb_dom_node_tag_id((lxb_dom_node_t *) node), len);
}

const char *
yoke_html_text(YokeHtmlNode *node, size_t *len)
{
    int type = yoke_html_type(node);
    if (type != YOKE_HTML_TEXT && type != YOKE_HTML_CDATA) {
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    lxb_dom_character_data_t *data =
        lxb_dom_interface_character_data((lxb_dom_node_t *) node);
    if (len != NULL) {
        *len = data->data.length;
    }
    return (const char *) data->data.data;
}

const char *
yoke_html_attr(YokeHtmlNode *node, const char *name, size_t name_len,
               size_t *len)
{
    if (yoke_html_type(node) != YOKE_HTML_ELEMENT) {
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    return (const char *) lxb_dom_element_get_attribute(
        (lxb_dom_element_t *) node, (const lxb_char_t *) name, name_len, len);
}
