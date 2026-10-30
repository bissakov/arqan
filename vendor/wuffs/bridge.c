#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__STATIC_FUNCTIONS
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__ZLIB
#define WUFFS_CONFIG__MODULE__PNG
#include "wuffs-v0.3.c"
#include "bridge.h"

struct AgentPng {
    wuffs_png__decoder dec;
    wuffs_base__io_buffer src;
    wuffs_base__image_config config;
};

size_t
agent_png_size(void)
{
    return sizeof(AgentPng);
}

int
agent_png_begin(AgentPng *p, const void *data, size_t n, uint32_t *w,
                uint32_t *h, uint64_t *work_n)
{
    wuffs_base__status st = wuffs_png__decoder__initialize(
        &p->dec, sizeof p->dec, WUFFS_VERSION, 0);
    if (!wuffs_base__status__is_ok(&st)) {
        return -1;
    }
    p->src = wuffs_base__ptr_u8__reader((uint8_t *) data, n, true);
    p->config = wuffs_base__null_image_config();
    st = wuffs_png__decoder__decode_image_config(&p->dec, &p->config, &p->src);
    if (!wuffs_base__status__is_ok(&st)) {
        return -1;
    }
    *w = wuffs_base__pixel_config__width(&p->config.pixcfg);
    *h = wuffs_base__pixel_config__height(&p->config.pixcfg);
    *work_n = wuffs_png__decoder__workbuf_len(&p->dec).max_incl;
    return 0;
}

int
agent_png_finish(AgentPng *p, void *work, size_t work_n, uint8_t *rgba,
                 size_t rgba_n)
{
    uint32_t w = wuffs_base__pixel_config__width(&p->config.pixcfg);
    uint32_t h = wuffs_base__pixel_config__height(&p->config.pixcfg);
    if ((uint64_t) w * h * 4 > rgba_n) {
        return -1;
    }
    wuffs_base__pixel_config__set(&p->config.pixcfg,
                                  WUFFS_BASE__PIXEL_FORMAT__RGBA_PREMUL,
                                  WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, w, h);
    wuffs_base__pixel_buffer pb;
    wuffs_base__status st = wuffs_base__pixel_buffer__set_from_slice(
        &pb, &p->config.pixcfg, wuffs_base__make_slice_u8(rgba, rgba_n));
    if (!wuffs_base__status__is_ok(&st)) {
        return -1;
    }
    st = wuffs_png__decoder__decode_frame(
        &p->dec, &pb, &p->src, WUFFS_BASE__PIXEL_BLEND__SRC,
        wuffs_base__make_slice_u8((uint8_t *) work, work_n), NULL);
    return wuffs_base__status__is_ok(&st) ? 0 : -1;
}

struct AgentInflate {
    wuffs_zlib__decoder dec;
    uint64_t in_pos, out_pos;
    uint8_t work[WUFFS_ZLIB__DECODER_WORKBUF_LEN_MAX_INCL_WORST_CASE];
};

size_t
agent_inflate_size(void)
{
    return sizeof(AgentInflate);
}

int
agent_inflate_begin(AgentInflate *z)
{
    wuffs_base__status st = wuffs_zlib__decoder__initialize(
        &z->dec, sizeof z->dec, WUFFS_VERSION, 0);
    if (!wuffs_base__status__is_ok(&st)) {
        return -1;
    }
    wuffs_zlib__decoder__set_quirk_enabled(
        &z->dec, WUFFS_BASE__QUIRK_IGNORE_CHECKSUM, true);
    z->in_pos = 0;
    z->out_pos = 0;
    return 0;
}

/* A call after AGENT_INFLATE_FULL continues the same stream: `in` starts
 * where the last call stopped reading, and `out` is fresh room. Wuffs reads
 * back-references through the stream positions, so they carry over here. */
int
agent_inflate(AgentInflate *z, const void *in, size_t in_n, size_t *in_used,
              void *out, size_t out_cap, size_t *out_n)
{
    wuffs_base__io_buffer src =
        wuffs_base__ptr_u8__reader((uint8_t *) in, in_n, true);
    wuffs_base__io_buffer dst =
        wuffs_base__ptr_u8__writer((uint8_t *) out, out_cap);
    src.meta.pos = z->in_pos;
    dst.meta.pos = z->out_pos;
    wuffs_base__status st = wuffs_zlib__decoder__transform_io(
        &z->dec, &dst, &src,
        wuffs_base__make_slice_u8(z->work, sizeof z->work));
    *in_used = src.meta.ri;
    *out_n = dst.meta.wi;
    z->in_pos += src.meta.ri;
    z->out_pos += dst.meta.wi;
    if (wuffs_base__status__is_ok(&st)) {
        return AGENT_INFLATE_DONE;
    }
    if (st.repr == wuffs_base__suspension__short_write) {
        return AGENT_INFLATE_FULL;
    }
    return AGENT_INFLATE_BAD;
}
