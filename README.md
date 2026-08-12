# arqan

`arqan` is a C17 terminal coding agent for OpenAI-compatible Chat Completions
and Anthropic Messages APIs. It streams replies in a fullscreen TUI and can
read, write, patch, search local files and the public web, fetch web pages,
and run shell commands.

## Build

```sh
make                 # bin/arqan and bin/arqan-highlight
make minimal         # bin/arqan only
make test
make test-asan
```

Building requires a C17 compiler and libcurl development files. Lexbor 3.0.0
is vendored and built as a separate object; no system Lexbor package, browser,
JavaScript runtime, API key, or web-search daemon is needed.

Run `./bin/arqan`, or supply a prompt for a non-interactive response:

```sh
arqan -p "summarise src/tui.c"
arqan --disable-tools bash,write,patch
arqan --disable-tools internet_search,page_fetch
```

## Configure

On first launch, use `/provider` to add an endpoint. Alternatively set:

```sh
export ARQAN_BASE_URL=https://api.openai.com/v1
export ARQAN_MODEL=gpt-5.6-sol
export ARQAN_API_KEY=sk-...
export ARQAN_API=openai       # or anthropic
```

Every setting is `ARQAN_<NAME>` in the environment and `<name>` in a config
file, which is TOML:

```toml
# $XDG_CONFIG_HOME/arqan/config.toml
base_url = "https://api.openai.com/v1"
model = "gpt-4o-mini"
api_key = "sk-..."
api = "openai"
max_tokens = 32768
stream = true

[providers.openai]
base_url = "https://api.openai.com/v1"
model = "gpt-4o-mini"
api = "openai"
```

A project overrides that in `.arqan/config.toml`, which is looked for in the
working directory and every directory above it, nearest last. It arrives with
a `git clone`, so it may not set `api_key` or anything naming a key store;
such a line is reported and ignored.

Precedence, lowest first: defaults, `$XDG_CONFIG_DIRS/arqan/config.toml`,
`$XDG_CONFIG_HOME/arqan/config.toml`, `.arqan/config.toml`, remembered UI
choices in `$XDG_STATE_HOME/arqan/state.toml`, the active provider's section,
`ARQAN_*`, then command-line options. `/provider` writes provider definitions
to the user's config and keys to `$XDG_STATE_HOME/arqan/credentials.toml`
(mode 0600). Run `arqan --help` for all options.

## Performance

Measured at `0bf0805` on Linux 7.1.6 with GCC 16.1.1 and a Ryzen 7 7800X3D.
Build times are medians of three clean builds; runtime tests use the real TUI
and agent loop against the local mock provider, excluding network and model
latency.

- Clean build: 1.41 s for `make -j16 minimal`; 2.92 s for `make -j16`.
- Executables: 356 KiB for `bin/arqan`; 12.6 MiB for `bin/arqan-highlight`.
- Startup: 4.53 ms median and 5.19 ms p95 to the first TUI frame over 20 runs.
- Idle memory: 1.94 MiB private dirty; 11.0 MiB resident including shared and
  file-backed pages.
- Simulated coding work: eight turns using `read`, `grep`, `find`, and `bash`
  across 16 provider requests took 1.59 s wall and 0.017 s CPU; the session
  ended at 2.95 MiB private dirty with 14.4 MiB peak RSS.
- Large context: resuming a 1.003M-token, 4.0 MB session took 0.24 s wall and
  0.04 s CPU at 13.5 MiB private dirty. Four switches between 1M- and
  200k-token sessions left that footprint unchanged.
- Test suite: 586/586 end-to-end cases passed in 28.9 s; the same 586 passed
  under ASan and UBSan in 29.4 s.

Private dirty is memory written for this process alone; RSS also counts
resident shared libraries and file-backed pages.

## License

Except where otherwise noted, arqan is licensed under the
[Mozilla Public License 2.0](LICENSE). Vendored Tree-sitter components retain
their upstream licenses under
[`vendor/tree-sitter/licenses/`](vendor/tree-sitter/licenses/). The vendored
Lexbor 3.0.0 HTML parser retains its Apache-2.0 license and notice in
[`vendor/lexbor/`](vendor/lexbor/).

## Development

The app is a unity build: `src/main.c` includes all implementation files and
`src/agent.h` is the shared header. Application memory comes from startup
arenas, not `malloc`. See [tests/README.md](tests/README.md) for the TUI test
harness and writing cases.

`make bench` measures the same binary through the same harness: cost per
keystroke, per delta, per turn and per tool call, plus stress cases that only
assert survival. See [bench/README.md](bench/README.md).
