# yoke

A terminal AI coding agent written in plain C17, designed as a counterpoint to
Claude Code, Codex, OpenCode and Pi. It talks to any OpenAI-compatible
chat-completions endpoint, streams the reply into a fullscreen TUI, and gives
the model four tools: read, write, bash and edit.

## Design principles

- **C17, single translation unit (unity) build**, so full rebuilds stay under a second.
- **Structure-of-Arrays (SoA)** for hot data (messages, tool calls, tool registry).
- **No heap allocations at all in our code.** Everything lives in two static
  blocks wrapped in arenas; per-turn scratch is freed by resetting a pointer,
  and the only heap traffic is libcurl's own.
- **Every bound is checked.** Sizes derived from a provider stream, a config
  file or the filesystem are validated before they reach an allocation, and a
  full arena or a full conversation is reported rather than written past.
- **Minimal deps**: libc and libcurl. JSON, SSE, TUI and the tool runner are
  hand-written.

## Layout

```
src/
  yoke.h          single umbrella header (all types + fwd decls)
  core.c          arena, Str, Buf, log, time
  json.c          tiny arena JSON parser + serializer
  http.c          libcurl streaming POST (SSE) + plain GET
  paths.c         XDG base directory resolution
  history.c       prompt history, mirrored to the XDG state dir
  session.c       per-directory saved conversations (/resume)
  config.c        env + XDG config file loader
  prompt.c        system prompt: SYSTEM.md + AGENTS.md lookup, expansion
  cli.c           command line parsing, above the config in precedence
  provider.c      chat-completions streaming + reasoning and tool deltas
  tools.c         SoA tool registry + read/write/bash/edit tools
  tui.c           alternate-screen TUI, viewport, composer + raw input
  main.c          unity includes + main + agent loop
tests/
  run.py          test runner (Python 3, no third-party packages)
  harness/        terminal emulator + pty driver
  mockprovider/   dummy OpenAI-compatible provider (lorem ipsum, tool calls)
  cases/          the tests
  golden/         expected screen dumps
```

## Build & run

```
make
./bin/yoke
```

Set at least a base URL, model and API key:

```
export YOKE_BASE_URL=https://api.openai.com/v1
export YOKE_MODEL=gpt-4o-mini
export YOKE_API_KEY=sk-...
```

or put them in `$XDG_CONFIG_HOME/yoke/config`, by default
`~/.config/yoke/config`:

```
base_url=https://api.openai.com/v1
model=gpt-4o-mini
api_key=sk-...
max_tokens=4096
max_messages=4096      # conversation capacity; a full one is reported, not overrun
```

Environment variables win over every file, and a model picked with `/model`
wins over the files but not over `YOKE_MODEL`.

Command line options outrank all of it, being the most local statement about
one invocation:

```
yoke --help
yoke -m gpt-4o --base-url https://api.openai.com/v1
yoke -p "summarise src/tui.c"    # one turn, reply on stdout, then exit
yoke "summarise src/tui.c"       # the same: a bare argument is the prompt
```

A one-shot run prints the reply and nothing else, exiting nonzero when the
turn did not complete, so it composes with pipes and scripts.

### The system prompt

Write your own in `.yoke/SYSTEM.md` for one project, or in
`$XDG_CONFIG_HOME/yoke/SYSTEM.md` for every session. The project file wins
when both exist, and it is looked up from the working directory upwards, so a
subdirectory still gets the project's prompt. `--system` and
`YOKE_SYSTEM_PROMPT` outrank both; with none of them, a built-in prompt is
used. It is not a config key: a prompt is a document, not a setting.

What only startup knows goes in as a placeholder:

| placeholder | expands to |
| --- | --- |
| `{tools}` | one `- name: description` line per registered tool |
| `{cwd}` | the directory yoke was launched in |

```markdown
You are a terse reviewer for this C17 codebase.

Available tools:
{tools}
Current working directory: {cwd}

Run `make test` after touching src/.
```

So a prompt written once keeps describing the tools that exist now. Anything
else in braces is left exactly as written, `{"a": 1}` included. A SYSTEM.md
larger than 64 KiB (about 65k characters) is refused at startup with an error
naming the file: a truncated prompt would be a different prompt.

### Project context

`AGENTS.md` is the project's instructions rather than yours, so it does not
compete with the prompt: it is appended to whichever prompt won, `--system`
included. Every one from the working directory up to the root applies,
outermost first, because a subdirectory refines its parent instead of
replacing it. It is a document about the project, not a template, so braces
in it stay exactly as written and the same 64 KiB limit applies.

## Using it

Enter sends and Alt+Enter inserts a newline. Ctrl-C discards the draft or
cancels a running turn, Esc also cancels one, and Ctrl-D quits from an empty
composer. Up/Down recall past prompts, PageUp/PageDown and the mouse wheel
scroll the transcript, and Ctrl-L repaints. Dragging selects any text on
screen and copies it over OSC 52 on release, so it works over ssh; Shift falls
back to the terminal's own selection.

Typing `/` opens a completion popup: `/clear` starts a fresh conversation,
`/resume` reopens one saved for this directory, `/model` switches model,
`/copy` puts the last reply on the clipboard as the Markdown the model wrote,
`/verbose` toggles untruncated tool output and `/exit` quits.

A tool call reads as the tool, what it acts on and a preview of what it
carries, with an `edit` shown as a diff, and its result as a summary line: the
exit status of a command, the size of a file, the error a failure returned.
Both previews are capped so one tool cannot take the scrollback; `/verbose`
lifts the caps and shows every line.

`/model` lists what the provider's `/models` endpoint serves and remembers the
choice for the next run. Past ten entries the popup takes the keyboard and
typing filters it by literal substring, so nothing typed while it is open
reaches the composer.

## Files

yoke follows the XDG Base Directory Specification and writes nothing directly
into `$HOME`. A relative value in an `XDG_*` variable is invalid per the spec
and ignored as if unset, and directories yoke creates are mode 0700.

| what | where | note |
| --- | --- | --- |
| settings | `$XDG_CONFIG_HOME/yoke/config` | every `$XDG_CONFIG_DIRS` entry is searched too, at lower precedence |
| system prompt | `$XDG_CONFIG_HOME/yoke/SYSTEM.md` | used for every session; `$XDG_CONFIG_DIRS` searched too, at lower precedence |
| project prompt | `.yoke/SYSTEM.md` | nearest one at or above the working directory, and it wins over the global one |
| project context | `AGENTS.md` | every one at or above the working directory, appended to the prompt |
| prompt history | `$XDG_STATE_HOME/yoke/history` | last 500 prompts, recalled in the composer with Up/Down |
| chosen model | `$XDG_STATE_HOME/yoke/model` | what `/model` last picked |
| sessions | `$XDG_DATA_HOME/yoke/sessions/<cwd>/<timestamp>.jsonl` | one file per conversation, keyed by the directory it ran in |

## Tests

```
make test                    # run the suite
make test T="-k composer"    # run matching cases
make test-update             # accept intended golden-screen changes
make test-asan               # the same suite against an ASan+UBSan binary
```

`bin/yoke` runs unmodified inside a pseudo-terminal against a dummy
OpenAI-compatible provider, and its output is replayed into a small terminal
emulator, so the tests assert on the rendered screen rather than on escape
sequences. The provider streams customisable lorem ipsum, tool calls, token
usage and HTTP errors, and doubles as a standalone server for driving the UI by
hand without an API key:

```
make mock MOCK_ARGS="--port 8080 --scenario words=80,chunk=2,delay=0.05"
YOKE_BASE_URL=http://127.0.0.1:8080/v1 YOKE_API_KEY=x YOKE_MODEL=mock ./bin/yoke
```

See `tests/README.md` for the scenario language and how to write a case.
