#ifndef AGENT_HIGHLIGHT_QUERIES_H
#define AGENT_HIGHLIGHT_QUERIES_H

#include <stddef.h>

typedef struct {
    const char *name;
    const unsigned char *text;
    size_t size;
    const char *sha256;
} YhlQuerySource;

extern const YhlQuerySource yhl_query_sources[];
extern const size_t yhl_query_source_count;

#endif
