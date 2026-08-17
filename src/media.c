#include "agent.h"

#include <stdio.h>
#include <string.h>

/* ---- images attached to a turn -------------------------------------------
 * Formats are recognized from their headers rather than from a path: what is
 * sent has to match the media type declared beside it, and an extension is
 * not evidence. Nothing here decodes pixels, so the dimensions come from the
 * header fields alone and are only ever used to refuse an image and to
 * describe it.
 */

b8 media_init(MediaSet *m, Arena *persist, size_t cap) {
    memset(m, 0, sizeof *m);
    if (!cap) return false;
    m->mime  = arena_new(persist, Str, cap);
    m->bytes = arena_new(persist, Str, cap);
    m->label = arena_new(persist, Str, cap);
    m->file  = arena_new(persist, Str, cap);
    m->w     = arena_new(persist, u32, cap);
    m->h     = arena_new(persist, u32, cap);
    if (!m->mime || !m->bytes || !m->label || !m->file || !m->w || !m->h)
        return false;
    m->cap = cap;
    return true;
}

static u32 be16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }
static u32 be32(const u8 *p) {
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}
static u32 le16(const u8 *p) { return (u32)p[1] << 8 | p[0]; }

/* The frame header of the first SOFn segment. Every other segment carries its
 * length, so the scan steps over them rather than searching for a signature
 * that could occur in entropy-coded data. */
static void jpeg_size(Str s, u32 *w, u32 *h) {
    const u8 *p = (const u8 *)s.p;
    size_t i = 2;
    while (i + 9 < s.n) {
        if (p[i] != 0xff) return;
        u8 marker = p[i + 1];
        if (marker == 0xff) { i++; continue; }          // fill bytes
        if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd9)) {
            i += 2;
            continue;
        }
        if (marker == 0xda) return;                     // scan data begins
        u32 len = be16(p + i + 2);
        if (len < 2) return;
        b8 sof = (marker >= 0xc0 && marker <= 0xcf)
              && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
        if (sof) {
            if (i + 9 >= s.n) return;
            *h = be16(p + i + 5);
            *w = be16(p + i + 7);
            return;
        }
        i += 2 + len;
    }
}

/* The three WebP chunk layouts. A lossy frame's dimensions sit behind a
 * three-byte start code; a lossless one packs them fourteen bits at a time. */
static void webp_size(Str s, u32 *w, u32 *h) {
    const u8 *p = (const u8 *)s.p;
    if (s.n < 30) return;
    if (!memcmp(p + 12, "VP8X", 4)) {
        *w = ((u32)p[24] | (u32)p[25] << 8 | (u32)p[26] << 16) + 1;
        *h = ((u32)p[27] | (u32)p[28] << 8 | (u32)p[29] << 16) + 1;
    } else if (!memcmp(p + 12, "VP8 ", 4)) {
        if (p[23] != 0x9d || p[24] != 0x01 || p[25] != 0x2a) return;
        *w = le16(p + 26) & 0x3fffu;
        *h = le16(p + 28) & 0x3fffu;
    } else if (!memcmp(p + 12, "VP8L", 4)) {
        if (p[20] != 0x2f) return;
        u32 bits = (u32)p[21] | (u32)p[22] << 8 | (u32)p[23] << 16
                 | (u32)p[24] << 24;
        *w = (bits & 0x3fffu) + 1;
        *h = ((bits >> 14) & 0x3fffu) + 1;
    }
}

b8 media_sniff(Str bytes, Str *mime, u32 *w, u32 *h) {
    const u8 *p = (const u8 *)bytes.p;
    *mime = (Str){0};
    *w = *h = 0;
    if (bytes.n < 16) return false;
    if (!memcmp(p, "\x89PNG\r\n\x1a\n", 8)) {
        *mime = STR("image/png");
        if (bytes.n >= 24 && !memcmp(p + 12, "IHDR", 4)) {
            *w = be32(p + 16);
            *h = be32(p + 20);
        }
        return true;
    }
    if (p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff) {
        *mime = STR("image/jpeg");
        jpeg_size(bytes, w, h);
        return true;
    }
    if (!memcmp(p, "GIF87a", 6) || !memcmp(p, "GIF89a", 6)) {
        *mime = STR("image/gif");
        *w = le16(p + 6);
        *h = le16(p + 8);
        return true;
    }
    if (!memcmp(p, "RIFF", 4) && bytes.n >= 16 && !memcmp(p + 8, "WEBP", 4)) {
        *mime = STR("image/webp");
        webp_size(bytes, w, h);
        return true;
    }
    return false;
}

Str media_ext(Str mime) {
    if (str_eq(mime, STR("image/png")))  return STR("png");
    if (str_eq(mime, STR("image/jpeg"))) return STR("jpg");
    if (str_eq(mime, STR("image/gif")))  return STR("gif");
    if (str_eq(mime, STR("image/webp"))) return STR("webp");
    return STR("bin");
}

// The type name without the "image/", which is what a chip has room for.
static Str media_kind(Str mime) {
    return str_starts(mime, STR("image/"))
         ? (Str){ mime.p + 6, mime.n - 6 } : mime;
}

static size_t media_push(MediaSet *m, Str mime, Str bytes, Str label, Str file,
                         u32 w, u32 h) {
    if (m->n >= m->cap) return MEDIA_NONE;
    size_t i = m->n++;
    m->mime[i]  = mime;
    m->bytes[i] = bytes;
    m->label[i] = label;
    m->file[i]  = file;
    m->w[i] = w;
    m->h[i] = h;
    return i;
}

size_t media_add(MediaSet *m, Arena *persist, Str bytes, Str label,
                 char *err, size_t err_cap) {
    if (m->n >= m->cap) {
        snprintf(err, err_cap, "no room for another image in this session");
        return MEDIA_NONE;
    }
    if (bytes.n > AGENT_MAX_IMAGE_BYTES) {
        char have[32], max[32];
        spill_size_text(have, sizeof have, bytes.n);
        spill_size_text(max, sizeof max, AGENT_MAX_IMAGE_BYTES);
        snprintf(err, err_cap, "image is %s; the limit is %s", have, max);
        return MEDIA_NONE;
    }
    Str mime;
    u32 w, h;
    if (!media_sniff(bytes, &mime, &w, &h)) {
        snprintf(err, err_cap, "not a PNG, JPEG, GIF or WebP image");
        return MEDIA_NONE;
    }
    if (w > AGENT_MAX_IMAGE_SIDE || h > AGENT_MAX_IMAGE_SIDE) {
        snprintf(err, err_cap, "image is %ux%u; the limit is %u on a side",
                 w, h, (unsigned)AGENT_MAX_IMAGE_SIDE);
        return MEDIA_NONE;
    }
    Str kept = str_dup(persist, bytes);
    /* The label is shown in a transcript row, so it is cut where a glyph
     * ends rather than mid-sequence. */
    Str name = str_dup_opt(persist, str_clip_utf8(label, 64));
    if (!kept.p) {
        snprintf(err, err_cap, "not enough memory to hold that image");
        return MEDIA_NONE;
    }
    return media_push(m, mime, kept, name, (Str){0}, w, h);
}

size_t media_add_file(MediaSet *m, Arena *persist, Arena *scratch, Str path,
                      char *err, size_t err_cap) {
    char z[AGENT_MAX_PATH];
    if (!path.n || path.n >= sizeof z) {
        snprintf(err, err_cap, "path is %s",
                 path.n ? "too long" : "missing");
        return MEDIA_NONE;
    }
    memcpy(z, path.p, path.n);
    z[path.n] = '\0';

    size_t mark = scratch->off;
    Str body = {0};
    u64 size = 0;
    FileStatus st = file_read(scratch, z, AGENT_MAX_IMAGE_BYTES, 0, &body,
                              &size);
    size_t id = MEDIA_NONE;
    switch (st) {
        case FILE_OK: break;
        case FILE_MISSING:     snprintf(err, err_cap, "%s: no such file", z); break;
        case FILE_NOT_REGULAR: snprintf(err, err_cap, "%s is not a file", z); break;
        case FILE_TOO_LARGE: {
            char have[32], max[32];
            spill_size_text(have, sizeof have, (size_t)size);
            spill_size_text(max, sizeof max, AGENT_MAX_IMAGE_BYTES);
            snprintf(err, err_cap, "%s is %s; the limit is %s", z, have, max);
            break;
        }
        case FILE_NO_MEMORY:   snprintf(err, err_cap, "%s does not fit in memory", z); break;
        case FILE_UNREADABLE:  snprintf(err, err_cap, "%s could not be read", z); break;
    }
    if (st == FILE_OK) {
        // The label is the basename: a chip has no room for a project path.
        Str label = path;
        for (size_t i = path.n; i-- > 0;)
            if (path.p[i] == '/') { label = str_drop(path, i + 1); break; }
        id = media_add(m, persist, body, label, err, err_cap);
    }
    scratch->off = mark;
    return id;
}

size_t media_add_missing(MediaSet *m, Arena *persist, Str label, Str mime,
                         Str file) {
    return media_push(m, str_dup_opt(persist, mime), (Str){0},
                      str_dup_opt(persist, label), str_dup_opt(persist, file),
                      0, 0);
}

b8 media_live(const MediaSet *m, size_t id) {
    return m && id < m->n && m->bytes[id].n > 0;
}

/* Keep `n` of the entries a turn added from `base` on and drop the rest, so
 * what it attaches ends up contiguous there. Only that tail moves: an entry
 * an earlier turn already indexes keeps its place. The ids are ascending and
 * each is at or past where it lands, so the copy is safe in place. Returns
 * where the kept run starts. */
size_t media_keep(MediaSet *m, size_t base, const size_t *ids, size_t n) {
    if (!n || base >= m->n || n > m->n - base) return m->n;
    for (size_t i = 0; i < n; i++) {
        size_t src = ids[i], dst = base + i;
        if (src >= m->n || src < dst) return m->n;
        if (src == dst) continue;
        m->mime[dst]  = m->mime[src];
        m->bytes[dst] = m->bytes[src];
        m->label[dst] = m->label[src];
        m->file[dst]  = m->file[src];
        m->w[dst] = m->w[src];
        m->h[dst] = m->h[src];
    }
    m->n = base + n;
    return base;
}

void media_describe(char *out, size_t cap, const MediaSet *m, size_t id) {
    if (!cap) return;
    out[0] = '\0';
    if (!m || id >= m->n) return;
    Str kind = media_kind(m->mime[id]);
    if (!m->bytes[id].n) {
        snprintf(out, cap, "%.*s - unavailable", (i32)kind.n, kind.p);
        return;
    }
    char size[32];
    spill_size_text(size, sizeof size, m->bytes[id].n);
    if (m->w[id] && m->h[id])
        snprintf(out, cap, "%.*s %ux%u - %s", (i32)kind.n, kind.p,
                 m->w[id], m->h[id], size);
    else
        snprintf(out, cap, "%.*s - %s", (i32)kind.n, kind.p, size);
}

void media_write_openai(Buf *b, const MediaSet *m, size_t id) {
    buf_puts(b, STR("{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:"));
    buf_json_chars(b, m->mime[id]);
    buf_puts(b, STR(";base64,"));
    buf_base64(b, m->bytes[id].p, m->bytes[id].n);
    buf_puts(b, STR("\"}}"));
}

void media_write_anthropic(Buf *b, const MediaSet *m, size_t id) {
    buf_puts(b, STR("{\"type\":\"image\",\"source\":{\"type\":\"base64\""
                    ",\"media_type\":"));
    buf_json_str(b, m->mime[id]);
    buf_puts(b, STR(",\"data\":\""));
    buf_base64(b, m->bytes[id].p, m->bytes[id].n);
    buf_puts(b, STR("\"}}"));
}
