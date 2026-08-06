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
  provider.c      OpenAI-compatible chat-completions streaming + tool deltas
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

## Using it

Enter sends and Alt+Enter inserts a newline. Ctrl-C discards the draft or
cancels a running turn, Esc also cancels one, and Ctrl-D quits from an empty
composer. Up/Down recall past prompts, PageUp/PageDown and the mouse wheel
scroll the transcript, and Ctrl-L repaints. Dragging selects any text on
screen and copies it over OSC 52 on release, so it works over ssh; Shift falls
back to the terminal's own selection.

Typing `/` opens a completion popup: `/clear` starts a fresh conversation,
`/resume` reopens one saved for this directory, `/model` switches model,
`/copy` puts the last reply on the clipboard as the Markdown the model wrote
and `/exit` quits.

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
