## Project

`arqan` is a terminal AI coding agent.

## Build and test

```sh
make              # build bin/arqan
make run          # build and run
make fmt          # format src/, highlight/ and tests/unit/
make check-format # verify formatting without writing
make test         # end-to-end TUI suite
make test-unit    # arena, string and JSON invariants
make bench        # benchmark and stress suite
make bench-guard  # measure this tree against the commit under it
make test-asan    # ASan + UBSan suite
make test-fil     # Fil-C memory-safety suite (needs /opt/fil)
make clean        # remove build/ and bin/
```

Run `make fmt` and `make test` after every change under `src/`. Warnings are
failures: CI builds with `-Werror` on top of `-Wall -Wextra -Wpedantic
-Wconversion`.

Install the pinned formatter with `pipx install clang-format==22.1.8`; the
Makefile refuses other major versions. Do not hand-format around it or add
`clang-format off`.

A change in behaviour needs a case in `tests/cases/`; for a bug, write the
regression first. Never weaken an assertion or update a golden file to hide a
failure. Check a suspected flake with `python3 tests/run.py --repeat 5`.

Run the other suites when a change touches what they cover:

- `test-unit` for arenas, string and buffer bounds, and JSON, or when a bug has
  no natural end-to-end test. It compiles `tests/unit/main.c` against `src/`
  directly, not through the unity build.
- `test-fil` for memory bugs, and always for arena, buffer, bounds and pointer
  arithmetic. Fil-C carries bounds on the pointer, so it catches an
  out-of-bounds access that lands inside another valid object; ASan cannot.
- `bench-guard` before changing startup, rendering, tool output, or session
  replay. It fails when CPU per operation grows past 1.4x or private memory
  past 1.15x. Never widen a tolerance to pass. It runs the binary it built, so
  rebuilding during a run makes it fail for that reason, not a real one.
  `bench/README.md` explains what is measured.

Startup time, throughput and memory use are user-visible. A change that costs
any of them is a regression.

## Core design

- Unity build: `src/main.c` includes every `.c` file. Add new modules there,
  not to the Makefile.
- `src/agent.h` is the only cross-module header.
- No `malloc` or `free`. Use the startup arenas: `persist` for session-lifetime
  data, `scratch` for per-turn data.
- Allocation failure is normal. Check `arena_alloc`, `buf_ok` and collection
  returns, validate outside sizes before allocating, and use
  `arena_alloc_array` for multiplied sizes.
- Keep hot collections as structure-of-arrays: extend `Conv` and
  `ToolRegistry` with parallel arrays.
- Read owned files through `file_read`. Write updates through a temporary file
  and a rename.
- Two names, kept apart. Code uses `agent`/`agent_`/`AGENT_`/`Agent`; anything
  a user sees uses `AGENT_NAME` and `AGENT_ENV_PREFIX`. Never hard-code the
  product name where those reach. Rename with `python3 scripts/rename.py`.

## Global state

The process is single-threaded, so globals are not a concurrency problem. They
are a coupling problem: call order turns into API and resets get missed.

Every global belongs to a named `static` state struct that owns one concern. Do
not add a loose `g_` scalar beside one; add a field to the struct that owns it.
A module may hold several structs for separate concerns, as `tui.c` does. Only
a `volatile sig_atomic_t` written by a signal handler and state under
`AGENT_TESTING` stay loose. `make check-globals` rejects any global typed as a
primitive, pointer or function pointer; `static const` passes.

Per-turn and per-render state goes in its own struct that begin and end assign
whole, so a new field resets by construction. `RenderBlock` in `render.c` is
the model. When a struct is too big to assign, as with the ~680KB `MdState`,
reset fields explicitly and mark the reset function with `INVARIANT`.

Leave `g_persist`, `g_scratch` and `g_screen` as they are. Do not thread an
`App *` through every function: thousands of call sites, unreviewable, and it
lands on the paths `bench-guard` protects.

## Module map

- `core.c`: arenas, strings and buffers, logging, time, file reads.
- `json.c`: JSON parser and serializer.
- `http.c`: libcurl requests, streaming SSE, idle polling.
- `paths.c`: XDG paths and the `.arqan` project directory chain.
- `settings.c`: the TOML-subset format, upserts, state writes.
- `config.c`: settings table, source precedence, `Config`, `UiPrefs`. A new
  setting is a row in `k_conf`, not a new reader.
- `endpoints.c`: provider definitions.
- `favorites.c`: `/model` pins.
- `prompt.c`: system prompt discovery and expansion.
- `cli.c`: command-line parsing.
- `tools.c`: the registry and the built-in tools.
- `spill.c`: full output of a paged tool, in a temporary file.
- `media.c`: images attached to a turn.
- `clipboard.c`: clipboard images via `wl-paste`, `xclip` or `pngpaste`, under
  a deadline and with no shell.
- `provider.c`: OpenAI and Anthropic requests and responses.
- `history.c`, `session.c`: prompt and conversation persistence.
- `telemetry.c`: anonymized diagnostics. Session shape only; never pass user or
  model content, real paths, tool arguments, or URLs to `tel_str`.
- `tui.c`, `markdown.c`, `render.c`: terminal UI and transcript rendering.
- `main.c`: command handling and the provider/tool loop.

Read `agent.h` and the module you are changing first, and follow its ownership,
bounds and rendering conventions instead of building a second path.

## Important behaviour

- Settings resolve through one table in `config.c`: defaults, XDG config files,
  `$cwd/.arqan/config.toml` and the directories above it, the state file, the
  active provider, `ARQAN_<NAME>`, then CLI options.
- A project config file is untrusted. It may not set `api_key`, a key-store
  directive, or `images`. A rejected value is reported and dropped, never
  clamped.
- Keys from `/provider` live in the credentials file at mode 0600. Refuse that
  file if group or other permissions are set, and never copy the keys into
  shared config.
- Prompts are documents, not config values. Project `AGENTS.md` files are
  appended to the chosen system prompt.
- Tool output is replayed to the provider, so bound it and say how to get the
  rest. Refuse oversized arguments instead of truncating them.
- What a page leaves out goes to a spill file the result names. Spilling is
  best effort and never changes what a tool answers.
- An image over `AGENT_MAX_IMAGE_BYTES` or `AGENT_MAX_IMAGE_SIDE` is refused
  with the limit named. Nothing is resized, and no pixel decoder is added. A
  turn with no media keeps the plain-string content shape on the wire.
- `images = off` leaves `Conv.media` absent, so no image reaches the model,
  including one a resumed session saved, and `/attach` is not registered.
- The registry enforces tool availability, including plan mode and disabled
  tools. Prompts must describe the tools actually offered.
- The agent runs tool rounds until it finishes or is interrupted. No round
  limit. Never retry a partial stream.
- The transcript renders from `Conv`. Anything that must survive replay or
  session persistence belongs in conversation state.
- TUI output goes through the existing paint and buffer APIs. Keep long waits
  single-threaded and responsive through their idle hooks.

## Comments

Do not comment code. Name things so it reads on its own, and delete any comment
that restates the next line. Three kinds stay:

- File header doc comments.
- Separators that split a long file into sections.
- `TODO` for deferred work with the reason, `NOTE` for a constraint the code
  cannot show, `INVARIANT` for something a later edit would break.

The last kind must carry what the code cannot: a size that forbids a copy, a
reset that happens elsewhere, why an obvious change is wrong. Otherwise delete
it.

## Writing for users

For replies, commit messages, docs and changelog entries alike:

- Plain English, short sentences, active voice.
- Simple words. Do not reach for a rare one, and do not vary wording for
  variety: use the same term for the same thing.
- No preamble, no summary of what you are about to say, no restating the
  question.
- ASCII only. No smart quotes, em dashes, or emoji. No AI or tool attribution.
- Say when something failed, is uncertain, or went untested. Never claim a test
  passed unless you ran it.
- Name the exact command you ran and show paths as paths.
- Answer what was asked. Offer extra work as a suggestion; do not do it
  unasked.
- When a decision is the user's, ask, and offer concrete options.

README.md and AGENTS.md must not be edited without explicit user approval.

## Handoff

If the task changed files Git tracks or would track, suggest one commit
message:

```
(<type>[+<type>...]): imperative lowercase description
```

Parentheses are required; the description is imperative, lowercase, no trailing
period. Use the most specific type, and prefer one: `feat`, `fix`, `refactor`,
`perf`, `test`, `docs`, `style`, `build`, `ci`, `revert`, `chore`. No
attribution, trailers, or tool signatures.

## Releases

`src/agent.h` holds the only version, `AGENT_VERSION`. Use `X.Y.Z` and a
matching `vX.Y.Z` tag. Follow Semantic Versioning, except that before 1.0 a
breaking or substantial release bumps minor.

Keep `CHANGELOG.md` in Keep a Changelog format. Put notable user-visible
changes under `Unreleased`; skip refactors, tests and formatting. On release,
move them under a dated `X.Y.Z` heading and update the comparison links.

Write entries without a commit link, since a commit cannot contain its own
hash. `python3 scripts/changelog-links.py` adds them and `--check` lists what
is still bare. It matches an entry by its first line, so a commit that only
reworded the changelog can claim it; check the links and retarget those.

To release:

1. In one commit, update `AGENT_VERSION` and `CHANGELOG.md`, and refresh the
   golden files with `python3 tests/run.py --update`.
2. Run `scripts/changelog-links.py`, then `scripts/release-linux.sh`.
3. Merge the pull request. `main` is protected, so the squash merge gives the
   commit a new hash: tag the merged commit, never the local one, and push the
   annotated tag afterwards. Never move or reuse a published tag.
4. Review the draft GitHub Release, use the changelog section as its notes,
   check the packages against `SHA256SUMS`, and publish by hand.
