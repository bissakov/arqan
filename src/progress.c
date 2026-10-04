/* The run ledger: what the harness knows about the work since the user's
 * last message, reported to the model as facts it did not write.
 *
 * Two outlets. A note appended to a tool result when one is due, which sits
 * at the end of the conversation and so leaves the prompt cache alone. And
 * the sections a checkpoint carries that the model does not write: the
 * user's requests in their own words, and a line about the run.
 */
#include "agent.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    i64 started;
    size_t calls, failed, compactions, notes;
    size_t todo_done, todo_total, todo_done_at;
    b8 todo_seen, due;
    size_t named;
    u64 fail_hash[AGENT_PROGRESS_FAIL_SLOTS];
    u32 fail_count[AGENT_PROGRESS_FAIL_SLOTS];
    char fail_cmd[AGENT_PROGRESS_FAIL_SLOTS][AGENT_PROGRESS_CMD_SHOWN];
    u8 fail_cmd_n[AGENT_PROGRESS_FAIL_SLOTS];
    b8 fail_cut[AGENT_PROGRESS_FAIL_SLOTS];
} ProgressRun;

/* INVARIANT: progress_run_begin assigns this whole, so a new field resets
 * with the run by construction. */
static ProgressRun g_progress;

#define PROGRESS_NONE ((size_t)-1)

void progress_run_begin(i64 sent_at) {
    g_progress = (ProgressRun){.started = sent_at, .named = PROGRESS_NONE};
}

static size_t progress_fail_slot(u64 hash) {
    size_t lowest = 0;
    for (size_t i = 0; i < AGENT_PROGRESS_FAIL_SLOTS; i++) {
        if (g_progress.fail_count[i] && g_progress.fail_hash[i] == hash)
            return i;
        if (g_progress.fail_count[i] < g_progress.fail_count[lowest])
            lowest = i;
    }
    g_progress.fail_count[lowest] = 0;
    g_progress.fail_hash[lowest] = hash;
    return lowest;
}

static void progress_fail_name(size_t slot, Str cmd) {
    cmd = str_trim(cmd);
    Str shown = str_clip_utf8(cmd, AGENT_PROGRESS_CMD_SHOWN);
    for (size_t i = 0; i < shown.n; i++) {
        char c = shown.p[i];
        g_progress.fail_cmd[slot][i] =
            (u8)c < 0x20 || c == 0x7f ? (char)' ' : c;
    }
    g_progress.fail_cmd_n[slot] = (u8)shown.n;
    g_progress.fail_cut[slot] = shown.n < cmd.n;
}

static void progress_fail_command(Str args, Arena *scratch) {
    size_t mark = scratch->off;
    Str cmd = json_str(json_parse(scratch, args), STR("command"));
    if (cmd.n) {
        size_t slot = progress_fail_slot(str_hash64(cmd));
        if (!g_progress.fail_count[slot]) progress_fail_name(slot, cmd);
        u32 count = ++g_progress.fail_count[slot];
        if (count % AGENT_PROGRESS_REPEAT_FAILS == 0) {
            g_progress.named = slot;
            g_progress.due = true;
        }
    }
    scratch->off = mark;
}

void progress_record(Str tool, Str args, b8 ok, i32 exit_code, Arena *scratch) {
    g_progress.calls++;
    if (g_progress.calls % AGENT_PROGRESS_CALLS == 0) g_progress.due = true;
    if (ok && !exit_code) return;
    g_progress.failed++;
    if (str_eq(tool, STR("bash"))) progress_fail_command(args, scratch);
}

void progress_todo_done_count(size_t done, size_t total) {
    if (g_progress.todo_seen && done > g_progress.todo_done)
        g_progress.todo_done_at = g_progress.calls;
    g_progress.todo_seen = true;
    g_progress.todo_done = done;
    g_progress.todo_total = total;
}

void progress_compacted(void) {
    g_progress.compactions++;
    g_progress.due = true;
}

static i64 progress_minutes(void) {
    if (g_progress.started <= 0) return -1;
    i64 now = (i64)time(NULL);
    return now > g_progress.started ? (now - g_progress.started) / 60 : 0;
}

static size_t progress_worst(void) {
    if (g_progress.named != PROGRESS_NONE) return g_progress.named;
    size_t worst = 0;
    for (size_t i = 1; i < AGENT_PROGRESS_FAIL_SLOTS; i++)
        if (g_progress.fail_count[i] > g_progress.fail_count[worst]) worst = i;
    return g_progress.fail_count[worst] >= AGENT_PROGRESS_REPEAT_FAILS
               ? worst
               : PROGRESS_NONE;
}

#define PROGRESS_PREFIX "\n\n[progress: "

void progress_note(Buf *out) {
    if (!g_progress.due || !g_progress.calls) return;
    char note[AGENT_PROGRESS_NOTE_BYTES];
    size_t n = 0;
#define NOTE(...)                                                              \
    do {                                                                       \
        i32 w = snprintf(note + n, sizeof note - n, __VA_ARGS__);              \
        if (w > 0)                                                             \
            n +=                                                               \
                (size_t)w < sizeof note - n ? (size_t)w : sizeof note - n - 1; \
    } while (0)
    NOTE("%s", PROGRESS_PREFIX);
    i64 minutes = progress_minutes();
    if (minutes > 0) NOTE("%lld min and ", (long long)minutes);
    NOTE("%zu tool call%s since the user's last message", g_progress.calls,
         g_progress.calls == 1 ? "" : "s");
    if (g_progress.failed) NOTE(", %zu failed", g_progress.failed);
    if (g_progress.compactions)
        NOTE("; context compacted %zu time%s", g_progress.compactions,
             g_progress.compactions == 1 ? "" : "s");
    if (g_progress.todo_total) {
        NOTE("; %zu of %zu steps done", g_progress.todo_done,
             g_progress.todo_total);
        size_t since = g_progress.calls - g_progress.todo_done_at;
        if (g_progress.todo_done < g_progress.todo_total
            && since >= AGENT_PROGRESS_CALLS)
            NOTE(", none finished in the last %zu calls", since);
    }
    size_t worst = progress_worst();
    if (worst != PROGRESS_NONE)
        NOTE("; \"%.*s%s\" failed %u times", (i32)g_progress.fail_cmd_n[worst],
             g_progress.fail_cmd[worst],
             g_progress.fail_cut[worst] ? "..." : "",
             g_progress.fail_count[worst]);
    NOTE(". If an approach keeps failing, stop and report.]");
#undef NOTE
    if (note[n - 1] != ']') return;
    buf_put(out, note, n);
    g_progress.due = false;
    g_progress.named = PROGRESS_NONE;
    g_progress.notes++;
}

Str progress_note_strip(Str result) {
    if (!result.n || result.p[result.n - 1] != ']') return result;
    Str prefix = STR(PROGRESS_PREFIX);
    size_t window = result.n < AGENT_PROGRESS_NOTE_BYTES
                        ? result.n
                        : AGENT_PROGRESS_NOTE_BYTES;
    for (size_t i = 0; i < window; i++) {
        size_t at = result.n - 1 - i;
        if (result.p[at] != '\n' || !at || result.p[at - 1] != '\n') continue;
        if (str_starts(str_drop(result, at - 1), prefix))
            return (Str){result.p, at - 1};
    }
    return result;
}

void progress_telemetry(TelEvent *e) {
    if (!g_progress.calls && !g_progress.compactions) return;
    tel_int(e, "progress_calls", (i64)g_progress.calls);
    tel_int(e, "progress_failed", (i64)g_progress.failed);
    tel_int(e, "progress_compactions", (i64)g_progress.compactions);
    tel_int(e, "progress_notes", (i64)g_progress.notes);
}

/* ---- checkpoint sections ------------------------------------------------ */

#define REQUESTS_HEADING "## User requests"
#define RUN_HEADING      "## Run"

static b8 progress_last_heading(Str doc, Str heading, size_t *body) {
    size_t off = 0;
    Str line;
    b8 found = false;
    while (str_line(doc, &off, &line))
        if (str_eq(line, heading)) {
            *body = off;
            found = true;
        }
    return found;
}

static b8 progress_count_line(Str line, Str tail, size_t *n) {
    size_t digits = 0;
    while (digits < line.n && digits < 9 && line.p[digits] >= '0'
           && line.p[digits] <= '9')
        digits++;
    if (!digits || !str_eq(str_drop(line, digits), tail)) return false;
    b8 ok = false;
    i64 v = str_int(str_take(line, digits), &ok);
    if (!ok || v < 0) return false;
    *n = (size_t)v;
    return true;
}

size_t progress_requests_parse(Str checkpoint, Str *out, size_t cap,
                               size_t *dropped) {
    *dropped = 0;
    size_t off = 0, found = 0;
    if (!progress_last_heading(checkpoint, STR(REQUESTS_HEADING), &off))
        return 0;
    Str line, block = {0};
    for (;;) {
        b8 more = str_line(checkpoint, &off, &line);
        b8 quoted = more && line.n && line.p[0] == '>';
        if (quoted) {
            if (!block.p) block.p = line.p;
            block.n = (size_t)(line.p + line.n - block.p);
            continue;
        }
        if (block.p) {
            if (found < cap && out) out[found] = block;
            found++;
            block = (Str){0};
        }
        if (!more || str_starts(line, STR("## "))) break;
        size_t n = 0;
        if (progress_count_line(line, STR(" earlier requests were left out."),
                                &n)
            || progress_count_line(line, STR(" earlier request was left out."),
                                   &n))
            *dropped += n;
    }
    return found;
}

static u32 progress_prior_compactions(Str prior) {
    if (!prior.n) return 0;
    size_t off = 0;
    if (!progress_last_heading(prior, STR(RUN_HEADING), &off)) return 1;
    Str line;
    while (str_line(prior, &off, &line)) {
        if (!line.n) continue;
        Str head = STR("Compaction ");
        if (!str_starts(line, head)) return 1;
        Str rest = str_drop(line, head.n);
        size_t digits = 0;
        while (digits < rest.n && digits < 9 && rest.p[digits] >= '0'
               && rest.p[digits] <= '9')
            digits++;
        b8 ok = false;
        i64 v = digits ? str_int(str_take(rest, digits), &ok) : 0;
        return ok && v > 0 ? (u32)v : 1;
    }
    return 1;
}

void progress_line(Buf *out, u32 compaction) {
    buf_putf(out, "Compaction %u of this session.", compaction);
    i64 minutes = progress_minutes();
    if (minutes < 0) return;
    if (minutes == 0)
        buf_puts(out, STR(" The user's last message was less than a minute "
                          "ago"));
    else
        buf_putf(out, " The user's last message was %lld min ago",
                 (long long)minutes);
    if (g_progress.calls) {
        buf_putf(out, "; %zu tool call%s since then", g_progress.calls,
                 g_progress.calls == 1 ? "" : "s");
        if (g_progress.failed) buf_putf(out, ", %zu failed", g_progress.failed);
    }
    buf_putc(out, '.');
}

static size_t progress_quoted_size(Str text) {
    size_t n = 0, off = 0;
    Str line;
    while (str_line(text, &off, &line)) n += line.n ? line.n + 3 : 2;
    return n ? n - 1 : 1;
}

static void progress_quote(Buf *b, Str text) {
    size_t off = 0;
    Str line;
    b8 first = true;
    while (str_line(text, &off, &line)) {
        if (!first) buf_putc(b, '\n');
        first = false;
        buf_puts(b, line.n ? STR("> ") : STR(">"));
        buf_puts(b, line);
    }
    if (first) buf_putc(b, '>');
}

#define PROGRESS_OVERSIZE \
    "> [a request of %zu bytes was left out; the summary covers it]"

b8 progress_checkpoint(Buf *b, Str prior, const Str *fresh, size_t n_fresh,
                       Arena *scratch) {
    size_t prior_dropped = 0;
    size_t n_prior = progress_requests_parse(prior, NULL, 0, &prior_dropped);
    Str *quoted = n_prior ? arena_new(scratch, Str, n_prior) : NULL;
    if (n_prior && !quoted) return false;
    if (n_prior)
        progress_requests_parse(prior, quoted, n_prior, &prior_dropped);

    size_t total = n_prior + n_fresh, kept = 0, used = 0;
    for (size_t k = total; k-- > 0;) {
        Str text = k < n_prior ? quoted[k] : fresh[k - n_prior];
        size_t size = k < n_prior ? text.n + 2 : progress_quoted_size(text) + 2;
        if (size > AGENT_CHECKPOINT_REQUESTS_BYTES) size = 80;
        if (used + size > AGENT_CHECKPOINT_REQUESTS_BYTES) break;
        used += size;
        kept++;
    }
    size_t dropped = prior_dropped + (total - kept);

    if (!buf_reserve(b, b->n + used + 512)) return false;
    buf_puts(b, STR("\n\n" REQUESTS_HEADING "\n\n"));
    if (dropped)
        buf_putf(b, "%zu earlier request%s left out.\n\n", dropped,
                 dropped == 1 ? " was" : "s were");
    if (!total) buf_puts(b, STR("None in this part of the conversation.\n\n"));
    for (size_t k = total - kept; k < total; k++) {
        b8 old = k < n_prior;
        Str text = old ? quoted[k] : fresh[k - n_prior];
        size_t size = old ? text.n : progress_quoted_size(text);
        if (size > AGENT_CHECKPOINT_REQUESTS_BYTES)
            buf_putf(b, PROGRESS_OVERSIZE, text.n);
        else if (old)
            buf_puts(b, text);
        else
            progress_quote(b, text);
        buf_puts(b, STR("\n\n"));
    }
    buf_puts(b, STR(RUN_HEADING "\n\n"));
    progress_line(b, progress_prior_compactions(prior) + 1);
    return buf_ok(b);
}
