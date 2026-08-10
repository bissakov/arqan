# yoke

`yoke` is a C17 terminal coding agent for OpenAI-compatible Chat Completions
and Anthropic Messages APIs. It streams replies in a fullscreen TUI and can
read, write, patch, search, and run shell commands.

## Build

```sh
make                 # bin/yoke and bin/yoke-highlight
make minimal         # bin/yoke only
make test
make test-asan
```

Run `./bin/yoke`, or supply a prompt for a non-interactive response:

```sh
yoke -p "summarise src/tui.c"
yoke --disable-tools bash,write,patch
```

## Configure

On first launch, use `/provider` to add an endpoint. Alternatively set:

```sh
export YOKE_BASE_URL=https://api.openai.com/v1
export YOKE_MODEL=gpt-4o-mini
export YOKE_API_KEY=sk-...
export YOKE_API=openai       # or anthropic
```

Every setting is `YOKE_<NAME>` in the environment and `<name>` in a config
file, which is TOML:

```toml
# $XDG_CONFIG_HOME/yoke/config.toml
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

A project overrides that in `.yoke/config.toml`, which is looked for in the
working directory and every directory above it, nearest last. It arrives with
a `git clone`, so it may not set `api_key` or anything naming a key store;
such a line is reported and ignored.

Precedence, lowest first: defaults, `$XDG_CONFIG_DIRS/yoke/config.toml`,
`$XDG_CONFIG_HOME/yoke/config.toml`, `.yoke/config.toml`, remembered UI
choices in `$XDG_STATE_HOME/yoke/state.toml`, the active provider's section,
`YOKE_*`, then command-line options. `/provider` writes provider definitions
to the user's config and keys to `$XDG_STATE_HOME/yoke/credentials.toml`
(mode 0600). Run `yoke --help` for all options.

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

yoke builds the helper's command line itself, looking the key up under the
service `yoke` and the account `<provider>`: `secret-tool` for
`secret-service`, `pass` under `yoke/<provider>`, and `security` for the macOS
`keychain`. For a store with no entry above, `key_source = command` runs the
section's own `key_command` and reads the key from its first output line.

A source directive says what yoke executes, so it is read only from the
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

The built-in tools are `read`, `write`, `bash`, `patch`, `grep`, and `find`.
Disable tools with `/settings`, `YOKE_DISABLE_TOOLS`, or `--disable-tools`.

## Prompts and files

Project prompts live in `.yoke/SYSTEM.md`; global prompts live in
`$XDG_CONFIG_HOME/yoke/SYSTEM.md`. Plan mode uses `PLAN.md` in the same
locations. `AGENTS.md` files between the working directory and root are
appended as project instructions. Prompt placeholders are `{tools}` and
`{cwd}`.

yoke follows XDG paths: config is under `yoke/config.toml`, state holds
history, credentials, and UI choices, and sessions are under
`$XDG_DATA_HOME/yoke/sessions`. Optional anonymized telemetry is off by
default and never includes prompt, tool-argument, raw-path, or endpoint data.

## Development

The app is a unity build: `src/main.c` includes all implementation files and
`src/yoke.h` is the shared header. Application memory comes from startup
arenas, not `malloc`. See [tests/README.md](tests/README.md) for the TUI test
harness and writing cases.
