/* ---- read-only shell commands ----
 * A command is read-only when every segment of its pipeline starts with a
 * program from the list and nothing in it redirects, substitutes, groups or
 * backgrounds. Such a command runs without approval and is the only kind plan
 * mode and a subagent may run.
 * A read-only command whose words name an absolute path, a `~` path, a `..`
 * component or a variable reads outside the project and asks like `read`
 * does. On Linux a read-only command also runs under Landlock with every
 * filesystem write and TCP denied, so the classifier is the first guard, not
 * the only one.
 * A repository's own config can name programs git runs, such as a textconv
 * or a clean filter. When the command is `contained`, the sandbox holds
 * them; when it is not, only the git subcommands that never run one stay
 * read-only.
 * NOTE: the path rule is textual. A path assembled at run time is caught by
 * the `$` rule; one hidden in a program's own script is not, and the sandbox
 * is what stops it.
 * INVARIANT: depends on core.c alone, so tests/fuzz/fuzz_classify.c can
 * include it directly.
 */

#include "agent.h"

#include <ctype.h>
#include <string.h>

static const char *const k_read_only_programs[] = {
    "[",    "awk",      "basename", "cat",  "cd",   "column", "cut",
    "date", "diff",     "dirname",  "du",   "echo", "file",   "find",
    "grep", "head",     "jq",       "ls",   "nl",   "od",     "printf",
    "pwd",  "readlink", "realpath", "rg",   "sed",  "seq",    "sha256sum",
    "sort", "stat",     "strings",  "tac",  "tail", "test",   "tr",
    "tree", "true",     "type",     "uniq", "wc",   "which",  "xxd",
};
static const char *const k_read_only_git[] = {
    "blame",    "describe",  "diff",     "grep", "log",
    "ls-files", "rev-parse", "shortlog", "show", "status",
};
static const char *const k_read_only_git_without_drivers[] = {
    "ls-files",
    "rev-parse",
    "shortlog",
};
static const char *const k_find_writes[] = {
    "-delete", "-exec",    "-execdir", "-ok",  "-okdir",
    "-fprint", "-fprint0", "-fprintf", "-fls",
};
static const char *const k_harmless_redirects[] = {
    "2>&1", "2>/dev/null", ">/dev/null", "1>/dev/null", "&>/dev/null",
};
static const char *const k_sed_long_options[] = {
    "--quiet",    "--silent", "--regexp-extended", "--null-data",
    "--separate", "--posix",  "--debug",
};
static const char *const k_awk_option_prefixes[] = {
    "-F", "-v", "-e", "--field-separator", "--assign", "--source",
};
static const char *const k_awk_options[] = {"--posix", "--traditional", "--"};
static const char *const k_xxd_valued[] = {
    "-c", "-cols", "-l", "-len", "-s", "-seek", "-g", "-o",
};

#define SHELL_SEGMENT_WORDS 64
#define LIST_N(a)           (sizeof(a) / sizeof *(a))

static Str shell_unquoted(Str w) {
    if (w.n >= 2 && (w.p[0] == '\'' || w.p[0] == '"') && w.p[w.n - 1] == w.p[0])
        return (Str){w.p + 1, w.n - 2};
    return w;
}

static b8 word_is(Str w, const char *z) {
    return str_eq(shell_unquoted(w), str_c(z));
}

static b8 word_in(Str w, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (word_is(w, list[i])) return true;
    return false;
}

static b8 word_starts_any(Str w, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (str_starts(w, str_c(list[i]))) return true;
    return false;
}

static b8 str_has(Str s, Str needle) {
    for (size_t i = 0; i + needle.n <= s.n; i++)
        if (memcmp(s.p + i, needle.p, needle.n) == 0) return true;
    return false;
}

static b8 option_abbreviates(Str v, const char *full) {
    const char *eq = memchr(v.p, '=', v.n);
    Str name = {v.p, eq ? (size_t)(eq - v.p) : v.n};
    return name.n >= 3 && str_starts(str_c(full), name);
}

static b8 option_is_short(Str v) {
    return v.n > 1 && v.p[0] == '-' && v.p[1] != '-';
}

static b8 short_option_has(Str v, char flag) {
    return option_is_short(v) && memchr(v.p + 1, flag, v.n - 1) != NULL;
}

/* A word is outside the project when it, or the value after its first `=`,
 * starts with `/` or an unquoted `~`, or has a `..` component. Quotes are
 * skipped since they do not change a path. */
static b8 word_outside(Str w) {
    if (str_eq(shell_unquoted(w), STR("/dev/null"))) return false;
    char quote = 0, prev = 0;
    size_t component = 0, dots = 0;
    for (size_t i = 0; i <= w.n; i++) {
        char c = i < w.n ? w.p[i] : 0;
        if (c == '\'' || c == '"') {
            if (quote == c)
                quote = 0;
            else if (!quote)
                quote = c;
            continue;
        }
        if (c == '\\' && quote != '\'' && i + 1 < w.n) {
            i++;
            component++;
            prev = w.p[i];
            continue;
        }
        b8 at_path_start = prev == 0 || prev == '=';
        if (c == '/' && at_path_start) return true;
        if (c == '~' && !quote && at_path_start) return true;
        if (c == '/' || c == '=' || c == 0) {
            if (component == 2 && dots == 2) return true;
            component = 0;
            dots = 0;
        } else {
            component++;
            if (c == '.') dots++;
        }
        prev = c;
    }
    return false;
}

/* ---- sed scripts ----
 * A walker over the commands of a sed script. It skips addresses, then checks
 * the command letter: `e`, `w` and `W` write or run, as do the `e` and `w`
 * flags of `s`. Anything it does not understand is a write. */

static b8 sed_skip_delimited(Str s, size_t *i, char delim) {
    while (*i < s.n) {
        char c = s.p[*i];
        if (c == '\\' && *i + 1 < s.n) {
            *i += 2;
            continue;
        }
        (*i)++;
        if (c == delim) return true;
    }
    return false;
}

static void sed_skip_spaces(Str s, size_t *i) {
    while (*i < s.n && (s.p[*i] == ' ' || s.p[*i] == '\t')) (*i)++;
}

static void sed_skip_digits(Str s, size_t *i) {
    while (*i < s.n && isdigit((unsigned char)s.p[*i])) (*i)++;
}

static b8 sed_skip_address(Str s, size_t *i, b8 *ok) {
    if (*i >= s.n) return false;
    char c = s.p[*i];
    if (isdigit((unsigned char)c)) {
        sed_skip_digits(s, i);
    } else if (c == '$' || c == '+' || c == '~') {
        (*i)++;
        sed_skip_digits(s, i);
    } else if (c == '/' || c == '\\') {
        char delim = c;
        if (c == '\\') {
            if (*i + 1 >= s.n) {
                *ok = false;
                return false;
            }
            delim = s.p[*i + 1];
            (*i)++;
        }
        (*i)++;
        if (!sed_skip_delimited(s, i, delim)) {
            *ok = false;
            return false;
        }
        while (*i < s.n && (s.p[*i] == 'I' || s.p[*i] == 'M')) (*i)++;
    } else {
        return false;
    }
    return true;
}

static b8 sed_skip_addresses(Str s, size_t *i) {
    b8 ok = true;
    if (!sed_skip_address(s, i, &ok)) return ok;
    sed_skip_spaces(s, i);
    if (*i < s.n && (s.p[*i] == ',' || s.p[*i] == '~')) {
        (*i)++;
        sed_skip_spaces(s, i);
        if (!sed_skip_address(s, i, &ok)) return false;
    }
    return ok;
}

static void sed_skip_line(Str s, size_t *i) {
    while (*i < s.n && s.p[*i] != '\n') (*i)++;
}

static void sed_skip_label(Str s, size_t *i) {
    while (*i < s.n && !strchr("\n;", s.p[*i])) (*i)++;
}

static b8 sed_substitute_read_only(Str s, size_t *i) {
    if (*i >= s.n) return false;
    char delim = s.p[(*i)++];
    if (delim == '\n' || delim == '\\') return false;
    if (!sed_skip_delimited(s, i, delim) || !sed_skip_delimited(s, i, delim))
        return false;
    while (*i < s.n && !strchr("\n;} \t", s.p[*i])) {
        char f = s.p[(*i)++];
        if (!strchr("gpiImM0123456789", f)) return false;
    }
    return true;
}

static b8 sed_script_read_only(Str s, b8 *outside) {
    size_t i = 0;
    while (i < s.n) {
        char c = s.p[i];
        if (strchr(" \t\n;{}", c)) {
            i++;
            continue;
        }
        if (!sed_skip_addresses(s, &i)) return false;
        sed_skip_spaces(s, &i);
        while (i < s.n && (s.p[i] == '!' || s.p[i] == ' ')) i++;
        if (i >= s.n) return false;
        char cmd = s.p[i++];
        if (cmd == '{') continue;
        switch (cmd) {
            case 'e':
            case 'w':
            case 'W': return false;
            case 's':
                if (!sed_substitute_read_only(s, &i)) return false;
                break;
            case 'y': {
                if (i >= s.n) return false;
                char delim = s.p[i++];
                if (!sed_skip_delimited(s, &i, delim)
                    || !sed_skip_delimited(s, &i, delim))
                    return false;
                break;
            }
            case 'a':
            case 'i':
            case 'c':
                sed_skip_line(s, &i);
                while (i < s.n && s.p[i - 1] == '\\') {
                    i++;
                    sed_skip_line(s, &i);
                }
                break;
            case 'r':
            case 'R': {
                sed_skip_spaces(s, &i);
                size_t from = i;
                sed_skip_line(s, &i);
                if (word_outside((Str){s.p + from, i - from})) *outside = true;
                break;
            }
            case '#': sed_skip_line(s, &i); break;
            case ':':
            case 'b':
            case 't':
            case 'T':
            case 'v': sed_skip_label(s, &i); break;
            case 'l':
            case 'q':
            case 'Q':
                sed_skip_spaces(s, &i);
                sed_skip_digits(s, &i);
                break;
            case 'd':
            case 'D':
            case 'g':
            case 'G':
            case 'h':
            case 'H':
            case 'n':
            case 'N':
            case 'p':
            case 'P':
            case 'x':
            case 'z':
            case '=':
            case 'F':
            case '}': break;
            default: return false;
        }
        sed_skip_spaces(s, &i);
        if (i < s.n && !strchr("\n;}#", s.p[i])) return false;
    }
    return true;
}

static b8 shell_sed_read_only(const Str *arg, size_t args, b8 *outside) {
    b8 have_script = false;
    for (size_t a = 0; a < args; a++) {
        Str v = shell_unquoted(arg[a]);
        if (v.n > 1 && v.p[0] == '-' && v.p[1] == '-') {
            if (word_in(arg[a], k_sed_long_options, LIST_N(k_sed_long_options)))
                continue;
            Str script;
            if (str_eq(v, STR("--expression"))) {
                if (a + 1 >= args) return false;
                script = shell_unquoted(arg[++a]);
            } else if (str_starts(v, STR("--expression="))) {
                script = (Str){v.p + 13, v.n - 13};
            } else {
                return false;
            }
            have_script = true;
            if (!sed_script_read_only(script, outside)) return false;
        } else if (v.n > 1 && v.p[0] == '-') {
            for (size_t k = 1; k < v.n; k++) {
                char f = v.p[k];
                if (f == 'e') {
                    Str script;
                    if (k + 1 < v.n)
                        script = (Str){v.p + k + 1, v.n - k - 1};
                    else if (a + 1 < args)
                        script = shell_unquoted(arg[++a]);
                    else
                        return false;
                    have_script = true;
                    if (!sed_script_read_only(script, outside)) return false;
                    break;
                }
                if (!strchr("nErsz", f)) return false;
            }
        } else if (!have_script) {
            have_script = true;
            if (!sed_script_read_only(v, outside)) return false;
        } else if (word_outside(arg[a])) {
            *outside = true;
        }
    }
    return have_script;
}

/* The script is spared the path rule, since `/re/` is not a path. The ways a
 * script names a file of its own, `getline`, `ARGV` and `@include`, are
 * refused instead. */
static b8 shell_awk_read_only(const Str *arg, size_t args, b8 *outside) {
    b8 have_script = false;
    for (size_t a = 0; a < args; a++) {
        Str v = shell_unquoted(arg[a]);
        b8 script = false;
        if (v.n > 1 && v.p[0] == '-') {
            if (!word_starts_any(v, k_awk_option_prefixes,
                                 LIST_N(k_awk_option_prefixes))
                && !word_in(arg[a], k_awk_options, LIST_N(k_awk_options)))
                return false;
            b8 valued = str_eq(v, STR("-F")) || str_eq(v, STR("-v"))
                        || str_eq(v, STR("--field-separator"))
                        || str_eq(v, STR("--assign"));
            script = str_starts(v, STR("-e")) || str_starts(v, STR("--source"));
            if (script && (str_eq(v, STR("-e")) || str_eq(v, STR("--source"))))
                valued = true;
            if (valued) {
                if (a + 1 >= args) return false;
                v = shell_unquoted(arg[++a]);
            }
        } else if (!have_script) {
            script = true;
        }
        if (script) have_script = true;
        if (!script && word_outside(arg[a])) *outside = true;
        if (memchr(v.p, '>', v.n) || memchr(v.p, '|', v.n)
            || memchr(v.p, '@', v.n) || str_has(v, STR("system"))
            || str_has(v, STR("getline")) || str_has(v, STR("ARGV")))
            return false;
    }
    return true;
}

static b8 shell_xxd_read_only(const Str *arg, size_t args) {
    size_t positional = 0;
    for (size_t a = 0; a < args; a++) {
        Str v = shell_unquoted(arg[a]);
        if (str_starts(v, STR("-r"))) return false;
        if (v.n > 1 && v.p[0] == '-') {
            if (word_in(arg[a], k_xxd_valued, LIST_N(k_xxd_valued))) a++;
            continue;
        }
        if (++positional > 1) return false;
    }
    return true;
}

static b8 shell_git_read_only(const Str *w, size_t n, b8 contained) {
    size_t k = 0;
    while (k < n) {
        if (word_is(w[k], "-C") && k + 1 < n)
            k += 2;
        else if (word_is(w[k], "--no-pager") || word_is(w[k], "-P"))
            k++;
        else
            break;
    }
    if (k >= n || !word_in(w[k], k_read_only_git, LIST_N(k_read_only_git)))
        return false;
    if (!contained
        && !word_in(w[k], k_read_only_git_without_drivers,
                    LIST_N(k_read_only_git_without_drivers)))
        return false;
    for (size_t a = k + 1; a < n; a++) {
        Str v = shell_unquoted(w[a]);
        if (option_abbreviates(v, "--output")
            || option_abbreviates(v, "--open-files-in-pager")
            || option_abbreviates(v, "--ext-diff") || short_option_has(v, 'O'))
            return false;
    }
    return true;
}

static b8 shell_segment_read_only(const Str *w, size_t n, b8 contained,
                                  b8 *outside) {
    size_t k = 0;
    if (k + 2 < n && word_is(w[k], "timeout")) k += 2;
    if (k >= n) return false;
    for (size_t a = 0; a <= k; a++)
        if (word_outside(w[a])) *outside = true;
    Str prog = w[k];
    const Str *arg = w + k + 1;
    size_t args = n - k - 1;
    b8 scripted = word_is(prog, "sed") || word_is(prog, "awk");
    for (size_t a = 0; a < args; a++) {
        Str raw = arg[a];
        if (raw.n > 1 && raw.p[0] == '-'
            && (memchr(raw.p + 1, '\'', raw.n - 1)
                || memchr(raw.p + 1, '"', raw.n - 1)
                || memchr(raw.p + 1, '\\', raw.n - 1)))
            return false;
        if (!scripted && word_outside(raw)) *outside = true;
    }
    if (word_is(prog, "git")) return shell_git_read_only(arg, args, contained);
    if (!word_in(prog, k_read_only_programs, LIST_N(k_read_only_programs)))
        return false;
    if (word_is(prog, "sed")) return shell_sed_read_only(arg, args, outside);
    if (word_is(prog, "awk")) return shell_awk_read_only(arg, args, outside);
    if (word_is(prog, "xxd")) return shell_xxd_read_only(arg, args);
    for (size_t a = 0; a < args; a++) {
        Str v = shell_unquoted(arg[a]);
        if (word_is(prog, "find")
            && word_in(arg[a], k_find_writes, LIST_N(k_find_writes)))
            return false;
        if (word_is(prog, "sort")
            && (short_option_has(v, 'o') || option_abbreviates(v, "--output")
                || option_abbreviates(v, "--compress-program")))
            return false;
        if (word_is(prog, "rg")
            && (str_starts(v, STR("--pre"))
                || str_starts(v, STR("--hostname-bin"))))
            return false;
        if (word_is(prog, "tree") && short_option_has(v, 'o')) return false;
        if (word_is(prog, "file")
            && (short_option_has(v, 'C') || option_abbreviates(v, "--compile")))
            return false;
        if (word_is(prog, "date")
            && (str_starts(v, STR("-s")) || option_abbreviates(v, "--set")))
            return false;
    }
    return true;
}

ShellClass shell_classify(Str cmd, b8 contained) {
    Str word[SHELL_SEGMENT_WORDS];
    size_t n = 0, start = SIZE_MAX;
    b8 any = false;
    b8 outside = false;
    char quote = 0;
    for (size_t i = 0; i <= cmd.n; i++) {
        char c = i < cmd.n ? cmd.p[i] : '\n';
        if (quote) {
            if (c == quote) {
                quote = 0;
            } else if (quote == '"') {
                if (c == '\\')
                    i++;
                else if (c == '`')
                    return SHELL_WRITES;
                else if (c == '$') {
                    if (i + 1 < cmd.n && cmd.p[i + 1] == '(')
                        return SHELL_WRITES;
                    outside = true;
                }
            }
            continue;
        }
        b8 chain = c == '&' && i + 1 < cmd.n && cmd.p[i + 1] == '&';
        b8 separator = c == '\n' || c == ';' || c == '|' || chain;
        if (separator || c == ' ' || c == '\t') {
            if (start != SIZE_MAX) {
                if (n == SHELL_SEGMENT_WORDS) return SHELL_WRITES;
                word[n++] = (Str){cmd.p + start, i - start};
                start = SIZE_MAX;
            }
            if (!separator) continue;
            if (chain || (c == '|' && i + 1 < cmd.n && cmd.p[i + 1] == '|'))
                i++;
            if (n) {
                if (!shell_segment_read_only(word, n, contained, &outside))
                    return SHELL_WRITES;
                any = true;
                n = 0;
            }
            continue;
        }
        if (c == '`' || c == '(' || c == ')' || c == '{' || c == '}'
            || c == '<')
            return SHELL_WRITES;
        if (c == '$') {
            if (i + 1 < cmd.n && cmd.p[i + 1] == '(') return SHELL_WRITES;
            outside = true;
        }
        if (c == '>' || c == '&') {
            size_t from = start == SIZE_MAX ? i : start;
            size_t end = from;
            while (end < cmd.n && !strchr(" \t\n;|", cmd.p[end])) end++;
            Str redirect = {cmd.p + from, end - from};
            if (!word_in(redirect, k_harmless_redirects,
                         LIST_N(k_harmless_redirects)))
                return SHELL_WRITES;
            i = end - 1;
            start = SIZE_MAX;
            continue;
        }
        if (start == SIZE_MAX) start = i;
        if (c == '\\')
            i++;
        else if (c == '\'' || c == '"')
            quote = c;
    }
    if (quote || n || start != SIZE_MAX || !any) return SHELL_WRITES;
    return outside ? SHELL_READS_OUTSIDE : SHELL_READS_INSIDE;
}
