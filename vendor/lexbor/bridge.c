#include "lexbor.c"
#include "bridge.h"

AgentHtmlDoc *
agent_html_parse(const char *html, size_t len)
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
    return (AgentHtmlDoc *) doc;
}

void
agent_html_destroy(AgentHtmlDoc *doc)
{
    if (doc != NULL) {
        lxb_html_document_destroy((lxb_html_document_t *) doc);
    }
}

AgentHtmlNode *
agent_html_root(AgentHtmlDoc *doc)
{
    return (AgentHtmlNode *) lxb_dom_interface_node((lxb_html_document_t *) doc);
}

AgentHtmlNode *
agent_html_body(AgentHtmlDoc *doc)
{
    lxb_html_body_element_t *body =
        lxb_html_document_body_element((lxb_html_document_t *) doc);
    return (AgentHtmlNode *) lxb_dom_interface_node(body);
}

AgentHtmlNode *
agent_html_first_child(AgentHtmlNode *node)
{
    return (AgentHtmlNode *) lxb_dom_node_first_child((lxb_dom_node_t *) node);
}

AgentHtmlNode *
agent_html_next(AgentHtmlNode *node)
{
    return (AgentHtmlNode *) lxb_dom_node_next((lxb_dom_node_t *) node);
}

AgentHtmlNode *
agent_html_parent(AgentHtmlNode *node)
{
    return (AgentHtmlNode *) lxb_dom_node_parent((lxb_dom_node_t *) node);
}

int
agent_html_type(AgentHtmlNode *node)
{
    return (int) lxb_dom_node_type((lxb_dom_node_t *) node);
}

const char *
agent_html_tag(AgentHtmlNode *node, size_t *len)
{
    if (agent_html_type(node) != AGENT_HTML_ELEMENT) {
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    return (const char *) lxb_tag_name_by_id(
        lxb_dom_node_tag_id((lxb_dom_node_t *) node), len);
}

const char *
agent_html_text(AgentHtmlNode *node, size_t *len)
{
    int type = agent_html_type(node);
    if (type != AGENT_HTML_TEXT && type != AGENT_HTML_CDATA) {
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
agent_html_attr(AgentHtmlNode *node, const char *name, size_t name_len,
               size_t *len)
{
    if (agent_html_type(node) != AGENT_HTML_ELEMENT) {
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    return (const char *) lxb_dom_element_get_attribute(
        (lxb_dom_element_t *) node, (const lxb_char_t *) name, name_len, len);
}
