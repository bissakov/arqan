## What this is

`yoke` is a terminal AI coding agent written in plain C17, a minimal counterpoint
to Claude Code / Codex / OpenCode. It talks to any OpenAI-compatible
chat-completions endpoint, streams responses via SSE, and exposes a small
built-in tool registry (read/write/bash/edit/grep/find) that the model can
call.

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

Runtime config comes from env vars or `$XDG_CONFIG_HOME/yoke/config`, which
is `key = value` lines under optional `[section]` headers (see `settings.c`):

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
- `http.c`: libcurl POST (`http_post`) plus a blocking `http_get` for short
  documents such as `/models`, used only by `provider.c`. A reply is delivered
  a line at a time to `on_line` or whole into `HttpReq.body_out`, which is the
  difference between a stream and the one document a turn with streaming off
  comes back as. Runs the transfer on the multi interface so one wait covers
  both curl's sockets and `HttpReq.idle_fd` (stdin), calling `on_idle` after
  every wait, which is what keeps the UI live mid-request and single-threaded
- `paths.c`: XDG base directory resolution for config, data, state and
  cache. Nothing goes directly in `$HOME`, a relative `XDG_*` value is
  ignored as the spec demands, and created directories are 0700. New
  persistent state picks a kind here instead of building its own path
- `settings.c`: the one file format every setting is written in, `key = value`
  lines under optional `[section]` headers with `#` comments, shared by the
  config file, the state file and the credentials file so a user has one
  syntax to learn and three places to look. A write is a per-key upsert rather
  than a rewrite, because a config file is a document its owner edits: the
  comments, the order and the keys yoke knows nothing about survive
  `/provider` writing a section, and the result is renamed over the old file so
  an interrupted write leaves the previous one. It also owns
  `$XDG_STATE_HOME/yoke/state`, the `state_get`/`state_set` pair holding what
  the UI last chose (`model`, `provider`, `telemetry`), so a remembered answer
  costs a key rather than a file
- `telemetry.c`: the record the telemetry setting collects for a bug report,
  appended as JSON lines to `$XDG_STATE_HOME/yoke/telemetry.jsonl` with the
  answer remembered as the state file's `telemetry` key, so a run that never
  reaches the composer records too. It holds the shape of a session and none of its content: a
  message is a byte and a line count, a tool call is its name and the keys of
  its arguments rather than the path or the command they carry, the working
  directory is a hash. A string field is for text yoke formats itself (a tool
  name, a model id, an HTTP status, a `yoke_log` line, which is mirrored here
  so the diagnostics sit beside the events they explain); anything a user or a
  model wrote goes through `tel_shape`, which keeps none of its bytes. An
  event is built on the stack and appended whole, so an interrupted run leaves
  whole lines and a recorder that cannot write costs the session nothing.
  `http.c` records a transfer here because most of what goes wrong with an
  endpoint is invisible from the transcript: curl's status, phase timings,
  bytes each way, and for a stream its SSE line count, its poll count and the
  longest silence between writes. The endpoint is a hash and a class rather
  than a URL, since a private host names its owner the way a path does
- `history.c`: prompt history: a ring of past submissions mirrored line by
  line to `$XDG_STATE_HOME/yoke/history` as they are submitted. It owns an
  arena because `/clear` rewinds the session's, and compacts that arena in
  place when it fills
- `session.c`: saved conversations, keyed by the working directory yoke was
  launched in: `$XDG_DATA_HOME/yoke/sessions/<percent-encoded cwd>/<timestamp>.jsonl`,
  one JSON object per line, appended as messages are produced. It owns its
  path buffers instead of an arena because `/clear` rewinds the session's and
  the file the next message appends to has to outlive that. A file is
  append-only, so `/fork` and `/rewind` both continue in a new one holding the
  conversation as it stands, which leaves the one they came from as it was
- `endpoints.c`: the providers `/provider` creates and switches to, since
  nothing is built in: they all speak the same protocol and only the user
  knows which ones exist. Each is a `[provider <name>]` section of the config
  file, so the settings a user edits are one document, and its key alone sits
  under the same section name in `$XDG_STATE_HOME/yoke/credentials` at mode
  0600, so a configuration that is shared carries no secret and one anyone
  else can read is refused rather than loaded. Only the section being changed
  is written, since the rest of the file is the user's
- `config.c`: loads `Config` from env vars, then the provider the state file
  names, then the model `/model` last remembered in the same file, then
  `$XDG_CONFIG_HOME/yoke/config`, then the `XDG_CONFIG_DIRS` entries at lower
  precedence. A run that names no endpoint and holds no key has nothing to
  talk to, which is what puts `/provider` on the welcome screen rather than a
  form in front of a user who asked for nothing yet, and what answers a
  message submitted before one exists instead of sending it
- `prompt.c`: the system prompt, taken whole from one source: `--system` or
  `YOKE_SYSTEM_PROMPT`, else the project's `.yoke/SYSTEM.md` found by walking
  up from the working directory, else the global
  `$XDG_CONFIG_HOME/yoke/SYSTEM.md`, else the built-in template. It is not a
  config key: a prompt is a document, not a setting. Every `AGENTS.md` at or
  above the working directory is appended to whichever prompt won, outermost
  first, as project context rather than a competing prompt, and verbatim since
  it is a document about the project and not a template. Whichever source wins
  is expanded before it is sent, `{tools}` becoming the registry listing for
  the mode the prompt describes and
  `{cwd}` the working directory, so a prompt written once keeps describing the
  tools that exist now; an unknown `{name}` stays verbatim
- `cli.c`: argv parsing into `CliOpts`, applied over `Config` after
  `config_load`, so a flag outranks the environment and the files. `--help`
  and `--version` answer and exit; a prompt (`-p` or a bare argument) runs one
  turn without the UI and exits on its result
- `tools.c`: the `ToolRegistry` and the built-in tools
  (read/write/bash/edit/grep/find, plus plan mode's `ask_user` and
  `submit_plan`). A result is not a view: it is replayed to the provider on
  every later turn of the session, so each tool answers with a page rather
  than everything it could say and names the call that continues it. `read`
  stops at `YOKE_READ_LINES` or `YOKE_READ_BYTES` and takes an `offset`,
  `shell_capture` keeps the last `YOKE_SHELL_OUT_BYTES` in a ring because a
  command says why it failed on its last lines, and `grep` and `find` cap
  their results. Those two share one walk, sorted by name so a search is
  reproducible and skipping dotfiles; the match is a literal substring rather
  than a regex, since `bash` still has the shell for the rest. Their root may
  name one file rather than a directory, since narrowing a query to the file it
  is about is the same request with a smaller root. `edit` requires
  each `old_text` to match exactly once, and takes a list of them applied in
  order and written once at the end, so an ambiguous or impossible batch
  leaves the file as it was. Each entry carries the modes
  it is offered in, since Plan mode's read-only promise is a property of the
  registry rather than a request made in the prompt: `tools_write_schemas`
  withholds what the mode does not have and `tools_run` refuses it, because a
  schema offered earlier in a conversation is still in the model's context.
  `shell_capture` is the shared command runner behind `bash` and the composer's
  `!` mode: it forks `/bin/sh` instead of using `popen` because stderr left
  inherited would paint over the frame and an inherited stdin would race the
  composer for keystrokes
- `provider.c`: OpenAI-compatible chat-completions client; parses SSE deltas
  into text/reasoning/tool-call callbacks and appends to `Conv`. With
  `Config.stream` off the reply is one `chat.completion` document instead, read
  into the same slots and pushed through the same callbacks, so nothing
  downstream of `read_completion` can tell the two apart.
  `conv_write_json` is where the conversation is charged for: a tool result
  older than `YOKE_ELIDE_TURNS` user turns goes out as a line naming what it
  was, since a file read four turns ago is either reflected in the work
  already or worth reading again. The transcript is unaffected, being a
  rendering of `Conv` rather than of the wire.
  A `reasoning_content` or `reasoning` delta reaches `on_reason` and the
  screen but never `Conv`, since a provider rejects a thinking trace it did
  not produce itself. Each event
  is parsed into a small arena that is reset per delta, so a turn's scratch
  use follows the size of the reply rather than the number of events. A
  request that reached nothing (no delta, no usage, no document) is sent again
  up to `Config.retries` times, backing off from `Config.retry_delay_ms`, since
  a transport failure or a 429/5xx is weather rather than an answer about the
  request; a stream that died after a delta is not, because those bytes are on
  screen and cannot be taken back, and a 401 or a 404 is not, because it will
  say the same thing again. The wait is sliced so `on_idle` keeps running and
  an interrupt ends it at once, and each attempt reaches `on_retry`, which
  `main.c` writes into the transcript in red rather than into a notice: it
  belongs to the turn being read. It never reaches `Conv`, so the model never
  sees it and a replay never repeats it. Also
  `provider_models`, the `/models` listing the model picker offers
- `tui.c`: alternate-screen terminal UI. Overlays stack upward from the
  bottom (notice row, completion popup, composer, status line) and are drawn
  over the transcript's last rows rather than taking rows from it, never over
  each other or the composer: the viewport is the whole body region either
  way, so opening one hides the rows it covers and leaves every other one
  where the reader last saw it, and the keys that move the viewport work the
  same under a popup, a picker or a question. `tui_pick` drives
  that same popup as a modal list, `tui_settings` drives it as one that is read
  rather than chosen from (Space acts on the selected row and hands it back to
  the caller, Enter and Escape close, and the selection survives the reopen a
  change costs), and `tui_ask` borrows the composer for one
  question, its answer echoed as dots when it is a secret and kept out of the
  history and the transcript either way, and past ten entries it takes the keyboard:
  typing filters by literal substring and the notice row becomes the search
  box. A caller says which end of its list the selection opens on and returns
  to after a search, since a list ordered like the transcript wants the entry
  nearest the composer rather than the first row. A notice is how a command that opened no popup answers, so nothing but the conversation is
  ever written into the transcript. Escape at an idle composer with nothing to
  dismiss arms a rewind and answers in that row; a second Escape submits
  `/rewind`, leaving the draft alone, so the key and the command reach `main.c`
  as one request, and Shift+Tab submits `/mode` the same way; the status line
  names the mode next to the model. The popup completes a path as well as a
  command: a word at the cursor starting with `@` is listed from the
  filesystem, directories first and carrying a trailing slash, which is what
  makes accepting one a step into it rather than an answer, and what a picked
  file leaves in the composer is text in a message rather than a command, so
  Enter takes it without submitting. A word typed after the directory searches
  the tree below it rather than that one directory, since naming every folder
  on the way to a deep file is the work the picker exists to save: the ranking
  is a name that starts with the word, a name that holds it, a path that holds
  it, then a path its letters appear across in order, and shallower before
  deeper, with the name breaking a tie so a search is reproducible. The walk
  is bounded by `TUI_PATH_SCAN` and `TUI_PATH_DEPTH` because it runs on a
  keystroke, and its results are held in one bounded list kept in order, so a
  tree of any size costs the 256 entries the popup keeps rather than the
  matches it met. What the project excludes is not offered:
  `.gitignore` and `.ignore` say the same thing about a path and are read as
  one list, every file from the working directory down to the one being
  listed, rebuilt per listing rather than cached since a stale answer is a
  file the picker refuses to show. It is a default rather than a rule, lifted
  by the `show_ignored` setting, since a path typed by hand reaches anything;
  `.git` is the one exclusion no setting lifts. Also: viewport, scrollback, raw-mode
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
  the whole scrollback. A byte range of the transcript can also be a zone: a
  click inside one submits `/expand <id>` the way Escape submits `/rewind`,
  a zone's rows are painted as a link and brighten while the pointer is on
  them (which is why mouse mode 1003 rather than 1002 is claimed), and
  `tui_anchor_zone`/`tui_restore_anchor` keep a zone where it is on screen
  while the caller replays the transcript around it. The air between two
  blocks is one blank row and comes from one place: a writer opens a block
  with `tui_block` and writes none of its own, since a margin split between
  the block that ended and the one that starts is a margin nobody owns. That
  holds because a trailing newline is never committed when it is written: it
  is held until content follows it, so a reply that ends on three of them and
  a tool result that ends on one arrive at the next block the same way. A user
  turn is the one block with padding rows of its own, which it commits so they
  fall inside the range painted with its background. The row below the
  transcript is the frame's, not a block's: a conversation long enough to fill
  the view has no way to write air under itself, so the layout keeps one blank
  row between it and the overlays, the way it keeps one between the composer
  and the status line
- `markdown.c`: a reply is Markdown, and this renders it into the transcript:
  headings, lists, block quotes, rules and fenced code become shapes, emphasis
  and inline code become styles, and the markers are dropped. It renders as the
  reply streams, buffering only what is undecided (the bytes that may still be
  a block marker, and an opener whose closer has not arrived), both bounded by
  the line they sit in, so a delta is painted as soon as its shape is known.
  The raw setting turns it off, and so does the absence of a fullscreen UI,
  since a one-shot run's stdout is a reply rather than a view
- `render.c`: how a tool call and its result read in the transcript: a header
  naming the tool and its target, and for a `read` the page it asked for, since
  two reads of one file are otherwise the same header twice, a preview of the input it carries (a diff for
  `edit`), and a result summarised by the tool's own shape. The JSON arguments
  never reach the screen except for a tool this module knows nothing about.
  The tail a truncated block ends on is its click target: it carries a TUI zone
  keyed by the `Conv` slot it was rendered from, and `Conv.expanded` is what a
  click leaves behind, lifting that one block's caps the way the verbose
  setting lifts every block's. A replay writes the same air a live turn does, since the
  transcript is one rendering either way. A plan and the question that led to
  it read as blocks of the same family, the plan as the Markdown it was
  written in and never truncated, since it is what the user is approving
- shell mode: a composed line whose first byte is `!` runs locally instead of
  reaching the model. The `!` is the composer's prompt marker rather than text,
  red in place of the blue one, and the run takes a `Conv` slot of its own
  (`conv_add_shell`: a user turn holding the command, with what it printed in
  the parallel `shell_out`), because the transcript is a rendering of the
  conversation and a run living outside it would vanish on the next replay. It
  reaches the provider as the user message it is, the command then its output,
  and a session file keeps it as a `"name":"shell"` line with an `"output"`
- plan mode: the second of the two `AgentMode`s, which the composer's
  Shift+Tab switches by submitting `/mode` the way Escape submits `/rewind`.
  It swaps slot 0 of the conversation for `Config.plan_prompt` and the
  registry for the read-only half of itself, so the mode is one assignment
  rather than a second agent. Its two tools are questions put to the user, so
  the agent loop answers them instead of `tools_run`, which cannot reach the
  screen: `ask_user` opens the picker on the option the model recommends, and
  `submit_plan` renders the plan and asks whether to carry it out. "Yes"
  flips the mode and continues the same turn; "yes, from a new session"
  rewinds the conversation to its system prompt and re-enters with the plan as
  the only message, which is why the plan is carried in the scratch arena that
  the persistent rewind does not touch; "no" ends the turn with the mode
  unchanged. Whichever it is, the answer reaches the model as the tool result
  it asked for
- `/settings`: the toggles and values of a session in one screen, built in
  `main.c` over `tui_settings`. A toggle that was worth a command of its own
  when it was the only one is not worth one when there are five, so verbose,
  raw, streaming, ignored paths and telemetry live here rather than in the
  completion popup,
  next to the mode, model, provider and token cap a turn is sent with. The
  rows are rebuilt from the state they describe on every pass of the loop, so
  the screen is its own answer and nothing in it writes a notice: a checkbox
  that did not move is a setting that refused to change. Nothing is persisted
  here either, since a setting that outlives the session is already remembered
  by whoever owns it
- `main.c`: wires everything together and runs the agent loop

**Agent loop shape** (`main.c`): each user turn calls `provider_run` in a
loop that runs until the model stops asking for tools, with no round cap: one
would end a long build in the middle of itself, and a user who walked away from
it is not there to lift it. Stopping the agent is theirs to do, through
`SIGINT`. `provider_run` streams one completion, appends
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
`YOKE_MAX_FILE_BYTES`). What it returns is capped separately and on purpose:
pick a default that answers the question, say what was left out and how to ask
for it, and never truncate silently.

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
