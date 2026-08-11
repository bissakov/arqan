#ifndef AGENT_LEXBOR_BRIDGE_H
#define AGENT_LEXBOR_BRIDGE_H

#include <stddef.h>

typedef struct AgentHtmlDoc AgentHtmlDoc;
typedef struct AgentHtmlNode AgentHtmlNode;

enum {
    AGENT_HTML_ELEMENT = 1,
    AGENT_HTML_TEXT = 3,
    AGENT_HTML_CDATA = 4,
};

AgentHtmlDoc  *agent_html_parse(const char *html, size_t len);
void          agent_html_destroy(AgentHtmlDoc *doc);
AgentHtmlNode *agent_html_root(AgentHtmlDoc *doc);
AgentHtmlNode *agent_html_body(AgentHtmlDoc *doc);
AgentHtmlNode *agent_html_first_child(AgentHtmlNode *node);
AgentHtmlNode *agent_html_next(AgentHtmlNode *node);
AgentHtmlNode *agent_html_parent(AgentHtmlNode *node);
int           agent_html_type(AgentHtmlNode *node);
const char   *agent_html_tag(AgentHtmlNode *node, size_t *len);
const char   *agent_html_text(AgentHtmlNode *node, size_t *len);
const char   *agent_html_attr(AgentHtmlNode *node, const char *name,
                             size_t name_len, size_t *len);

#endif
