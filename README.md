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

The same values can live in `$XDG_CONFIG_HOME/yoke/config`:

```ini
base_url = https://api.openai.com/v1
model = gpt-4o-mini
api_key = sk-...
api = openai
```

`/provider` stores provider definitions in that config and keys in
`$XDG_STATE_HOME/yoke/credentials` (mode 0600). Command-line options override
environment, saved choices, and config. Run `yoke --help` for all options.

## Use

- Enter sends; Alt+Enter adds a line. Ctrl-C or Esc cancels; Ctrl-D quits an
  empty composer.
- `!command` runs a local shell command. Prefix with `\!` to send it to the
  model instead.
- `@` opens the project file picker.
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

yoke follows XDG paths: config is under `yoke/config`, state includes history,
credentials, and UI choices, and sessions are under
`$XDG_DATA_HOME/yoke/sessions`. Optional anonymized telemetry is off by
default and never includes prompt, tool-argument, raw-path, or endpoint data.

## Development

The app is a unity build: `src/main.c` includes all implementation files and
`src/yoke.h` is the shared header. Application memory comes from startup
arenas, not `malloc`. See [tests/README.md](tests/README.md) for the TUI test
harness and writing cases.
