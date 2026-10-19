/* Passage ranking for the web tools.
 *
 * Splits extracted page text into passages at blank lines and headings,
 * scores each against the words and phrases of a query with BM25, and writes
 * the best of them, in page order, within a byte budget. Also writes a short
 * outline of the page's headings with their line numbers.
 */

#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PASSAGE_BYTES           800u
#define PASSAGE_PHRASE_BYTES    64u
#define PASSAGE_HEADING_SHOW    80u
#define PASSAGE_OUTLINE_ENTRIES 30u
#define PASSAGE_OUTLINE_BYTES   1024u
#define PASSAGE_OUTLINE_SHOW    100u
#define PASSAGE_NONE            0xffffffffu

static const f64 k_passage_k1 = 1.2;
static const f64 k_passage_b = 0.75;
static const f64 k_passage_heading_bonus = 1.0;
static const f64 k_passage_phrase_bonus = 3.0;

static const char *const k_passage_stopwords[] = {
    "a",    "an",    "and",   "are",  "as",  "at",   "be", "by",  "can",
    "do",   "does",  "for",   "from", "how", "i",    "in", "is",  "it",
    "of",   "on",    "or",    "that", "the", "this", "to", "was", "what",
    "when", "where", "which", "who",  "why", "with",
};

static const char *const k_passage_operators[] = {
    "site",     "inurl",     "intitle",  "intext", "allintitle",
    "allinurl", "allintext", "filetype", "ext",    "before",
    "after",    "related",   "cache",
};

static b8 passage_word_byte(char c) {
    u8 u = (u8)c;
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
           || (u >= '0' && u <= '9') || u == '_';
}

static b8 passage_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'
           || c == '\v';
}

static char passage_lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* NOTE: the unit and fuzz binaries link no libm. For x > 0, x = m * 2^e with
 * m in [1, 2), and ln(m) = 2 atanh((m - 1) / (m + 1)). */
static f64 passage_ln(f64 x) {
    if (x <= 0) return 0;
    i32 e = 0;
    while (x >= 2.0) {
        x /= 2.0;
        e++;
    }
    while (x < 1.0) {
        x *= 2.0;
        e--;
    }
    f64 z = (x - 1.0) / (x + 1.0), z2 = z * z, term = z, sum = 0;
    for (i32 k = 1; k < 40; k += 2) {
        sum += term / (f64)k;
        term *= z2;
    }
    return 2.0 * sum + (f64)e * 0.69314718055994530942;
}

/* ---- query ------------------------------------------------------------- */

Str passage_term(const PassageQuery *q, size_t i) {
    return (Str){q->text + q->off[i], q->len[i]};
}

static b8 passage_ci_eq(Str a, const char *z) {
    size_t n = strlen(z);
    if (a.n != n) return false;
    for (size_t i = 0; i < n; i++)
        if (passage_lower(a.p[i]) != z[i]) return false;
    return true;
}

static b8 passage_listed(Str w, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (passage_ci_eq(w, list[i])) return true;
    return false;
}

static void passage_add(PassageQuery *q, size_t *used, Str s, b8 phrase) {
    if (!s.n || q->n >= AGENT_PASSAGE_TERMS || s.n > sizeof q->text - *used)
        return;
    char *dst = q->text + *used;
    for (size_t i = 0; i < s.n; i++) dst[i] = passage_lower(s.p[i]);
    for (size_t i = 0; i < q->n; i++)
        if (q->phrase[i] == phrase && q->len[i] == s.n
            && !memcmp(q->text + q->off[i], dst, s.n))
            return;
    q->off[q->n] = (u32)*used;
    q->len[q->n] = (u32)s.n;
    q->phrase[q->n] = phrase;
    q->n++;
    *used += s.n;
}

static void passage_add_words(PassageQuery *q, size_t *used, Str s) {
    for (size_t i = 0; i < s.n;) {
        if (!passage_word_byte(s.p[i])) {
            i++;
            continue;
        }
        size_t start = i;
        while (i < s.n && passage_word_byte(s.p[i])) i++;
        Str w = {s.p + start, i - start};
        if (!passage_listed(w, k_passage_stopwords,
                            sizeof k_passage_stopwords
                                / sizeof k_passage_stopwords[0]))
            passage_add(q, used, w, false);
    }
}

static b8 passage_operator(Str chunk) {
    if (str_eq(chunk, STR("OR")) || str_eq(chunk, STR("AND"))) return true;
    if (chunk.n && chunk.p[0] == '-') return true;
    const char *colon = memchr(chunk.p, ':', chunk.n);
    if (!colon) return false;
    Str name = {chunk.p, (size_t)(colon - chunk.p)};
    return passage_listed(name, k_passage_operators,
                          sizeof k_passage_operators
                              / sizeof k_passage_operators[0]);
}

static void passage_add_phrase(PassageQuery *q, size_t *used, Str inner) {
    char phrase[PASSAGE_PHRASE_BYTES];
    size_t n = 0;
    b8 space = false, fits = true;
    for (size_t i = 0; i < inner.n; i++) {
        if (passage_space(inner.p[i])) {
            space = n != 0;
            continue;
        }
        if (n + (space ? 2 : 1) > sizeof phrase) {
            fits = false;
            break;
        }
        if (space) phrase[n++] = ' ';
        space = false;
        phrase[n++] = inner.p[i];
    }
    if (fits && n && memchr(phrase, ' ', n))
        passage_add(q, used, (Str){phrase, n}, true);
}

void passage_query(Str query, PassageQuery *q) {
    q->n = 0;
    size_t used = 0;
    for (size_t i = 0; i < query.n; i++) {
        if (query.p[i] != '"') continue;
        const char *close = memchr(query.p + i + 1, '"', query.n - i - 1);
        if (!close) break;
        size_t end = (size_t)(close - query.p);
        passage_add_phrase(q, &used, (Str){query.p + i + 1, end - i - 1});
        i = end;
    }
    b8 quoted = false;
    for (size_t i = 0; i < query.n;) {
        if (query.p[i] == '"') {
            quoted = !quoted;
            i++;
            continue;
        }
        if (passage_space(query.p[i])) {
            i++;
            continue;
        }
        size_t start = i;
        while (i < query.n && query.p[i] != '"' && !passage_space(query.p[i]))
            i++;
        Str chunk = {query.p + start, i - start};
        if (quoted || !passage_operator(chunk))
            passage_add_words(q, &used, chunk);
    }
}

/* ---- splitting ---------------------------------------------------------- */

typedef struct {
    u32 first, last;
    u32 start, end;
    u32 heading, heading_n;
    u32 tokens;
    u16 mask;
    u8 chosen;
    b8 starts_heading;
    u32 clip;
    f64 score;
} Passage;

typedef struct {
    Str text;
    u32 start;
    u32 number;
    b8 blank;
    b8 heading;
} PassageLine;

typedef struct {
    Str body;
    size_t off;
    u32 line;
    b8 fenced;
    b8 has_pending;
    PassageLine pending;
    u32 heading, heading_n;
} PassageScan;

static size_t passage_heading_level(Str line) {
    size_t n = 0;
    while (n < line.n && line.p[n] == '#') n++;
    if (!n || n > 6 || (n < line.n && line.p[n] != ' ')) return 0;
    return n;
}

static b8 passage_fence(Str line) {
    return str_starts(line, STR("```")) || str_starts(line, STR("~~~"));
}

static b8 passage_scan_line(PassageScan *s, PassageLine *l) {
    if (s->has_pending) {
        *l = s->pending;
        s->has_pending = false;
        return true;
    }
    Str line;
    if (!str_line(s->body, &s->off, &line)) return false;
    s->line++;
    b8 fence = passage_fence(line);
    l->text = line;
    l->start = (u32)(line.p - s->body.p);
    l->number = s->line;
    l->heading = !s->fenced && !fence && passage_heading_level(line) != 0;
    if (fence) s->fenced = !s->fenced;
    l->blank = true;
    for (size_t i = 0; i < line.n && l->blank; i++)
        if (!passage_space(line.p[i])) l->blank = false;
    return true;
}

static b8 passage_scan_block(PassageScan *s, Passage *b) {
    PassageLine l;
    do {
        if (!passage_scan_line(s, &l)) return false;
    } while (l.blank);
    if (l.heading) {
        s->heading = l.start;
        s->heading_n = (u32)l.text.n;
    }
    *b = (Passage){0};
    b->first = b->last = l.number;
    b->start = l.start;
    b->end = l.start + (u32)l.text.n;
    b->heading = s->heading;
    b->heading_n = s->heading_n;
    b->starts_heading = l.heading;
    while (passage_scan_line(s, &l)) {
        if (l.blank) break;
        if (l.heading || l.start + l.text.n - b->start > PASSAGE_BYTES) {
            s->pending = l;
            s->has_pending = true;
            break;
        }
        b->last = l.number;
        b->end = l.start + (u32)l.text.n;
    }
    return true;
}

static size_t passage_split(Str body, Passage *out) {
    PassageScan s = {0};
    s.body = body;
    s.heading = PASSAGE_NONE;
    Passage cur = {0}, b;
    b8 open = false;
    size_t n = 0;
    while (passage_scan_block(&s, &b)) {
        if (open && !b.starts_heading && b.end - cur.start <= PASSAGE_BYTES) {
            cur.last = b.last;
            cur.end = b.end;
            continue;
        }
        if (open && out) out[n] = cur;
        if (open) n++;
        cur = b;
        open = true;
    }
    if (open && out) out[n] = cur;
    if (open) n++;
    return n;
}

/* ---- scoring ------------------------------------------------------------ */

static u32 passage_phrase_at(Str text, size_t i, Str phrase) {
    size_t k = i;
    for (size_t j = 0; j < phrase.n; j++) {
        if (phrase.p[j] == ' ') {
            if (k >= text.n || !passage_space(text.p[k])) return 0;
            while (k < text.n && passage_space(text.p[k])) k++;
            continue;
        }
        if (k >= text.n || passage_lower(text.p[k]) != phrase.p[j]) return 0;
        k++;
    }
    return (u32)(k - i);
}

static u32 passage_phrase_count(Str text, Str phrase) {
    b8 word_head = passage_word_byte(phrase.p[0]);
    b8 word_tail = passage_word_byte(phrase.p[phrase.n - 1]);
    u32 count = 0;
    for (size_t i = 0; i < text.n; i++) {
        if (passage_lower(text.p[i]) != phrase.p[0]) continue;
        if (word_head && i && passage_word_byte(text.p[i - 1])) continue;
        u32 m = passage_phrase_at(text, i, phrase);
        if (!m) continue;
        if (word_tail && i + m < text.n && passage_word_byte(text.p[i + m]))
            continue;
        count++;
        i += m - 1;
    }
    return count;
}

static u32 passage_count(Str text, const PassageQuery *q,
                         u32 tf[AGENT_PASSAGE_TERMS]) {
    u32 tokens = 0;
    memset(tf, 0, sizeof(u32) * AGENT_PASSAGE_TERMS);
    for (size_t i = 0; i < text.n;) {
        if (!passage_word_byte(text.p[i])) {
            i++;
            continue;
        }
        size_t start = i;
        while (i < text.n && passage_word_byte(text.p[i])) i++;
        tokens++;
        size_t n = i - start;
        for (size_t t = 0; t < q->n; t++) {
            if (q->phrase[t] || q->len[t] != n) continue;
            const char *term = q->text + q->off[t];
            size_t k = 0;
            while (k < n && passage_lower(text.p[start + k]) == term[k]) k++;
            if (k == n) tf[t]++;
        }
    }
    for (size_t t = 0; t < q->n; t++)
        if (q->phrase[t])
            tf[t] = passage_phrase_count(text, passage_term(q, t));
    return tokens;
}

static u16 passage_mask(const PassageQuery *q,
                        const u32 tf[AGENT_PASSAGE_TERMS]) {
    u16 mask = 0;
    for (size_t t = 0; t < q->n; t++)
        if (tf[t]) mask = (u16)(mask | (1u << t));
    return mask;
}

static Str passage_text(Str body, const Passage *p) {
    return (Str){body.p + p->start, p->end - p->start};
}

static Str passage_heading_text(Str body, const Passage *p) {
    if (p->heading == PASSAGE_NONE) return (Str){0};
    Str line = {body.p + p->heading, p->heading_n};
    size_t level = passage_heading_level(line);
    return str_trim(str_drop(line, level));
}

static void passage_score(Str body, const PassageQuery *q, Passage *ps,
                          size_t n) {
    u32 df[AGENT_PASSAGE_TERMS] = {0};
    u32 tf[AGENT_PASSAGE_TERMS];
    f64 total_tokens = 0;
    for (size_t i = 0; i < n; i++) {
        ps[i].tokens = passage_count(passage_text(body, &ps[i]), q, tf);
        ps[i].mask = passage_mask(q, tf);
        total_tokens += ps[i].tokens;
        for (size_t t = 0; t < q->n; t++)
            if (tf[t]) df[t]++;
    }
    f64 avg = n && total_tokens > 0 ? total_tokens / (f64)n : 1.0;
    f64 idf[AGENT_PASSAGE_TERMS];
    for (size_t t = 0; t < q->n; t++)
        idf[t] =
            passage_ln(1.0 + ((f64)n - (f64)df[t] + 0.5) / ((f64)df[t] + 0.5));
    u32 cached_heading = PASSAGE_NONE;
    u16 heading_mask = 0;
    for (size_t i = 0; i < n; i++) {
        Passage *p = &ps[i];
        if (p->heading != cached_heading) {
            cached_heading = p->heading;
            Str h = passage_heading_text(body, p);
            passage_count(h, q, tf);
            heading_mask = passage_mask(q, tf);
        }
        p->score = 0;
        if (!p->mask && !heading_mask) continue;
        if (p->mask)
            passage_count(passage_text(body, p), q, tf);
        else
            memset(tf, 0, sizeof tf);
        f64 norm = 1.0 - k_passage_b + k_passage_b * (f64)p->tokens / avg;
        for (size_t t = 0; t < q->n; t++) {
            f64 f = (f64)tf[t];
            if (q->phrase[t])
                p->score += k_passage_phrase_bonus * (1.0 + idf[t])
                            * (f > 3.0 ? 3.0 : f);
            else if (f > 0)
                p->score += idf[t] * f * (k_passage_k1 + 1.0)
                            / (f + k_passage_k1 * norm);
            if (heading_mask & (1u << t))
                p->score += k_passage_heading_bonus * idf[t];
        }
    }
}

/* ---- selection ---------------------------------------------------------- */

typedef struct {
    f64 score;
    u32 index;
} PassageRank;

static int passage_rank_cmp(const void *a, const void *b) {
    const PassageRank *x = (const PassageRank *)a, *y = (const PassageRank *)b;
    if (x->score != y->score) return x->score > y->score ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}

static size_t passage_label(char *z, size_t cap, Str body, const Passage *p,
                            u32 last, b8 clipped) {
    Str h = str_clip_utf8(passage_heading_text(body, p), PASSAGE_HEADING_SHOW);
    char lines[48];
    if (p->first == last)
        snprintf(lines, sizeof lines, "line %u", (unsigned)p->first);
    else
        snprintf(lines, sizeof lines, "lines %u-%u", (unsigned)p->first,
                 (unsigned)last);
    i32 w;
    if (h.n)
        w = snprintf(z, cap, "[%s, under \"%.*s\"%s]", lines, (i32)h.n, h.p,
                     clipped ? ", clipped" : "");
    else
        w = snprintf(z, cap, "[%s%s]", lines, clipped ? ", clipped" : "");
    if (w < 0) return 0;
    return (size_t)w < cap ? (size_t)w : cap - 1;
}

b8 passage_write(Str body, const PassageQuery *q, size_t budget, Arena *scratch,
                 Buf *out, PassageReport *report) {
    *report = (PassageReport){0};
    if (!q->n || !body.n) return true;
    if (body.n >= PASSAGE_NONE) return false;
    size_t n = passage_split(body, NULL);
    if (!n) return true;
    Passage *ps = arena_new(scratch, Passage, n);
    if (!ps) return false;
    passage_split(body, ps);
    passage_score(body, q, ps, n);

    size_t ranked = 0;
    for (size_t i = 0; i < n; i++)
        if (ps[i].score > 0) ranked++;
    report->matched = ranked;
    if (!ranked) return true;
    PassageRank *rank = arena_new(scratch, PassageRank, ranked);
    if (!rank) return false;
    for (size_t i = 0, k = 0; i < n; i++)
        if (ps[i].score > 0) rank[k++] = (PassageRank){ps[i].score, (u32)i};
    qsort(rank, ranked, sizeof *rank, passage_rank_cmp);

    char label[256];
    size_t used = 0;
    for (size_t r = 0; r < ranked && used < budget; r++) {
        Passage *p = &ps[rank[r].index];
        size_t text_n = p->end - p->start;
        size_t cost =
            passage_label(label, sizeof label, body, p, PASSAGE_NONE - 1, false)
            + 1 + text_n + 2;
        if (cost <= budget - used) {
            p->chosen = 1;
            used += cost;
            report->shown++;
            continue;
        }
        if (report->shown) continue;
        size_t fixed =
            passage_label(label, sizeof label, body, p, p->last, true) + 1 + 2;
        if (fixed + 32 > budget - used) continue;
        Str part = str_clip_utf8(passage_text(body, p), budget - used - fixed);
        if (!part.n) continue;
        p->chosen = 1;
        p->clip = (u32)part.n;
        used += fixed + part.n;
        report->shown++;
    }

    b8 first_run = true;
    for (size_t i = 0; i < n; i++) {
        if (!ps[i].chosen) continue;
        size_t j = i;
        if (!ps[i].clip)
            while (j + 1 < n && ps[j + 1].chosen && !ps[j + 1].clip) j++;
        if (!first_run) buf_putc(out, '\n');
        first_run = false;
        size_t w = passage_label(label, sizeof label, body, &ps[i], ps[j].last,
                                 ps[i].clip != 0);
        buf_put(out, label, w);
        buf_putc(out, '\n');
        for (size_t k = i; k <= j; k++) {
            Str text = passage_text(body, &ps[k]);
            if (ps[k].clip) text.n = ps[k].clip;
            if (k > i) buf_putc(out, '\n');
            buf_puts(out, text);
            buf_putc(out, '\n');
        }
        i = j;
    }
    return buf_ok(out);
}

/* ---- outline ------------------------------------------------------------ */

size_t passage_outline(Str body, size_t budget, Buf *out) {
    if (budget > PASSAGE_OUTLINE_BYTES) budget = PASSAGE_OUTLINE_BYTES;
    size_t count[7] = {0}, bytes_all = 0;
    PassageScan s = {0};
    s.body = body;
    PassageLine l;
    char entry[PASSAGE_OUTLINE_SHOW + 32];
    while (passage_scan_line(&s, &l)) {
        if (!l.heading) continue;
        count[passage_heading_level(l.text)]++;
        Str shown = str_clip_utf8(l.text, PASSAGE_OUTLINE_SHOW);
        bytes_all +=
            (size_t)snprintf(entry, sizeof entry, "%u: %.*s\n",
                             (unsigned)l.number, (i32)shown.n, shown.p);
    }
    size_t all = 0, top = 0;
    for (size_t lv = 1; lv <= 6; lv++) {
        all += count[lv];
        if (lv <= 3) top += count[lv];
    }
    if (!all) return 0;
    size_t max_level =
        (all <= PASSAGE_OUTLINE_ENTRIES && bytes_all <= budget) || !top ? 6 : 3;

    enum { MORE_NOTE = 40 };
    size_t written = 0, used = 0;
    s = (PassageScan){0};
    s.body = body;
    while (passage_scan_line(&s, &l)) {
        if (!l.heading || passage_heading_level(l.text) > max_level) continue;
        if (written == PASSAGE_OUTLINE_ENTRIES) break;
        Str shown = str_clip_utf8(l.text, PASSAGE_OUTLINE_SHOW);
        i32 w = snprintf(entry, sizeof entry, "%u: %.*s\n", (unsigned)l.number,
                         (i32)shown.n, shown.p);
        if (w < 0 || (size_t)w >= sizeof entry) break;
        if (used + (size_t)w + MORE_NOTE > budget) break;
        buf_put(out, entry, (size_t)w);
        used += (size_t)w;
        written++;
    }
    if (written < all) {
        i32 w = snprintf(entry, sizeof entry, "[%zu more headings]\n",
                         all - written);
        if (w > 0 && used + (size_t)w <= budget) buf_put(out, entry, (size_t)w);
    }
    return written;
}
