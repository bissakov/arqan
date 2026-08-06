# ah — a C17 AI coding harness

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
  ah.h            single umbrella header (all types + fwd decls)
  core.c          arena, Str, slices, log, time, dynamic arrays in arenas
  json.c          tiny arena JSON parser + serializer
  http.c          libcurl streaming POST (SSE)
  config.c        env + ~/.config/ah/config loader
  provider.c      OpenAI-compatible chat-completions streaming + tool deltas
  tools.c         SoA tool registry + read/write/bash/edit tools
  tui.c           alternate-screen TUI, viewport, composer + raw input
  main.c          unity includes + main + agent loop
```

`main.c` `#include`s every `*.c` so the whole project is one translation unit.
The Makefile compiles a single object → one link. Rebuild from scratch is well
under a second.

## Build & run

```
make
./ah
```

Set at least a base URL, model and API key, e.g.:

```
export AH_BASE_URL=https://api.openai.com/v1
export AH_MODEL=gpt-4o-mini
export AH_API_KEY=sk-...
```

or put them in `~/.config/ah/config`:

```
base_url=https://api.openai.com/v1
model=gpt-4o-mini
api_key=sk-...
```

## Status

This is the scaffolded core: arena memory, JSON, streaming HTTP, an
OpenAI-compatible streaming provider, a SoA tool registry with four tools
(read / write / bash / edit), and a fullscreen, repaintable TUI wired into an
agent loop. It compiles and runs end-to-end against any OpenAI-compatible
endpoint.

The TUI uses the terminal's alternate screen, adapts to `SIGWINCH`, streams
responses into a bounded transcript, and keeps a multiline composer at the
bottom. Enter sends, Alt+Enter inserts a newline, PageUp/PageDown scroll the
transcript, the mouse wheel scrolls it through a visible scrollbar, Ctrl-L
repaints, Ctrl-C cancels, and Ctrl-D quits from an empty composer. `/new`
clears the active conversation and `/exit` exits. Redirected stdin/stdout
automatically falls back to plain text.
