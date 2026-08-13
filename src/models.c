#include "agent.h"

#include <stdio.h>
#include <string.h>

#define MODEL_SECTION_PREFIX STR("providers.")

/* A model id is quoted in the section because ids routinely contain '/',
 * ':', and '.'. The settings reader compares section text literally, so the
 * same escaping used in the file is used for lookup. */
static Str model_section(Str provider, Str model, Arena *a) {
    if (!endpoint_name_ok(provider) || !model.n
        || model.n > AGENT_MAX_MODEL_NAME) return (Str){0};
    Buf b;
    buf_init(&b, a, MODEL_SECTION_PREFIX.n + provider.n + model.n + 16);
    buf_puts(&b, MODEL_SECTION_PREFIX);
    buf_puts(&b, provider);
    buf_puts(&b, STR(".models.\""));
    for (size_t i = 0; i < model.n; i++) {
        u8 ch = (u8)model.p[i];
        if (ch == '"' || ch == '\\') {
            buf_putc(&b, '\\');
            buf_putc(&b, (char)ch);
        } else if (ch < 0x20 || ch == 0x7f) {
            static const char hex[] = "0123456789abcdef";
            buf_puts(&b, STR("\\u00"));
            buf_putc(&b, hex[ch >> 4]);
            buf_putc(&b, hex[ch & 0x0f]);
        } else {
            buf_putc(&b, (char)ch);
        }
    }
    buf_putc(&b, '"');
    return buf_ok(&b) ? buf_finish(&b) : (Str){0};
}

static b8 model_list_ok(Str list, b8 budgets) {
    if (list.n > AGENT_MAX_REASONING_LIST) return false;
    size_t off = 0, count = 0;
    while (off < list.n) {
        size_t end = off;
        while (end < list.n && list.p[end] != ',') end++;
        Str item = str_trim((Str){ list.p + off, end - off });
        if (!item.n || ++count > AGENT_MAX_ENDPOINTS) return false;
        if (budgets) {
            b8 ok = false;
            i64 n = str_int(item, &ok);
            if (!ok || n <= 0) return false;
        }
        for (size_t prev = 0; prev < off;) {
            size_t pend = prev;
            while (pend < list.n && list.p[pend] != ',') pend++;
            if (str_eq(item, str_trim((Str){ list.p + prev, pend - prev })))
                return false;
            prev = pend + 1;
        }
        off = end + 1;
    }
    return true;
}

static b8 model_selected_ok(Str list, Str selected) {
    if (!selected.n) return true;
    for (size_t off = 0; off < list.n;) {
        size_t end = off;
        while (end < list.n && list.p[end] != ',') end++;
        if (str_eq(selected, str_trim((Str){ list.p + off, end - off })))
            return true;
        off = end + 1;
    }
    return false;
}

static void model_profile_take(ModelProfile *p, const Settings *s, Str sec,
                               Arena *out) {
    for (size_t i = 0; i < s->n; i++) {
        if (!str_eq(s->section[i], sec)) continue;
        p->configured = true;
        Str key = s->key[i], v = s->val[i];
        if (str_eq(key, STR("reasoning_efforts")))
            p->reasoning_efforts = str_dup_opt(out, v);
        else if (str_eq(key, STR("thinking_budgets")))
            p->thinking_budgets = str_dup_opt(out, v);
        else if (str_eq(key, STR("reasoning_effort")))
            p->reasoning_effort = str_dup_opt(out, v);
        else if (str_eq(key, STR("thinking_budget")))
            p->thinking_budget = str_dup_opt(out, v);
        else if (str_eq(key, STR("reasoning_template")))
            p->reasoning_template = str_dup_opt(out, v);
        else if (str_eq(key, STR("context_window"))) {
            p->context_window = 0;
            b8 ok = false;
            i64 n = str_int(v, &ok);
            if (ok && n > 0 && (u64)n <= (u64)AGENT_MAX_CONTEXT_WINDOW)
                p->context_window = (size_t)n;
        }
    }
}

void model_profile_load(ModelProfile *p, Str provider, Str model, Arena *out,
                        Arena *scratch) {
    memset(p, 0, sizeof *p);
    Str section = model_section(provider, model, scratch);
    if (!section.n) return;
    Str files[AGENT_MAX_CONFIG_FILES + AGENT_MAX_PROJECT_FILES];
    size_t n = paths_config_files(AGENT_CONFIG_NAME, scratch, files,
                                  AGENT_MAX_CONFIG_FILES);
    n += paths_project_files(AGENT_CONFIG_NAME, scratch, files + n,
                             AGENT_MAX_PROJECT_FILES);
    for (size_t i = 0; i < n; i++) {
        Settings s;
        if (settings_load(&s, files[i], scratch))
            model_profile_take(p, &s, section, out);
    }
    if (!model_list_ok(p->reasoning_efforts, false)
        || !model_list_ok(p->thinking_budgets, true)
        || !model_selected_ok(p->reasoning_efforts, p->reasoning_effort)
        || !model_selected_ok(p->thinking_budgets, p->thinking_budget)
        || p->reasoning_template.n > AGENT_MAX_REASONING_TEMPLATE)
        *p = (ModelProfile){0};
}

b8 model_profile_save(Str provider, Str model, const ModelProfile *p,
                      Arena *scratch) {
    if (!p || !model_list_ok(p->reasoning_efforts, false)
        || !model_list_ok(p->thinking_budgets, true)
        || !model_selected_ok(p->reasoning_efforts, p->reasoning_effort)
        || !model_selected_ok(p->thinking_budgets, p->thinking_budget)
        || p->reasoning_template.n > AGENT_MAX_REASONING_TEMPLATE
        || p->context_window > AGENT_MAX_CONTEXT_WINDOW) return false;
    size_t mark = scratch->off;
    Str dir = paths_dir(AGENT_DIR_CONFIG, scratch);
    Str path = paths_file(AGENT_DIR_CONFIG, AGENT_CONFIG_NAME, scratch);
    Str section = model_section(provider, model, scratch);
    char window[32] = {0};
    if (p->context_window)
        snprintf(window, sizeof window, "%zu", p->context_window);
    Str keys[6] = { STR("context_window"), STR("reasoning_efforts"),
        STR("thinking_budgets"), STR("reasoning_effort"),
        STR("thinking_budget"), STR("reasoning_template") };
    Str vals[6] = { str_c(window), p->reasoning_efforts, p->thinking_budgets,
        p->reasoning_effort, p->thinking_budget, p->reasoning_template };
    b8 ok = dir.n && path.n && section.n && paths_ensure_dir(dir)
         && settings_set(path, section, keys, vals, 6, 0600, scratch);
    scratch->off = mark;
    return ok;
}

b8 model_profiles_delete(Str provider, Arena *scratch) {
    if (!endpoint_name_ok(provider)) return false;
    size_t mark = scratch->off;
    Str path = paths_file(AGENT_DIR_CONFIG, AGENT_CONFIG_NAME, scratch);
    Buf prefix;
    buf_init(&prefix, scratch, MODEL_SECTION_PREFIX.n + provider.n + 9);
    buf_puts(&prefix, MODEL_SECTION_PREFIX);
    buf_puts(&prefix, provider);
    buf_puts(&prefix, STR(".models."));
    if (!path.n || !buf_ok(&prefix)) { scratch->off = mark; return false; }
    Settings s;
    if (!settings_load(&s, path, scratch)) {
        scratch->off = mark;
        return true;
    }
    Str sections[AGENT_MAX_SETTINGS];
    size_t n = settings_sections(&s, buf_finish(&prefix), sections,
                                 AGENT_MAX_SETTINGS);
    b8 ok = true;
    for (size_t i = 0; i < n; i++)
        if (!settings_remove_section(path, sections[i], scratch)) ok = false;
    scratch->off = mark;
    return ok;
}
