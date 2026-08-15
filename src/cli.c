#include "agent.h"

#include <stdio.h>
#include <string.h>

static const char g_usage[] =
    "usage: " AGENT_NAME " [options] [prompt]\n"
    "\n"
    "A tiny terminal coding agent.\n"
    "\n"
    "options:\n"
    "  -p, --prompt TEXT   run one turn non-interactively and exit\n"
    "  -m, --model NAME    model to use\n"
    "  -u, --base-url URL  endpoint to talk to\n"
    "  -a, --api KIND      wire format it speaks: openai or anthropic\n"
    "  -k, --api-key KEY   API key\n"
    "  -s, --system TEXT   system prompt\n"
    "  -t, --max-tokens N  cap the tokens a single reply may use\n"
    "  -d, --disable-tools LIST\n"
    "                      comma separated tools the model may not call,\n"
    "                      e.g. bash,write,patch\n"
    "  -h, --help          print help\n"
    "  -v, --version       print version\n"
    "\n"
    "environment: every setting is " AGENT_ENV_PREFIX "<NAME>, e.g. "
    AGENT_ENV_PREFIX "BASE_URL,\n" AGENT_ENV_PREFIX "MODEL, "
    AGENT_ENV_PREFIX "API_KEY, " AGENT_ENV_PREFIX "API, "
    AGENT_ENV_PREFIX "MAX_TOKENS, " AGENT_ENV_PREFIX "STREAM,\n"
    AGENT_ENV_PREFIX "MODE, " AGENT_ENV_PREFIX "DISABLE_TOOLS, plus "
    AGENT_ENV_PREFIX "SYSTEM_PROMPT.\n"
    "Config: $XDG_CONFIG_HOME/" AGENT_NAME "/config.toml, overridden by\n"
    "$PWD/." AGENT_NAME "/config.toml.\n"
    "Without any of them, " AGENT_NAME " asks for a provider; /provider stores\n"
    "connections and /model picks a model from any of them.\n";

static void cli_bad(const char *fmt, const char *arg) {
    fprintf(stderr, AGENT_NAME ": ");
    fprintf(stderr, fmt, arg);
    fprintf(stderr, "\nTry '" AGENT_NAME " --help'.\n");
}

/* One option and the argv slot its value may come from. `attached` is the
 * text after "=" or after a short name, NULL when there was none. */
typedef struct {
    CliOpts *o;
    const char *name;      // as written, for diagnostics
    const char *attached;
    i32 *i;
    i32 argc;
    char **argv;
} CliArg;

static b8 cli_value(CliArg *a, Str *out) {
    const char *v = a->attached;
    if (!v) {
        if (*a->i + 1 >= a->argc) {
            cli_bad("option '%s' requires a value", a->name);
            return false;
        }
        v = a->argv[++*a->i];
    }
    *out = str_c(v);
    return true;
}

/* Returns false on a bad option, having reported it. `*done` is set when the
 * option printed something the caller should exit on. */
static b8 cli_option(CliArg *a, char c, const char *lng, b8 *done) {
    CliOpts *o = a->o;
#ifdef AGENT_TESTING
    /* Not part of OPT: a short name of '\0' would match every long option. */
    if (lng && !strcmp(lng, "ca-trust")) {
        http_print_ca_trust();
        *done = true;
        return true;
    }
#endif
#define OPT(sc, ln) (c == (sc) || (lng && !strcmp(lng, (ln))))
    if (OPT('h', "help")) { fputs(g_usage, stdout); *done = true; return true; }
    if (OPT('v', "version")) { printf(AGENT_NAME " %s\n", AGENT_VERSION); *done = true; return true; }
    if (OPT('p', "prompt")) { o->have_prompt = true; return cli_value(a, &o->prompt); }
    if (OPT('m', "model")) return cli_value(a, &o->model);
    if (OPT('u', "base-url")) return cli_value(a, &o->base_url);
    if (OPT('a', "api")) {
        Str v;
        if (!cli_value(a, &v)) return false;
        /* A name nothing speaks is a typo worth reporting: taking the default
         * would send the wrong shape to the endpoint it was meant to fix. */
        if (!str_eq(v, STR("openai")) && !str_eq(v, STR("anthropic"))) {
            cli_bad("option '%s' wants openai or anthropic", a->name);
            return false;
        }
        o->api = v;
        return true;
    }
    if (OPT('k', "api-key")) return cli_value(a, &o->api_key);
    if (OPT('s', "system")) return cli_value(a, &o->system_prompt);
    if (OPT('d', "disable-tools")) return cli_value(a, &o->disable_tools);
    if (OPT('t', "max-tokens")) {
        Str v;
        if (!cli_value(a, &v)) return false;
        b8 ok = false;
        i64 n = str_int(v, &ok);
        if (!ok || n < 1 || n > (1 << 20)) {
            cli_bad("option '%s' wants a token count between 1 and 1048576",
                    a->name);
            return false;
        }
        o->max_tokens = (i32)n;
        return true;
    }
#undef OPT
    cli_bad("unknown option '%s'", a->name);
    return false;
}

CliStatus cli_parse(i32 argc, char **argv, CliOpts *out) {
    memset(out, 0, sizeof *out);
    b8 no_more_options = false;

    for (i32 i = 1; i < argc; i++) {
        char *arg = argv[i];
        CliArg a = { out, arg, NULL, &i, argc, argv };
        b8 done = false;

        if (no_more_options || arg[0] != '-' || arg[1] == '\0') {
            if (out->have_prompt) {
                cli_bad("unexpected argument '%s'", arg);
                return CLI_ERROR;
            }
            out->have_prompt = true;
            out->prompt = str_c(arg);
            continue;
        }
        if (!strcmp(arg, "--")) { no_more_options = true; continue; }

        if (arg[1] == '-') {
            char name[64];
            const char *eq = strchr(arg + 2, '=');
            size_t n = eq ? (size_t)(eq - (arg + 2)) : strlen(arg + 2);
            if (n >= sizeof name) { cli_bad("unknown option '%s'", arg); return CLI_ERROR; }
            memcpy(name, arg + 2, n);
            name[n] = '\0';
            a.attached = eq ? eq + 1 : NULL;
            if (!cli_option(&a, '\0', name, &done)) return CLI_ERROR;
        } else {
            /* Short options do not cluster: every one of them takes a value
             * except -h and -v, so "-hv" would only ever be a typo. */
            a.attached = arg[2] ? arg + 2 : NULL;
            if (!cli_option(&a, arg[1], NULL, &done)) return CLI_ERROR;
        }
        if (done) return CLI_DONE;
    }
    return CLI_RUN;
}

void cli_apply(const CliOpts *o, Config *c) {
    if (o->model.p)         c->model = o->model;
    if (o->base_url.p)      { c->base_url = o->base_url; c->base_url_set = true; }
    if (o->api.p)           c->api = api_from_str(o->api);
    if (o->api_key.p)       c->api_key = o->api_key;
    if (o->system_prompt.p) c->system_prompt = o->system_prompt;
    if (o->disable_tools.p) c->disable_tools = o->disable_tools;
    if (o->max_tokens)      c->max_tokens = o->max_tokens;
}
