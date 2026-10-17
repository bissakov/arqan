#ifndef AGENT_MINIZ_BRIDGE_H
#define AGENT_MINIZ_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

size_t   agent_deflate_size(void);
size_t   agent_deflate(void *state, const void *in, size_t in_n, void *out,
                       size_t out_cap, int level);
uint32_t agent_crc32(uint32_t crc, const void *data, size_t n);

#endif
