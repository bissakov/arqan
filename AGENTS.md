## What this is

`yoke` is a terminal AI coding agent written in plain C17, a minimal counterpoint
to Claude Code / Codex / OpenCode. It talks to any OpenAI-compatible
chat-completions endpoint, streams responses via SSE, and exposes a small
built-in tool registry (read/write/bash/edit) that the model can call.

## Build & run

```
make            # builds bin/yoke
make run        # builds and runs
make test       # end-to-end TUI tests (Python 3, no third-party packages)
make test-asan  # the same suite against an ASan+UBSan build
make clean      # removes build/ and bin/
```

There is no linter target: `CFLAGS` already includes `-Wall -Wextra
-Wpedantic -Wconversion`, so treat new warnings as build failures.

`make test` is not optional after touching `src/`; see [Tests](#tests).

Runtime config comes from env vars or `$XDG_CONFIG_HOME/yoke/config`:

```
export YOKE_BASE_URL=https://api.openai.com/v1
export YOKE_MODEL=gpt-4o-mini
export YOKE_API_KEY=sk-...
export YOKE_MAX_MESSAGES=4096   # conversation capacity
```

(see `config.c` for the full key set and precedence).

## Architecture

**Unity build.** `main.c` `#include`s every other `.c` file in `src/` as one
translation unit; the Makefile compiles exactly one object and links once.
When adding a new module, add the `.c` file and `#include` it from `main.c`, and
do not add it to a separate compile unit or the Makefile's `$(SRC)`.

`src/yoke.h` is the single umbrella header: every type, struct, and function
signature used across modules lives there. Read it first when orienting: it maps the whole data model without opening
every `.c`.

**Data-oriented, no heap after startup.** Two large blocks are obtained once
via static arrays in `main.c` (`g_persist`, `g_scratch`, sized by
`YOKE_PERSIST_BYTES` / `YOKE_ARENA_BYTES` in `yoke.h`) and wrapped in `Arena`s.
All conversation state, tool registry entries, and JSON parsing live in one
of these two arenas: `persist` for data that must survive the whole
session, `scratch` for per-turn work that's thrown away via `arena_reset`
after each provider turn. There are no `malloc`/`free` calls anywhere in our
code (libcurl's internal allocations are the one exception, outside our
control). New features should follow this: allocate from the right arena
up front rather than introducing dynamic allocation.

**Allocation can fail, and every caller checks.** The arenas are finite and
their consumers are fed by a remote provider, so `arena_alloc` returning
NULL is a normal path, not a theoretical one: it is checked at every call
site, `Buf` latches an `oom` flag and drops writes instead of running past
`cap` (`buf_ok` reports it), and `conv_*` returns `CONV_NONE` when the
conversation is full. Sizes that come from outside (a JSON field, a file's
length, a config value) are validated *before* they reach an allocation;
`arena_alloc_array` exists so a `count * size` can never wrap into a small
satisfiable request. Adding a code path that ignores one of these is the
same class of bug as a missing bounds check.

**SoA everywhere.** Conversation messages (`Conv` in `yoke.h`), the tool
registry (`ToolRegistry`), and other hot collections are structure-of-arrays
(parallel arrays indexed by id), not arrays-of-structs. When extending
`Conv` or `ToolRegistry`, add a new parallel array rather than switching to
an AoS layout.

**Module responsibilities:**
- `core.c`: arena allocator, `Str`/`Buf` string types, logging, monotonic time
- `json.c`: arena-backed JSON DOM: parser + serializer, no separate token stream
- `http.c`: libcurl streaming POST or SSE (`http_sse_post`), used only by
  `provider.c`. Runs the transfer on the multi interface so one wait covers
  both curl's sockets and `HttpReq.idle_fd` (stdin), calling `on_idle` after
  every wait, which is what keeps the UI live mid-request and single-threaded
- `paths.c`: XDG base directory resolution for config, data, state and
  cache. Nothing goes directly in `$HOME`, a relative `XDG_*` value is
  ignored as the spec demands, and created directories are 0700. New
  persistent state picks a kind here instead of building its own path
- `history.c`: prompt history: a ring of past submissions mirrored line by
  line to `$XDG_STATE_HOME/yoke/history` as they are submitted. It owns an
  arena because `/clear` rewinds the session's, and compacts that arena in
  place when it fills
- `session.c`: saved conversations, keyed by the working directory yoke was
  launched in: `$XDG_DATA_HOME/yoke/sessions/<percent-encoded cwd>/<timestamp>.jsonl`,
  one JSON object per line, appended as messages are produced. It owns its
  path buffers instead of an arena because `/clear` rewinds the session's and
  the file the next message appends to has to outlive that
- `config.c`: loads `Config` from env vars, then `$XDG_CONFIG_HOME/yoke/config`,
  then the `XDG_CONFIG_DIRS` entries at lower precedence
- `tools.c`: the `ToolRegistry` and the four built-in tools (read/write/bash/edit)
- `provider.c`: OpenAI-compatible chat-completions streaming client; parses
  SSE deltas into text/tool-call callbacks and appends to `Conv`. Each event
  is parsed into a small arena that is reset per delta, so a turn's scratch
  use follows the size of the reply rather than the number of events
- `tui.c`: alternate-screen terminal UI. Overlays stack upward from the
  bottom (notice row, completion popup, composer, status line) and eat into
  the transcript, never into each other or the composer. A notice is how a
  command that opened no popup answers, so nothing but the conversation is
  ever written into the transcript. Also: viewport, scrollback, raw-mode
  composer with Up/Down recall of the persisted prompt history, mouse wheel
  scrolling, drag-to-select with OSC 52 copy, and SIGWINCH-aware repaint.
  Every visible glyph is painted through `put_text`, which mirrors it into the
  per-row screen snapshot that selection highlights and copies from. Frames are
  built in `TuiState.out` and hit the terminal as one `write`; the composer lives in
  `TuiState` for the whole session, so `tui_readline` (blocking, submits) and
  `tui_poll_input` (non-blocking, refuses Enter while `busy`) drive the same
  editor. The transcript carries a wrapped-row index (row count plus periodic
  byte-offset checkpoints, extended incrementally as output arrives and
  dropped whenever existing bytes move) so a frame costs the visible rows, not
  the whole scrollback
- `main.c`: wires everything together and runs the agent loop

**Agent loop shape** (`main.c`): each user turn calls `provider_run` in a
loop capped at 16 iterations. `provider_run` streams one completion, appends
the assistant message (and any tool calls) to `Conv`, and returns the tool
call count. If nonzero, `main` scans the newly appended `Conv` tail for
tool-call slots (an assistant head slot carrying the prose followed by one
carrier slot per call, each with its own `tool_call_id`, which `conv_is_call`
identifies), executes each via `tools_find` + `tools_run`, appends the
results as `M_TOOL` messages, and loops again; a zero return ends the turn.
`SIGINT` sets `g_got_sigint`, checked between and during provider calls to
support cancellation. `main` marks the turn with `tui_set_busy` and passes
`on_idle` + `tui_input_fd()` to the provider, which is how typing stays alive
while the model streams.

**Adding a tool:** implement a `ToolRun`, `b8 (*)(Str args_json, Arena
*scratch, Buf *out, char *err, size_t err_cap)`, in `tools.c` and register
it (name + description + JSON schema fragment) in `tools_init`, capped by
`YOKE_MAX_TOOLS`. The registry is SoA: `tools_find` returns a tool id (or
`TOOL_NONE`) and `tools_run` dispatches on it. A tool never clamps an
argument to fit a buffer: a truncated path or command is a *different*
operation than the one the user reviewed, so oversized arguments are
refused with an error (`YOKE_MAX_PATH`, `YOKE_MAX_COMMAND`,
`YOKE_MAX_FILE_BYTES`).

## Tests

**Every change to `src/` ends with `make test`.** Not "when it feels risky",
not "before the PR", but after the change, every time. The suite drives the real
binary end to end, runs in parallel, and finishes in seconds, so there is no
budget argument for skipping it: the cost of running it is far below the cost
of one regression that reaches a user. A change is not done when it compiles;
it is done when the suite is green.

**Every behavioural change also brings a case with it.** Testing this agent is
cheap and direct: you script what the provider streams, drive real keystrokes
into a real pty, and assert on the rendered screen. There is no mocking
ceremony, no seam to invent, no dependency to inject. If a behaviour is worth
implementing, it is worth ten lines in `tests/cases/`. So:

- new feature: a case that exercises it through the UI;
- bug fix: a case that fails before the fix and passes after (write it first
  and watch it fail, which is the only proof the fix addresses the real cause);
- refactor: no new case, but the existing suite must stay green *unchanged*;
  if a refactor forces a test edit, the refactor changed behaviour.

When a case is hard to write, that is a signal about the design, not about the
harness: the seam is usually missing in `src/`, and adding it is part of the
task.

`tests/` drives the real `bin/yoke` inside a pseudo-terminal against a dummy
OpenAI-compatible provider, replaying its output through a small terminal
emulator. Assertions are made against the rendered screen, not escape
sequences; `tests/golden/` holds expected screen dumps (`make test-update`
rewrites them, so read the diff first). `tests/README.md` documents the
scenario language of the mock provider and the waiting primitives that keep
cases reproducible, where nothing sleeps for a fixed time and everything waits
on a state. Cases are fully isolated (own temp `HOME`, own provider port, own pty)
and the runner executes them in parallel; `-j 1` forces one at a time when
debugging.

A failing test means the source is wrong until proven otherwise. Do not
relax an assertion to match wrong behaviour: fix it in `src/`, then keep the
test as the regression. The same applies to golden screens: `make
test-update` is for changes you *intended* to make and have read in the diff,
never a way to make red go away. An unexplained golden diff is a bug report.

Flaky is failing. If a case only passes sometimes, `python3 tests/run.py
--repeat 5` it and fix the race in `src/` or the missing wait in the case;
never retry around it.

**No throwaway tests.** Do not verify behavior with one-off `bash`
invocations, temporary scripts, or scratch programs that live outside
`tests/`. Every regression check for the TUI goes into `tests/` as a proper
case so it runs under `make test`, is isolated like the rest, and stays
reproducible. Ad-hoc commands are fine for exploration, but anything meant
to prove a behavior is correct, or keep it correct, must be recorded as a
test case, not left as a transient shell snippet.

## Comments & documentation

Comments and docs are slop-free or absent. A comment earns its place by saying
something the code cannot: why a constant has that value, which invariant a
branch protects, what a wire format looks like. If the code already says it,
delete the comment. When touching a file, remove restatements you find rather
than leaving them.

Banned outright:

- Verbosity: preamble, hedging, and words that carry nothing.
- Awkward or padded phrasing.
- Em dash, en dash, smart quotes, ellipsis character, arrows, checkmarks, emoji.
  Use ASCII: `-`, `"`, `'`, `...`, `->`.
- Repeating information that already exists elsewhere in the file, the header,
  or this document.
- Runs of short sentences that a single `and` would join.

Write full sentences with a capital and a period, present tense, active voice,
and keep them within the file's line width. Doc comments in `src/yoke.h`
describe contracts: ownership, arena used, what a failure returns. They do not
narrate the implementation.

```c
/* Bad */
/* Loop over the messages and process each one. */
/* This function -> returns the tool id. */

/* Good */
/* Slot 0 holds the prose; every later slot carries one tool_call_id. */
```

The same rules apply to `README`s, `tests/README.md`, and commit messages.

## End of task

- Never add attributions to AI agents (or any automated tooling) in commit messages, PR
  descriptions, code comments, changelogs, or any tracked content. Changes are authored by the
  contributor; do not reference the assistant or its involvement anywhere.
- If, and only if, the task created or modified files that git tracks (or
  would track, i.e. not ignored), end with a suggested commit message
  matching the project's convention:

  ```
  (<type>[+<type>...]): brief description
  <type> = feat | fix | refactor | perf | test | docs | style | build | ci | revert | chore
  ```

  Parentheses are mandatory; the description is imperative, lowercase, no
  trailing period. No attribution of any kind: no `Co-Authored-By`, no
  "Generated with" trailers, no tool signatures.
- Types, most specific wins: pick the one matching the intent of the
  change, not the file kind:
  - `(feat)`: new user- or operator-visible capability (endpoint,
    provider, pack field, CLI flag).
  - `(fix)`: corrects wrong behavior; something was broken before, works
    after.
  - `(refactor)`: code restructuring with no behavior change.
  - `(perf)`: improves speed or resource use without changing behavior.
  - `(test)`: adds or changes tests only.
  - `(docs)`: documentation only (`README`, `docs/`, docstrings,
    pack/dataset READMEs).
  - `(style)`: formatting, naming, whitespace; no semantic change.
  - `(build)`: dependencies, `pyproject.toml`/`uv.lock`, Dockerfiles,
    compose, Helm, Makefile.
  - `(ci)`: `.github/workflows/` only.
  - `(revert)`: undoes a previous commit; reference it in the
    description.
  - `(chore)`: repo housekeeping that fits none of the above (gitignore,
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
