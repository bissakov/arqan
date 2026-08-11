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
export ARQAN_MODEL=gpt-4o-mini
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

### External key stores

`/provider` can leave the key to an external store instead, so no plaintext
key is written anywhere. It asks which store to use right after a key is
entered, so a provider that needs no key is never asked. To move a key that
is already stored, use `+ edit a provider` and choose **Move**: that names the
store holding it now and writes it to another without asking for the value
again. The credentials file then records only which store to ask:

```toml
[providers.openai]
key_source = "secret-service"   # or: pass, keychain, file
```

arqan builds the helper's command line itself, looking the key up under the
service `arqan` and the account `<provider>`: `secret-tool` for
`secret-service`, `pass` under `arqan/<provider>`, and `security` for the macOS
`keychain`. For a store with no entry above, `key_source = command` runs the
section's own `key_command` and reads the key from its first output line.

A source directive says what arqan executes, so it is read only from the
credentials file: that file is mode 0600, machine-local, and not the one a
dotfile repository carries. The same keys in the shared config file are
ignored with a warning, and no helper is ever run through a shell, so a value
in either file cannot become a pipeline or a substitution. `key_command` is
split on whitespace; anything needing quoting belongs in a wrapper script.

## Use

- Enter sends; Alt+Enter adds a line. Ctrl-C or Esc cancels; Ctrl-D quits an
  empty composer.
- `!command` runs a local shell command. Prefix with `\!` to send it to the
  model instead.
- `@` opens the project file picker.
- Ctrl-R (or `/find`) searches the transcript: Enter or Up walks older matches,
  Down newer, Esc closes. The reach is the scrollback's, and Ctrl-E lifts the
  caps on tool output when a match may be hiding under one.
- `/` opens commands: `/clear`, `/resume`, `/fork`, `/model`, `/provider`,
  `/mode`, `/settings`, `/help`, and `/exit`.
- Shift+Tab toggles Build and Plan modes. Plan mode can inspect but cannot
  write or patch.
- While a turn runs, `/settings`, `/statusline`, `/about` and `/copy` still
  work; a message waits in the composer, and a setting the running request
  reads changes at the next prompt.

The built-in tools are `read`, `write`, `bash`, `patch`, `grep`, `find`,
`internet_search`, and `page_fetch`. Web results are untrusted reference content;
searching never fetches a result automatically. For example, an agent can call
`internet_search` with `{"query":"site:example.com C17"}` and then call `page_fetch`
on one returned public HTTP(S) URL. Disable either tool with `/settings`,
`ARQAN_DISABLE_TOOLS`, or `--disable-tools`.

Search requests are spaced at least ten seconds apart. A challenge, HTTP 202,
403, or 429 pauses further searches in that arqan process for one hour; arqan
does not retry or attempt to solve service challenges.

A tool result is replayed on every later turn, so each call returns a bounded
page. When `bash`, `grep`, `find`, `page_fetch`, or `internet_search` leaves
something out, the whole output is written to
`$TMPDIR/arqan-<tool>-<hash>.log` (`.txt` for the others) at mode 0600 and the
result names that file, so the next call narrows it on disk instead of paging
it all back into the conversation, or fetching and searching again. The name
hashes the call, so repeating one overwrites its own file; a page that left
nothing out keeps no file.

## Prompts and files

Project prompts live in `.arqan/SYSTEM.md`; global prompts live in
`$XDG_CONFIG_HOME/arqan/SYSTEM.md`. Plan mode uses `PLAN.md` in the same
locations. `AGENTS.md` files between the working directory and root are
appended as project instructions. Prompt placeholders are `{tools}` and
`{cwd}`.

arqan follows XDG paths: config is under `arqan/config.toml`, state holds
history, credentials, and UI choices, and sessions are under
`$XDG_DATA_HOME/arqan/sessions`. Optional anonymized telemetry is off by
default and never includes prompt, tool-argument, raw-path, or endpoint data.

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
