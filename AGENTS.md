## Project

`arqan` is a C17 terminal coding agent for OpenAI-compatible Chat Completions
and Anthropic Messages APIs. It streams SSE responses and exposes built-in
read, write, bash, patch, grep, and find tools.

## Build and test

```sh
make             # build bin/arqan
make run         # build and run
make test        # end-to-end TUI suite
make bench       # benchmark and stress suite
make bench-guard # measure this tree against the commit under it
make test-asan   # ASan + UBSan suite
make test-fil    # Fil-C memory-safety suite (needs /opt/fil)
make clean       # remove build/ and bin/
```

`CFLAGS` already enables `-Wall -Wextra -Wpedantic -Wconversion`; new warnings
are failures, and CI builds with `EXTRA_CFLAGS=-Werror`. After every change
under `src/`, run `make test`. Behavioural changes need a focused end-to-end
case in `tests/cases/`; write bug regressions first. Do not weaken assertions
or update golden files merely to hide a failure. For suspected flakes, use
`python3 tests/run.py --repeat 5`.

Startup time, throughput and footprint are user-visible behaviour: a change
that costs any of them is a regression. `make bench-guard` builds and measures
the commit under this tree, then this tree against it, and fails when CPU per
operation grows past 1.4x or private memory past 1.15x. CI runs it on every
pull request. Run it before proposing a change to the startup path, the render
path, tool output, or session replay. Do not widen a tolerance or a budget to
pass one. `bench/README.md` records what is measured and how.

`make test-asan` and `make test-fil` are not per-change gates; run one when the
change touches memory. Prefer `make test-fil` for arena, buffer, bounds and
pointer-arithmetic work and for any suspected memory bug: Fil-C carries bounds
on the pointer, so it catches an out-of-bounds access that lands inside another
valid object, which ASan cannot. It costs about ninety seconds beyond
`make test` and ships nothing: the build stays in `build/fil/` and `bin/fil/`,
and no release target reads them.

## Core design

- This is a unity build: `src/main.c` includes every `.c` file and the Makefile
  compiles one translation unit. Add new modules to `main.c`, not the Makefile.
- `src/agent.h` is the sole cross-module header. Put shared types and function
  declarations there.
- Two names, kept apart. Code, macros and file names use the neutral internal
  prefix `agent`/`agent_`/`AGENT_`/`Agent`; anything a user sees (the binary,
  `ARQAN_*` environment variables, XDG and `.arqan` directories, help text,
  docs) uses the product name through `AGENT_NAME` and `AGENT_ENV_PREFIX`.
  Never write the product name as a bare literal where those macros reach, and
  rename with `python3 scripts/rename.py` rather than by hand.
- No application `malloc` or `free`. Use the startup arenas: `persist` for
  session-lifetime data and `scratch` for temporary turn data.
- Allocation failure is normal. Check `arena_alloc`, `buf_ok`, and collection
  return values; validate external sizes before allocating and use
  `arena_alloc_array` for multiplied sizes.
- Keep hot collections as structure-of-arrays. Extend `Conv` and
  `ToolRegistry` with parallel arrays.
- Route owned file reads through `file_read`. Build updates atomically through
  a temporary file and rename where the existing code does so.

## Module map

- `core.c`: arenas, strings/buffers, logging, time, file reads.
- `json.c`: arena-backed JSON parser and serializer.
- `http.c`: libcurl requests, streaming SSE, idle polling.
- `paths.c`: XDG paths and the `.arqan` project directory chain.
- `settings.c`: the TOML-subset file format, upserts, state writes.
- `config.c`: the settings table, source precedence, `Config` and
  `UiPrefs`. A new setting is a row of `k_conf`, not a new reader.
- `endpoints.c`: provider definitions, one `[providers.<name>]` section each.
- `favorites.c`: the models `/model` pins, one state-file section per provider.
- `prompt.c`: system prompt discovery and expansion.
- `cli.c`: command-line parsing.
- `tools.c`: registry and built-in tool implementations.
- `spill.c`: full output of a paged tool, written to a temporary file the
  result names.
- `provider.c`: OpenAI/Anthropic request and response handling.
- `history.c`, `session.c`: local prompt and conversation persistence.
- `telemetry.c`: anonymized diagnostics. Record session shape only; never pass
  user/model content, raw paths, tool argument values, or endpoint URLs to
  `tel_str`.
- `tui.c`, `markdown.c`, `render.c`: terminal UI and transcript rendering.
- `main.c`: command handling and the provider/tool loop.

Read `agent.h` and the relevant module before changing behaviour. Preserve the
existing ownership, bounds, and rendering conventions rather than duplicating
them in a new path.

## Important behaviour

- Settings resolve through one table in `config.c`: defaults, the XDG config
  files, `$cwd/.arqan/config.toml` and the directories above it, the state
  file, the active provider, `ARQAN_<NAME>`, then CLI options. A project
  file is untrusted input: it may not set `api_key` or any key-store
  directive. A refused value is reported and dropped, never clamped.
- Keys managed by `/provider` live in the credentials file at mode 0600;
  refuse that file when group or other permissions are present, and never
  copy those keys into shared config.
- Prompts are documents, not config values. Project `AGENTS.md` files are
  appended to the chosen system prompt.
- Tool output is replayed to the provider, so it must be bounded and explain
  how to retrieve omitted data. Refuse oversized operation arguments rather
  than silently truncating them.
- What a page leaves out goes to a spill file the result names, so the next
  call can narrow it on disk. Spilling is best effort and never changes what
  a tool answers.
- Tool availability is registry-enforced, including plan mode and disabled
  tools; prompts must describe the tools actually offered.
- The agent keeps running tool rounds until completion or interruption; do not
  add a round limit. Provider retry policy must not retry partial streams.
- The transcript is rendered from `Conv`; add conversation state for anything
  that must survive replay or session persistence.
- TUI output must use its existing paint/buffer APIs. Keep long waits
  single-threaded and responsive through their idle hooks.

## Code and docs

Use C17 and the project's established style. Keep comments only when they
state an invariant, rationale, contract, or wire format the code cannot make
clear. Write concise active sentences in ASCII; no padded preambles, smart
punctuation, emoji, or AI/tool attributions. Header doc comments describe
ownership, arena, and failure behaviour.

README.md and AGENTS.md *must not* be edited without explicit user approval.

## Handoff

If the task created or modified files that Git tracks or would track, suggest
one commit message:

```
(<type>[+<type>...]): imperative lowercase description
```


Parentheses are mandatory; the description is imperative, lowercase, no
trailing period. No attribution of any kind: no `Co-Authored-By`, no
"Generated with" trailers, no tool signatures.

Use the most specific type: `feat`, `fix`, `refactor`, `perf`, `test`, `docs`,
`style`, `build`, `ci`, `revert`, or `chore`. Prefer one type and never add
assistant/tool attribution or trailers.

## Releases

- `src/agent.h` contains the sole product version as `AGENT_VERSION`. Use
  numeric `X.Y.Z` versions and a matching `vX.Y.Z` release tag.
- Follow Semantic Versioning: increment patch for fixes, minor for compatible
  features, and major for breaking changes. Before 1.0, increment minor for
  breaking or substantial feature releases.
- Maintain `CHANGELOG.md` using Keep a Changelog. Add notable user-visible
  changes under `Unreleased`; do not list routine refactors, tests, or
  formatting. When releasing, move those entries under a dated `X.Y.Z`
  heading and update comparison links.
- Write a changelog entry without a commit link; a commit cannot carry its
  own hash. `python3 scripts/changelog-links.py` links every bare entry to
  the commit that introduced it, and `--check` reports the ones still bare.
- Prepare a release by updating `AGENT_VERSION` and `CHANGELOG.md` in one
  commit, running `scripts/changelog-links.py` and then
  `scripts/release-linux.sh`, then creating and pushing the matching
  annotated tag. Never move or reuse a published version tag.
- Review the generated draft GitHub Release, use the matching changelog
  section as its curated notes, verify both attached files, and publish it
  manually.
