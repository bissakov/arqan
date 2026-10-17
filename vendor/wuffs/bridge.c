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
