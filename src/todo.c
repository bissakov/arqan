#include "agent.h"

static struct {
    TodoList list;
    const char *src;
    size_t slot;
} g_todo;

Str todo_text(const TodoList *l, size_t i) {
    return (Str){l->text[i], l->len[i]};
}

size_t todo_active(const TodoList *l) {
    for (size_t i = 0; i < l->n; i++)
        if (l->status[i] == TODO_ACTIVE) return i;
    return AGENT_TODO_NONE;
}

size_t todo_done(const TodoList *l) {
    size_t n = 0;
    for (size_t i = 0; i < l->n; i++)
        if (l->status[i] == TODO_DONE) n++;
    return n;
}

static b8 todo_status_parse(Str s, u8 *out) {
    if (str_eq(s, STR("pending"))) {
        *out = TODO_PENDING;
    } else if (str_eq(s, STR("in_progress"))) {
        *out = TODO_ACTIVE;
    } else if (str_eq(s, STR("done"))) {
        *out = TODO_DONE;
    } else {
        return false;
    }
    return true;
}

static b8 todo_add(TodoList *l, Str text, u8 status) {
    if (l->n >= AGENT_MAX_TODOS || !text.n || text.n > AGENT_MAX_TODO_TEXT)
        return false;
    memcpy(l->text[l->n], text.p, text.n);
    l->len[l->n] = (u8)text.n;
    l->status[l->n] = status;
    l->n++;
    return true;
}

b8 todo_parse(Str args_json, Arena *scratch, TodoList *out, char *err,
              size_t err_cap) {
    JVal *j = json_parse_error(scratch, args_json, err, err_cap);
    if (!j) return false;

    const JVal *items = json_get(j, STR("items"));
    if (!items || items->type != J_ARR) {
        snprintf(err, err_cap, "missing items array");
        return false;
    }
    if (items->u.arr.n > AGENT_MAX_TODOS) {
        snprintf(err, err_cap, "%zu items, limit %d: send fewer, larger steps",
                 items->u.arr.n, AGENT_MAX_TODOS);
        return false;
    }

    TodoList l = {0};
    size_t active = 0;
    for (size_t i = 0; i < items->u.arr.n; i++) {
        const JVal *it = json_at(items, i);
        if (!it || it->type != J_OBJ) {
            snprintf(err, err_cap, "item %zu is not an object", i + 1);
            return false;
        }
        Str text = str_trim(json_str(it, STR("text")));
        if (!text.n) {
            snprintf(err, err_cap, "item %zu has no text", i + 1);
            return false;
        }
        if (text.n > AGENT_MAX_TODO_TEXT) {
            snprintf(err, err_cap, "item %zu is %zu bytes, limit %d", i + 1,
                     text.n, AGENT_MAX_TODO_TEXT);
            return false;
        }
        u8 status;
        Str s = json_str(it, STR("status"));
        if (!todo_status_parse(s, &status)) {
            snprintf(err, err_cap,
                     "item %zu has status \"%.*s\": use pending, in_progress "
                     "or done",
                     i + 1, (int)s.n, s.p);
            return false;
        }
        if (!todo_add(&l, text, status)) {
            snprintf(err, err_cap, "item %zu does not fit the list", i + 1);
            return false;
        }
        if (status == TODO_ACTIVE) active++;
    }
    if (active > 1) {
        snprintf(err, err_cap, "%zu items are in_progress, work on one",
                 active);
        return false;
    }
    *out = l;
    return true;
}

/* The list as todo_write_md left it in a compaction checkpoint, which is the
 * only carrier once the call that held it has been summarized away. */
b8 todo_parse_md(Str doc, TodoList *out) {
    TodoList l = {0};
    size_t off = 0;
    Str line;
    b8 in_list = false;
    while (str_line(doc, &off, &line)) {
        if (str_eq(str_trim(line), STR("## Step list"))) {
            in_list = true;
            continue;
        }
        if (!in_list) continue;
        b8 done = str_starts(line, STR("- [x] "));
        if (!done && !str_starts(line, STR("- [ ] "))) {
            if (l.n) break;
            continue;
        }
        Str text = str_trim(str_drop(line, 6));
        u8 status = done ? TODO_DONE : TODO_PENDING;
        Str tail = STR(" (in progress)");
        if (!done && text.n > tail.n
            && str_eq(str_drop(text, text.n - tail.n), tail)) {
            text = str_trim(str_take(text, text.n - tail.n));
            status = TODO_ACTIVE;
        }
        if (!todo_add(&l, text, status)) return false;
    }
    if (!l.n) return false;
    *out = l;
    return true;
}

static void todo_count(Buf *b, size_t n, const char *what, b8 *first) {
    if (!n) return;
    buf_puts(b, *first ? STR(": ") : STR(", "));
    *first = false;
    buf_putf(b, "%zu %s", n, what);
}

void todo_summary(Buf *b, const TodoList *l) {
    if (!l->n) {
        buf_puts(b, STR("list cleared"));
        return;
    }
    size_t done = todo_done(l);
    size_t active = todo_active(l) == AGENT_TODO_NONE ? 0 : 1;
    buf_putf(b, "%zu todo%s", l->n, l->n == 1 ? "" : "s");
    b8 first = true;
    todo_count(b, done, "done", &first);
    todo_count(b, active, "in progress", &first);
    todo_count(b, l->n - done - active, "pending", &first);
}

void todo_write_md(Buf *b, const TodoList *l) {
    for (size_t i = 0; i < l->n; i++) {
        buf_puts(b, l->status[i] == TODO_DONE ? STR("- [x] ") : STR("- [ ] "));
        buf_puts(b, todo_text(l, i));
        if (l->status[i] == TODO_ACTIVE) buf_puts(b, STR(" (in progress)"));
        buf_putc(b, '\n');
    }
}

const TodoList *todo_current(void) {
    return &g_todo.list;
}

static void todo_publish(void) {
    tui_set_todo(todo_done(&g_todo.list), g_todo.list.n);
}

void todo_clear(void) {
    g_todo.list.n = 0;
    g_todo.src = NULL;
    g_todo.slot = AGENT_TODO_NONE;
    todo_publish();
}

b8 todo_run(Str args_json, Arena *scratch, Buf *out, char *err,
            size_t err_cap) {
    TodoList l;
    if (!todo_parse(args_json, scratch, &l, err, err_cap)) return false;
    g_todo.list = l;
    /* INVARIANT: Conv does not hold this call yet, so the source is left
     * unset and the next todo_sync rederives from the recorded one. */
    g_todo.src = NULL;
    g_todo.slot = AGENT_TODO_NONE;
    todo_publish();
    todo_summary(out, &l);
    return buf_ok(out);
}

/* The newest slot that can state the list: a todo call, or the compaction
 * checkpoint that outlived one. The scan stops at a checkpoint either way,
 * since nothing before it survived the cut. */
static size_t todo_source(const Conv *c, b8 *from_md) {
    for (size_t i = c->n; i > 0; i--) {
        size_t s = i - 1;
        if (conv_is_call(c, s)) {
            if (str_eq(c->tool_name[s], STR("todo"))) {
                *from_md = false;
                return s;
            }
            continue;
        }
        if (c->role[s] == M_USER
            && str_starts(c->text[s], STR("# Context checkpoint"))) {
            *from_md = true;
            return s;
        }
    }
    return AGENT_TODO_NONE;
}

void todo_sync(const Conv *c, Arena *scratch) {
    b8 from_md = false;
    size_t slot = todo_source(c, &from_md);
    if (slot == AGENT_TODO_NONE) {
        todo_clear();
        return;
    }
    if (slot == g_todo.slot && c->text[slot].p == g_todo.src) return;

    size_t mark = scratch->off;
    char err[AGENT_TOOL_ERR];
    TodoList l;
    b8 ok = from_md ? todo_parse_md(c->text[slot], &l)
                    : todo_parse(c->text[slot], scratch, &l, err, sizeof err);
    if (ok) {
        g_todo.list = l;
        g_todo.slot = slot;
        g_todo.src = c->text[slot].p;
        todo_publish();
    } else {
        todo_clear();
    }
    scratch->off = mark;
}
