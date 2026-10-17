#include "agent.h"
#include "../vendor/miniz/bridge.h"
#include "../vendor/wuffs/bridge.h"

#include <string.h>

/* ---- images scaled down for the terminal ---------------------------------
 * The copy of a PNG that the transcript draws, at the pixel size it is drawn
 * at. A terminal keeps each image it is sent decoded at full size, and a
 * large one sent whole through tmux has crashed Ghostty. The image the model
 * gets is never touched here.
 *
 * Wuffs decodes the PNG to premultiplied RGBA, a box filter averages it down,
 * and the result goes back out as a PNG through miniz's deflate. Every buffer
 * comes from the caller's arena and its total is checked against the room
 * left before the first one is taken, so a large image is refused rather
 * than left half-built.
 */

#define THUMB_MAX_PIXELS (1u << 24)
#define THUMB_LEVEL      4
#define THUMB_ALIGN      16u
#define THUMB_BUFFERS    12u

_Static_assert(THUMB_MAX_PIXELS <= UINT32_MAX / 255u,
               "a row of channel sums fits in u32");

static const u8 k_png_sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

void thumb_size(u32 w, u32 h, u32 max_w, u32 max_h, u32 *out_w, u32 *out_h) {
    u64 tw = w, th = h;
    if (max_w && max_h && (w > max_w || h > max_h)) {
        if ((u64)w * max_h >= (u64)h * max_w) {
            tw = max_w;
            th = ((u64)h * max_w + w / 2) / w;
        } else {
            th = max_h;
            tw = ((u64)w * max_h + h / 2) / h;
        }
    }
    if (tw > w) tw = w;
    if (th > h) th = h;
    *out_w = tw ? (u32)tw : 1;
    *out_h = th ? (u32)th : 1;
}

static b8 room_add(u64 *total, u64 n) {
    if (n > UINT64_MAX - *total - THUMB_ALIGN) return false;
    *total += n + THUMB_ALIGN;
    return true;
}

static b8 room_left(const Arena *a, u64 need) {
    return need <= (u64)(a->cap - a->off);
}

static void box_filter(const u8 *src, u32 w, u32 h, u8 *dst, u32 dw, u32 dh,
                       u32 *xs, u64 *acc) {
    for (u32 x = 0; x <= dw; x++) xs[x] = (u32)((u64)x * w / dw);
    for (u32 y = 0; y < dh; y++) {
        u32 y0 = (u32)((u64)y * h / dh);
        u32 y1 = (u32)((u64)(y + 1) * h / dh);
        memset(acc, 0, (size_t)dw * 4 * sizeof *acc);
        for (u32 sy = y0; sy < y1; sy++) {
            const u8 *row = src + (size_t)sy * w * 4;
            for (u32 x = 0; x < dw; x++) {
                u32 sum[4] = {0, 0, 0, 0};
                for (u32 sx = xs[x]; sx < xs[x + 1]; sx++) {
                    const u8 *p = row + (size_t)sx * 4;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    sum[3] += p[3];
                }
                for (u32 c = 0; c < 4; c++) acc[(size_t)x * 4 + c] += sum[c];
            }
        }
        u8 *out = dst + (size_t)y * dw * 4;
        for (u32 x = 0; x < dw; x++) {
            u64 area = (u64)(y1 - y0) * (xs[x + 1] - xs[x]);
            for (u32 c = 0; c < 4; c++)
                out[(size_t)x * 4 + c] =
                    (u8)((acc[(size_t)x * 4 + c] + area / 2) / area);
        }
    }
}

static b8 all_opaque(const u8 *px, size_t n) {
    for (size_t i = 3; i < n; i += 4)
        if (px[i] != 255) return false;
    return true;
}

static void unpremultiply(const u8 *px, u32 w, u32 ch, u8 *out) {
    for (u32 x = 0; x < w; x++) {
        const u8 *p = px + (size_t)x * 4;
        u8 *o = out + (size_t)x * ch;
        u32 a = p[3];
        for (u32 c = 0; c < 3; c++) {
            u32 v = a ? (p[c] * 255u + a / 2) / a : 0;
            o[c] = (u8)(v > 255 ? 255 : v);
        }
        if (ch == 4) o[3] = (u8)a;
    }
}

static u8 paeth(u8 a, u8 b, u8 c) {
    i32 p = (i32)a + b - c;
    i32 pa = p > a ? p - a : a - p;
    i32 pb = p > b ? p - b : b - p;
    i32 pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static void filter_row(const u8 *cur, const u8 *prev, size_t n, u32 ch,
                       u8 *trial[5], u8 *out) {
    u64 best_cost = UINT64_MAX;
    u8 best = 0;
    for (u8 f = 0; f < 5; f++) {
        u8 *t = trial[f];
        u64 cost = 0;
        for (size_t i = 0; i < n; i++) {
            u8 left = i >= ch ? cur[i - ch] : 0;
            u8 up = prev[i];
            u8 corner = i >= ch ? prev[i - ch] : 0;
            u8 pred = (u8)(f == 1   ? left
                           : f == 2 ? up
                           : f == 3 ? (left + up) / 2
                           : f == 4 ? paeth(left, up, corner)
                                    : 0);
            t[i] = (u8)(cur[i] - pred);
            cost += t[i] < 128 ? (u32)t[i] : 256u - t[i];
        }
        if (cost < best_cost) {
            best_cost = cost;
            best = f;
        }
    }
    out[0] = best;
    memcpy(out + 1, trial[best], n);
}

static u8 *put_be32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
    return p + 4;
}

static u8 *put_chunk(u8 *p, const char kind[4], const u8 *data, size_t n) {
    p = put_be32(p, (u32)n);
    u8 *body = p;
    memcpy(p, kind, 4);
    if (n) memcpy(p + 4, data, n);
    p += 4 + n;
    return put_be32(p, agent_crc32(0, body, 4 + n));
}

ThumbStatus thumb_png(Arena *a, Str png, u32 max_w, u32 max_h, Str *out,
                      u32 *out_w, u32 *out_h) {
    if (!max_w || !max_h || png.n < sizeof k_png_sig
        || memcmp(png.p, k_png_sig, sizeof k_png_sig))
        return THUMB_BAD;
    size_t state_n = agent_png_size();
    if (!room_left(a, (u64)state_n + THUMB_ALIGN)) return THUMB_NO_ROOM;
    AgentPng *dec = arena_alloc(a, state_n, THUMB_ALIGN);
    if (!dec) return THUMB_NO_ROOM;
    u32 w = 0, h = 0;
    u64 work_n = 0;
    if (agent_png_begin(dec, png.p, png.n, &w, &h, &work_n) || !w || !h)
        return THUMB_BAD;
    if ((u64)w * h > THUMB_MAX_PIXELS) return THUMB_TOO_BIG;
    if (w <= max_w && h <= max_h) return THUMB_FITS;

    u32 dw, dh;
    thumb_size(w, h, max_w, max_h, &dw, &dh);
    u64 src_px = (u64)w * h * 4, dst_px = (u64)dw * dh * 4;
    u64 stride = (u64)dw * 4, raw_n = (u64)dh * (1 + stride);
    u64 z_cap = raw_n + raw_n / 1024 + 1024;
    u64 png_cap = sizeof k_png_sig + 25 + 12 + z_cap + 12;
    u64 need = 0;
    if (!room_add(&need, work_n) || !room_add(&need, src_px)
        || !room_add(&need, dst_px) || !room_add(&need, ((u64)dw + 1) * 4)
        || !room_add(&need, stride * sizeof(u64))
        || !room_add(&need, stride * 7) || !room_add(&need, raw_n)
        || !room_add(&need, agent_deflate_size()) || !room_add(&need, z_cap)
        || !room_add(&need, png_cap))
        return THUMB_TOO_BIG;
    if (!room_left(a, need)) return THUMB_NO_ROOM;

    u8 *work = arena_alloc(a, (size_t)(work_n ? work_n : 1), THUMB_ALIGN);
    u8 *src = arena_alloc(a, (size_t)src_px, THUMB_ALIGN);
    u8 *dst = arena_alloc(a, (size_t)dst_px, THUMB_ALIGN);
    u32 *xs = arena_alloc_array(a, (size_t)dw + 1, sizeof *xs, THUMB_ALIGN);
    u64 *acc = arena_alloc_array(a, (size_t)stride, sizeof *acc, THUMB_ALIGN);
    u8 *rows = arena_alloc(a, (size_t)stride * 7, THUMB_ALIGN);
    u8 *raw = arena_alloc(a, (size_t)raw_n, THUMB_ALIGN);
    void *zstate = arena_alloc(a, agent_deflate_size(), THUMB_ALIGN);
    u8 *z = arena_alloc(a, (size_t)z_cap, THUMB_ALIGN);
    u8 *file = arena_alloc(a, (size_t)png_cap, THUMB_ALIGN);
    if (!work || !src || !dst || !xs || !acc || !rows || !raw || !zstate || !z
        || !file)
        return THUMB_NO_ROOM;

    if (agent_png_finish(dec, work, (size_t)work_n, src, (size_t)src_px))
        return THUMB_BAD;
    box_filter(src, w, h, dst, dw, dh, xs, acc);

    u32 ch = all_opaque(dst, (size_t)dst_px) ? 3 : 4;
    size_t n = (size_t)dw * ch;
    u8 *cur = rows, *prev = rows + stride;
    u8 *trial[5];
    for (u32 f = 0; f < 5; f++) trial[f] = rows + stride * (2 + f);
    memset(prev, 0, n);
    u8 *at = raw;
    for (u32 y = 0; y < dh; y++) {
        unpremultiply(dst + (size_t)y * stride, dw, ch, cur);
        filter_row(cur, prev, n, ch, trial, at);
        at += 1 + n;
        u8 *swap = prev;
        prev = cur;
        cur = swap;
    }
    size_t z_n = agent_deflate(zstate, raw, (size_t)(at - raw), z,
                               (size_t)z_cap, THUMB_LEVEL);
    if (!z_n) return THUMB_NO_ROOM;

    u8 ihdr[13];
    put_be32(ihdr, dw);
    put_be32(ihdr + 4, dh);
    ihdr[8] = 8;
    ihdr[9] = ch == 4 ? 6 : 2;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    u8 *p = file;
    memcpy(p, k_png_sig, sizeof k_png_sig);
    p += sizeof k_png_sig;
    p = put_chunk(p, "IHDR", ihdr, sizeof ihdr);
    p = put_chunk(p, "IDAT", z, z_n);
    p = put_chunk(p, "IEND", NULL, 0);
    *out = (Str){(const char *)file, (size_t)(p - file)};
    *out_w = dw;
    *out_h = dh;
    return THUMB_SCALED;
}
