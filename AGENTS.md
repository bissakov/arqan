## What this is

`ah` is a terminal AI coding agent written in plain C17 — a minimal counterpoint
to Claude Code / Codex / OpenCode. It talks to any OpenAI-compatible
chat-completions endpoint, streams responses via SSE, and exposes a small
built-in tool registry (read/write/bash/edit) that the model can call.

## Build & run

```
make            # builds bin/ah
make run        # builds and runs
make test       # end-to-end TUI tests (Python 3, no third-party packages)
make clean      # removes build/ and bin/
```

There is no linter target — `CFLAGS` already includes `-Wall -Wextra
-Wpedantic -Wconversion`, so treat new warnings as build failures.

Runtime config comes from env vars or `~/.config/ah/config`:

```
export AH_BASE_URL=https://api.openai.com/v1
export AH_MODEL=gpt-4o-mini
export AH_API_KEY=sk-...
```

(see `config.c` for the full key set and precedence).

## Architecture

**Unity build.** `main.c` `#include`s every other `.c` file in `src/` as one
translation unit; the Makefile compiles exactly one object and links once.
When adding a new module, add the `.c` file and `#include` it from `main.c` —
do not add it to a separate compile unit or the Makefile's `$(SRC)`.

`src/ah.h` is the single umbrella header: every type, struct, and function
signature used across modules lives there. Read it first when orienting —
it's a complete map of the data model without needing to open every `.c`.

**Data-oriented, no heap after startup.** Two large blocks are obtained once
via static arrays in `main.c` (`g_persist`, `g_scratch`, sized by
`AH_PERSIST_BYTES` / `AH_ARENA_BYTES` in `ah.h`) and wrapped in `Arena`s.
All conversation state, tool registry entries, and JSON parsing live in one
of these two arenas — `persist` for data that must survive the whole
session, `scratch` for per-turn work that's thrown away via `arena_reset`
after each provider turn. There are no `malloc`/`free` calls in the hot
path (libcurl's internal allocations are the one exception, outside our
control). New features should follow this: allocate from the right arena
up front rather than introducing dynamic allocation.

**SoA everywhere.** Conversation messages (`Conv` in `ah.h`), the tool
registry (`ToolRegistry`), and other hot collections are structure-of-arrays
(parallel arrays indexed by id), not arrays-of-structs. When extending
`Conv` or `ToolRegistry`, add a new parallel array rather than switching to
an AoS layout.

**Module responsibilities:**
- `core.c` — arena allocator, `Str`/`Buf` string types, logging, monotonic time
- `json.c` — arena-backed JSON DOM: parser + serializer, no separate token stream
- `http.c` — libcurl streaming POST or SSE (`http_sse_post`), used only by
  `provider.c`. Runs the transfer on the multi interface so one wait covers
  both curl's sockets and `HttpReq.idle_fd` (stdin), calling `on_idle` after
  every wait — that is what keeps the UI live mid-request, single-threaded
- `config.c` — loads `Config` from env vars then `~/.config/ah/config`
- `tools.c` — the `ToolRegistry` and the four built-in tools (read/write/bash/edit)
- `provider.c` — OpenAI-compatible chat-completions streaming client; parses
  SSE deltas into text/tool-call callbacks and appends to `Conv`
- `tui.c` — alternate-screen terminal UI: viewport, scrollback, raw-mode
  composer, mouse wheel scrolling, drag-to-select with OSC 52 copy,
  SIGWINCH-aware repaint. Every visible glyph is painted through `put_text`,
  which mirrors it into the per-row screen snapshot selection highlights and
  copies from. Frames are built
  in `TuiState.out` and hit the terminal as one `write`; the composer lives in
  `TuiState` for the whole session, so `tui_readline` (blocking, submits) and
  `tui_poll_input` (non-blocking, refuses Enter while `busy`) drive the same
  editor
- `main.c` — wires everything together and runs the agent loop

**Agent loop shape** (`main.c`): each user turn calls `provider_run` in a
loop capped at 16 iterations. `provider_run` streams one completion, appends
the assistant message (and any tool calls) to `Conv`, and returns the tool
call count. If nonzero, `main` scans the newly appended `Conv` tail for
tool-call slots, executes each via `tools_find` + `ToolDef.run`, appends the
results as `M_TOOL` messages, and loops again; a zero return ends the turn.
`SIGINT` sets `g_got_sigint`, checked between and during provider calls to
support cancellation. `main` marks the turn with `tui_set_busy` and passes
`on_idle` + `tui_input_fd()` to the provider, which is how typing stays alive
while the model streams.

**Adding a tool:** implement a `b8 (*run)(Str args_json, Arena *scratch, Buf
*out, char *err, size_t err_cap)` function in `tools.c`, register it (name +
description + JSON schema fragment) in `tools_init`, capped by
`AH_MAX_TOOLS`.

## Tests

`tests/` drives the real `bin/ah` inside a pseudo-terminal against a dummy
OpenAI-compatible provider, replaying its output through a small terminal
emulator. Assertions are made against the rendered screen, not escape
sequences; `tests/golden/` holds expected screen dumps (`make test-update`
rewrites them, so read the diff first). `tests/README.md` documents the
scenario language of the mock provider and the waiting primitives that keep
cases reproducible — nothing sleeps for a fixed time, everything waits on a
state. Cases are fully isolated (own temp `HOME`, own provider port, own pty)
and the runner executes them in parallel; `-j 1` forces one at a time when
debugging.

A failing test means the source is wrong until proven otherwise. Do not
relax an assertion to match clunky behaviour: fix it in `src/`, then keep the
test as the regression.

## End of task

- Never add attributions to AI agents (or any automated tooling) in commit messages, PR
  descriptions, code comments, changelogs, or any tracked content. Changes are authored by the
  contributor; do not reference the assistant or its involvement anywhere.
- If — and only if — the task created or modified files that git tracks (or
  would track, i.e. not ignored), end with a suggested commit message
  matching the project's convention:

  ```
  (<type>[+<type>...]): brief description
  <type> = feat | fix | refactor | perf | test | docs | style | build | ci | revert | chore
  ```

  Parentheses are mandatory; the description is imperative, lowercase, no
  trailing period. No attribution of any kind — no `Co-Authored-By`, no
  "Generated with" trailers, no tool signatures.
- Types, most specific wins — pick the one matching the intent of the
  change, not the file kind:
  - `(feat)` — new user- or operator-visible capability (endpoint,
    provider, pack field, CLI flag).
  - `(fix)` — corrects wrong behavior; something was broken before, works
    after.
  - `(refactor)` — code restructuring with no behavior change.
  - `(perf)` — improves speed or resource use without changing behavior.
  - `(test)` — adds or changes tests only.
  - `(docs)` — documentation only (`README`, `docs/`, docstrings,
    pack/dataset READMEs).
  - `(style)` — formatting, naming, whitespace; no semantic change.
  - `(build)` — dependencies, `pyproject.toml`/`uv.lock`, Dockerfiles,
    compose, Helm, Makefile.
  - `(ci)` — `.github/workflows/` only.
  - `(revert)` — undoes a previous commit; reference it in the
    description.
  - `(chore)` — repo housekeeping that fits none of the above (gitignore,
    configs, data manifests).
- Prefer a single type. Incidental companions don't earn a union: a
  feature with its own tests and docs is `(feat)`. Use a union like
  `(fix+refactor)` only when each part would stand as its own commit,
  dominant type first; if the parts are unrelated, suggest splitting the
  commit instead.
- Otherwise end without one. No commit message for: answering questions
  or analysis, work outside the repository or in git-ignored paths,
  amendments to work already summarized with a commit message earlier in
  the session (restate the one message covering the final state instead
  of adding a second), or tasks that left the working tree unchanged.
