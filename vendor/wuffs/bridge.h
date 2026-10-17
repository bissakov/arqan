#ifndef AGENT_WUFFS_BRIDGE_H
#define AGENT_WUFFS_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

typedef struct AgentPng AgentPng;

size_t agent_png_size(void);
int    agent_png_begin(AgentPng *p, const void *data, size_t n, uint32_t *w,
                       uint32_t *h, uint64_t *work_n);
int    agent_png_finish(AgentPng *p, void *work, size_t work_n,
                        uint8_t *rgba, size_t rgba_n);

#endif
