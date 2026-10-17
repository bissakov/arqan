#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_INFLATE_APIS
#define MINIZ_NO_MALLOC
#include "miniz.c"
#include "bridge.h"

size_t
agent_deflate_size(void)
{
    return sizeof(tdefl_compressor);
}

size_t
agent_deflate(void *state, const void *in, size_t in_n, void *out,
              size_t out_cap, int level)
{
    mz_uint flags = TDEFL_WRITE_ZLIB_HEADER
                    | tdefl_create_comp_flags_from_zip_params(
                        level, 15, MZ_DEFAULT_STRATEGY);
    if (tdefl_init((tdefl_compressor *) state, NULL, NULL, (int) flags)
        != TDEFL_STATUS_OKAY)
    {
        return 0;
    }
    size_t in_left = in_n, out_n = out_cap;
    tdefl_status st = tdefl_compress((tdefl_compressor *) state, in, &in_left,
                                     out, &out_n, TDEFL_FINISH);
    return st == TDEFL_STATUS_DONE && in_left == in_n ? out_n : 0;
}

uint32_t
agent_crc32(uint32_t crc, const void *data, size_t n)
{
    return (uint32_t) mz_crc32(crc, (const unsigned char *) data, n);
}
