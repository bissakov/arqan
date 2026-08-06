# yoke

A terminal AI coding agent written in plain C17, designed as a counterpoint to
Claude Code, Codex, OpenCode and Pi.

## Design principles

- **C17, single translation unit (unity) build** — sub-second full rebuilds.
- **Structure-of-Arrays (SoA)** for hot data (messages, tool calls, tool registry).
- **Data-oriented design** — cache-friendly, contiguous, typed arrays.
- **No heap allocations after start-up.** Everything lives in a few big
  preallocated blocks obtained once via `mmap`/`aligned_alloc`.
- **Arenas / bump allocators** for per-turn scratch; freed by resetting a
  pointer. No `free()` calls anywhere in the hot path.
- **Fast start-up** — no runtime, no JIT, no modules to resolve. The binary
  does one `mmap`, reads one config file, and enters the event loop.
- **Minimal deps** — only libc + libcurl (for HTTPS to OpenAI-compatible
  endpoints). Everything else is hand-written (JSON, SSE, TUI, tool runner).

## Layout

```
src/
  yoke.h            single umbrella header (all types + fwd decls)
  core.c          arena, Str, slices, log, time, dynamic arrays in arenas
  json.c          tiny arena JSON parser + serializer
  http.c          libcurl streaming POST (SSE)
  config.c        env + ~/.config/yoke/config loader
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

`main.c` `#include`s every `*.c` so the whole project is one translation unit.
The Makefile compiles a single object → one link. Rebuild from scratch is well
under a second.

## Build & run

```
make
./yoke
```

Set at least a base URL, model and API key, e.g.:

```
export YOKE_BASE_URL=https://api.openai.com/v1
export YOKE_MODEL=gpt-4o-mini
export YOKE_API_KEY=sk-...
```

or put them in `~/.config/yoke/config`:

```
base_url=https://api.openai.com/v1
model=gpt-4o-mini
api_key=sk-...
```

## Tests

```
make test                    # run the suite
make test T="-k composer"    # run matching cases
make test-update             # accept intended golden-screen changes
```

`bin/yoke` runs unmodified inside a pseudo-terminal against a dummy
OpenAI-compatible provider, and its output is replayed into a small terminal
emulator — so the tests assert on the rendered screen rather than on escape
sequences. The provider streams customisable lorem ipsum, tool calls, token
usage and HTTP errors, and doubles as a standalone server for driving the UI by
hand without an API key:

```
make mock MOCK_ARGS="--port 8080 --scenario words=80,chunk=2,delay=0.05"
YOKE_BASE_URL=http://127.0.0.1:8080/v1 YOKE_API_KEY=x YOKE_MODEL=mock ./bin/yoke
```

See `tests/README.md` for the scenario language and how to write a case.

## Status

This is the scaffolded core: arena memory, JSON, streaming HTTP, an
OpenAI-compatible streaming provider, a SoA tool registry with four tools
(read / write / bash / edit), and a fullscreen, repaintable TUI wired into an
agent loop. It compiles and runs end-to-end against any OpenAI-compatible
endpoint.

The TUI uses the terminal's alternate screen, adapts to `SIGWINCH`, streams
responses into a bounded transcript, and keeps a multiline composer at the
bottom, with the status line just below it. Enter sends, Alt+Enter inserts a
newline, PageUp/PageDown scroll the transcript, the mouse wheel scrolls it
through a visible scrollbar, Ctrl-L repaints, Ctrl-C cancels, and Ctrl-D quits
from an empty composer.

Text is selectable with the mouse anywhere on screen — transcript, composer and
status line alike. Dragging highlights the range, releasing copies it to the
system clipboard via OSC 52 (so it works over ssh too) and the status line
confirms with `copied`; any keystroke drops the highlight. Holding Shift while
dragging bypasses the app and falls back to the terminal's own selection.
Redirected stdin/stdout automatically falls back to plain text.

Typing `/` opens a completion popup above the composer listing the slash
commands; it narrows as you type, Ctrl-N/Ctrl-P (or the arrow keys) move
through it, Tab completes the highlighted entry, Enter completes *and* runs it
in one keystroke, and Esc dismisses it. `/clear` clears the active conversation
and `/exit` exits.

The status line spells out what the session is doing — `ready`, `thinking`,
`running <tool>` — rather than leaving it to the colour of the bullet, so it
still reads under `NO_COLOR`. Diagnostics that would otherwise land on stderr,
in the middle of the frame, are folded into the transcript as notices.

The composer stays editable while a turn is running — the request waits on
stdin alongside its socket — but Enter only sends once the turn is done; the
dimmed prompt marks the difference. Each repaint is assembled in memory and
written to the terminal in one go, and a burst of keystrokes collapses into a
single frame, so fast typing never shows a half-drawn screen.
