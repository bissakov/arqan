# arqan

`arqan` is a terminal coding agent written in C17. It talks to any
OpenAI-compatible Chat Completions API or to the Anthropic Messages API, and
runs in a fullscreen TUI.

The model can read, search, write and patch files, run shell commands, search
the web and fetch pages. It can also ask you to pick between options, keep a
step list, and hand a read-only question to a subagent. You approve file
changes and shell commands unless you turn that off.

Linux x86_64 only.

## Install

| Package | Needs |
| --- | --- |
| `.deb` (Debian, Ubuntu) | glibc 2.31+, libcurl 7.66+ (`libcurl.so.4`) |
| `.rpm` (Fedora, RHEL and similar) | glibc 2.34+, libcurl 7.66+ (`libcurl.so.4`) |
| `.pkg.tar.zst` (Arch and similar) | glibc 2.34+, libcurl 7.66+ (`libcurl.so.4`) |
| `.tar.gz` (portable) | nothing but the kernel |

All four need a CA certificate bundle at run time.

### From the package repositories

The signed repositories live at <https://bissakov.github.io/arqan>. That page
also shows the fingerprint of the signing key. Your package manager then
handles upgrades. The repositories keep the ten newest releases; older ones
are on the GitHub release page.

Debian and Ubuntu:

```sh
sudo install -d -m 0755 /etc/apt/keyrings
sudo curl -fsSLo /etc/apt/keyrings/arqan-archive-keyring.asc \
    https://bissakov.github.io/arqan/arqan-archive-keyring.asc
sudo curl -fsSLo /etc/apt/sources.list.d/arqan.sources \
    https://bissakov.github.io/arqan/deb/arqan.sources
sudo apt update && sudo apt install arqan
```

Fedora, RHEL and similar:

```sh
sudo rpm --import https://bissakov.github.io/arqan/arqan-archive-keyring.asc
sudo curl -fsSLo /etc/yum.repos.d/arqan.repo \
    https://bissakov.github.io/arqan/rpm/arqan.repo
sudo dnf install arqan
```

Arch and similar. Check that the fingerprint below matches the one on the
repository page before you sign the key:

```sh
curl -fsSLo /tmp/arqan-archive-keyring.asc \
    https://bissakov.github.io/arqan/arqan-archive-keyring.asc
sudo pacman-key --add /tmp/arqan-archive-keyring.asc
sudo pacman-key --lsign-key 01B3FA5FA91E2672872EC2710430B0C28F7AFB0A
sudo tee -a /etc/pacman.conf >/dev/null <<'EOF'

[arqan]
Server = https://bissakov.github.io/arqan/arch/$arch
EOF
sudo pacman -Syu arqan
```

### From a release

Download `SHA256SUMS` and one package from the GitHub release page. Check it:

```sh
sha256sum --ignore-missing -c SHA256SUMS
```

Then install it:

```sh
sudo apt install ./arqan_X.Y.Z-1_amd64.deb             # Debian, Ubuntu
sudo dnf install ./arqan-X.Y.Z-1.x86_64.rpm            # Fedora, RHEL
sudo pacman -U ./arqan-X.Y.Z-1-x86_64.pkg.tar.zst      # Arch
```

To upgrade, run the same command with the newer package.

The portable archive holds static musl builds. Use it when no package fits:
a musl distribution, an older glibc, or a container with no libcurl. It runs
in place:

```sh
tar -xzf arqan-X.Y.Z-linux-x86_64.tar.gz
./arqan-X.Y.Z-linux-x86_64/bin/arqan
```

If it cannot find the system CA store, point `SSL_CERT_FILE` or
`SSL_CERT_DIR` at it.

### Remove

```sh
sudo apt remove arqan       # Debian, Ubuntu
sudo dnf remove arqan       # Fedora, RHEL
sudo pacman -R arqan        # Arch
```

Removing the package does not touch your files. These stay:

- config in `${XDG_CONFIG_HOME:-$HOME/.config}/arqan`
- state and API keys in `${XDG_STATE_HOME:-$HOME/.local/state}/arqan`
- sessions in `${XDG_DATA_HOME:-$HOME/.local/share}/arqan`
- `.arqan` directories in your projects

## First run

Run `arqan`. It asks for a provider on first start. Use `/provider` to add a
connection (base URL, API type and key), then `/model` to pick a model.

## Use

Type a message and press Enter. `/` opens the command list and `@` opens the
file picker.

### Modes and permissions

- **Build mode** is the default. The model can use every tool.
- **Plan mode** allows only tools that read. The model ends with a plan for
  you to approve. Switch with `/mode` or Shift+Tab.

With `permissions = "ask"`, the default, arqan asks before it runs a shell
command, writes or patches a file, calls an MCP tool, or reads outside the
project. You can approve one call or the whole class until arqan exits. With
`permissions = "free"` it never asks. Change it in `/settings`.

A shell command made only of reading programs runs without asking. On Linux it
runs under Landlock with writes and the network denied, and reads only the
project and the system directories. No child process gets your API keys. Set
`ARQAN_DUMPABLE=1` to allow core dumps and debugger attach.

### Commands

| Command | Does |
| --- | --- |
| `/clear` (`/new`) | Start a new conversation |
| `/resume` | Resume or delete a saved session from this directory |
| `/fork` | Continue in a copy of this session |
| `/rewind` | Go back to an earlier message and edit it |
| `/compact` | Summarize this session and continue in a new one |
| `/title` | Name this session; `/title auto` lets the small model do it |
| `/export` | Export this session as Markdown |
| `/model` | Pick a model from any provider |
| `/provider` | Add, edit or remove a provider |
| `/mode` | Switch between Build and Plan mode (Shift+Tab) |
| `/attach` | Attach an image, by path or from the clipboard (Ctrl-V) |
| `/copy` | Copy the last reply to the clipboard |
| `/find` (`/search`) | Search the transcript (Ctrl-R) |
| `/todo` | Show the model's step list |
| `/task` | Show what the subagent is doing (Ctrl-O) |
| `/mcp` | List, approve, reject, restart or disable MCP servers |
| `/settings` (`/config`) | Change settings |
| `/statusline` | Choose what the status line shows |
| `/keys` | Show the keyboard shortcuts |
| `/help` | Ask the model how to use arqan |
| `/about` | Version and contributors |
| `/restart` | Restart arqan |
| `/exit` (`/quit`) | Quit |

`/attach` exists only when `images` is not `off`, `/task` only when
`subagents` is on, and `/mcp` only when `mcp` is on.

### One-shot runs

```sh
arqan -p "summarise src/tui.c"
arqan -p "list the TODOs" --disable-tools bash,write,patch
```

`-p` runs one turn and prints the reply. In `ask` mode there is no one to
approve a call, so shell commands and file changes are refused. Set
`permissions = "free"` only for input you trust. Run `arqan --help` for all
options.

### Images

`/attach` sends an image with your next message, to a model that can see
images. Give a path, pick a file with `@`, or paste from the clipboard with
Ctrl-V (needs `wl-paste`, `xclip` or `pngpaste`). An image over the size limit
is refused, not resized. Set `images = "off"` to send no images at all.

### Sessions

Every conversation is saved per working directory, under
`${XDG_DATA_HOME:-$HOME/.local/share}/arqan/sessions/`. Use `/resume` to go
back to one, or set `resume_last = true` to start in the newest.

When the context fills up, arqan first replaces old tool output with a short
note (`elide_at`, 75% of the window by default). At `compact_at` (85%) it
summarizes the session and continues in a new one. Set `compact = "manual"`
to only compact with `/compact`, or `"off"` to never do it.

## Configure

### Providers and keys

A provider is a connection: a base URL, an API type and a key. A model is a
pair of provider and model name. `/model` lists models from every provider,
so you never switch provider by hand.

`/provider` saves the connection to your config file and the key to
`${XDG_STATE_HOME:-$HOME/.local/state}/arqan/credentials.toml` with mode 0600.
arqan refuses that file if group or others have any permission on it. Instead of the file,
`/provider` can keep the key in the Secret Service (`secret-tool`), in
`pass`, in the macOS keychain, or read it from a command you give.

You can also skip `/provider` and use the environment:

```sh
export ARQAN_BASE_URL=https://api.openai.com/v1
export ARQAN_API=openai       # or anthropic
export ARQAN_MODEL=example-model
export ARQAN_API_KEY=sk-...
```

### Config files

Config files are TOML. Every setting is `name` in a file and `ARQAN_NAME` in
the environment.

```toml
# ~/.config/arqan/config.toml
provider = "openai"        # the [providers.<name>] section to use
model = "example-model"
max_tokens = 32768
permissions = "ask"

[providers.openai]
base_url = "https://api.openai.com/v1"
api = "openai"             # or "anthropic"
model = "example-model"    # default model for this provider
small_model = "example-small-model"
```

A setting can come from several places. Later ones win:

1. built-in defaults
2. `$XDG_CONFIG_DIRS/arqan/config.toml`
3. `$XDG_CONFIG_HOME/arqan/config.toml`
4. `.arqan/config.toml` in the working directory and each directory above
   it; the nearest one wins
5. choices remembered in `$XDG_STATE_HOME/arqan/state.toml`, such as the
   model you picked
6. the chosen provider's section
7. `ARQAN_*` environment variables
8. command-line options

### Project config

A `.arqan/config.toml` comes with a `git clone`, so arqan does not trust it.
It may not set anything that sends data or a key somewhere, runs a command,
or widens what the model may do. These settings are refused in a project file:

`base_url`, `api`, `api_key`, `permissions`, `telemetry`, `notify_command`,
`search_endpoint`, `search_api_key`, `search_engine_id`, `small_provider`,
`stream_timeout_ms`, `ask_timeout_ms`, `shell_timeout_ms`, `images`,
`cache_guard`, `subagent_tasks`, `subagent_slice_ms`, `mcp`,
`mcp_timeout_ms`.

A refused line is reported and dropped. A provider defined in a project file
gets no API key and may not redefine a provider you configured. arqan also
ignores a project file, prompt or `AGENTS.md` that belongs to another user or
that others can write to.

### Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `provider` | | Provider section to use |
| `model` | | Model name |
| `base_url` | | API base URL |
| `api` | `openai` | `openai` or `anthropic` |
| `api_key` | | API key |
| `max_tokens` | `32768` | Most tokens one reply may use |
| `max_messages` | `4096` | Most messages in one conversation |
| `stream` | `true` | Show a reply as it arrives |
| `mode` | `build` | `build` or `plan` |
| `permissions` | `ask` | `ask` or `free` |
| `disable_tools` | | Comma-separated tools the model may not call |
| `retries` | `4` | Retries for a failed request |
| `retry_delay_ms` | `2000` | Delay before the first retry; it doubles each time |
| `stream_timeout_ms` | `300000` | How long a streamed reply may send nothing before it fails; `0` for no limit |
| `shell_timeout_ms` | `120000` | Longest wait for a shell command |
| `ask_timeout_ms` | `180000` | How long a question with a recommended answer waits for you |
| `small_model` | | Cheaper model for titles, and optionally compaction and subagents |
| `small_provider` | | Provider of `small_model` |
| `auto_title` | `true` | Name a session after its first turn (needs `small_model`) |
| `resume_last` | `false` | Start in this directory's newest session |
| `compact` | `auto` | `off`, `manual` or `auto` |
| `compact_at` | `85` | Percent of the context window that triggers compaction |
| `elide_at` | `75` | Percent at which old tool output is replaced by a note |
| `compact_model` | `main` | `main` or `small` |
| `cache_guard` | `stop` | On a prompt cache rebuild arqan did not cause: `stop`, `warn` or `off` |
| `subagents` | `true` | Offer the `task` tool |
| `subagent_model` | `main` | `main` or `small` |
| `subagent_tasks` | `1` | Subagent tasks that may run at once |
| `subagent_slice_ms` | `120000` | How long a subagent task runs before it reports back; `0` for no limit |
| `images` | `auto` | `auto` or `off` |
| `search_backend` | `auto` | See [Web search](#web-search) |
| `search_endpoint` | | SearXNG URL |
| `search_api_key` | | Brave API or Google key |
| `search_engine_id` | | Google engine ID |
| `mcp` | `false` | Start MCP servers from `mcp.json` |
| `mcp_timeout_ms` | `30000` | Timeout for an MCP request |
| `notify` | `osc9` | `off`, `bel`, `osc9` or `both` |
| `notify_command` | | Command to run on a notification |
| `notify_min_ms` | `10000` | Notify only for turns longer than this |
| `telemetry` | `false` | Write a local debug log |
| `wrap` | `word` | `word` or `justified` |
| `verbose_tools` | `false` | Show every line of tool output |
| `raw_markdown` | `false` | No Markdown or syntax highlighting |
| `show_ignored` | `false` | Offer files that `.gitignore` and `.ignore` exclude |
| `show_instructions` | `false` | Show the system prompt and `AGENTS.md` in the transcript |
| `status_fields` | `2047` | Status line fields, as a bit mask; set it with `/statusline` |

### System prompt and AGENTS.md

arqan uses the first system prompt it finds:

1. `--system` or `ARQAN_SYSTEM_PROMPT`
2. `.arqan/SYSTEM.md` in the working directory or a directory above it
3. `SYSTEM.md` in the arqan config directory
4. the built-in prompt

Plan mode reads `PLAN.md` from the same places instead.

Every `AGENTS.md` from the working directory up to `/` is added after the
prompt. Where two conflict, the one nearer the working directory wins.

### Web search

`internet_search` needs no key by default. `search_backend` picks the engine.
The keyless engines run with strict safe search:

| Value | Needs |
| --- | --- |
| `auto` | nothing; tries DuckDuckGo, then Brave, then Bing |
| `ddg` | nothing |
| `brave` | nothing |
| `bing` | nothing |
| `brave_api` | `search_api_key` |
| `google` | `search_api_key` and `search_engine_id` |
| `searxng` | `search_endpoint` |

### MCP servers

Set `mcp = true`, then list servers in `mcp.json` in the arqan config
directory or in a project `.arqan` directory:

```json
{
  "servers": {
    "github": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-github"],
      "env": { "GITHUB_TOKEN": "$GITHUB_TOKEN" }
    },
    "linear": {
      "url": "https://mcp.linear.app/mcp",
      "auth": { "type": "bearer", "token": "$LINEAR_TOKEN" }
    }
  }
}
```

`command` starts a local server; `url` connects over HTTP. A server from a
project file does not start until you approve it with `/mcp approve <name>`.
The file is read again before each turn. See [docs/mcp.md](docs/mcp.md) for
the details.

### Notifications

When a turn ends or needs your input, arqan notifies through the terminal with
OSC 9 (`notify`). Set `notify_command` to run a program instead. It gets one
line of JSON on stdin with `kind`, `text` and `cwd`.

### Telemetry

Off by default. When on, arqan writes an anonymized debug log to
`${XDG_STATE_HOME:-$HOME/.local/state}/arqan/telemetry/` for you to attach to
a bug report. It records the shape of a session, not your messages, the
model's replies, file paths, tool arguments or URLs. Nothing is sent anywhere.

## tmux

arqan copies (`/copy` and mouse selection) with OSC 52 and notifies with
OSC 9. Both work over ssh with no helper program. tmux drops both by default.
Add to `~/.tmux.conf`:

```sh
set -s set-clipboard on        # allow copying
set -g allow-passthrough all   # allow notifications from any pane (tmux 3.4+;
                               # 'on' covers only visible panes)
```

`set-clipboard` also needs the outer terminal to have the `Ms` capability.
tmux adds it on its own only when `TERM` matches `xterm*`. For another
terminal, add `set -as terminal-features ',<term>:clipboard'`.

When you ssh to another host from a tmux pane, `TMUX` does not reach that
host, so arqan there finds tmux from `TERM` instead. This needs `TERM` to
start with `tmux`, as it does with `set -g default-terminal tmux-256color`.
With a `screen*` value, or when `TERM` is changed on the way, tmux drops the
notifications. tmux inside tmux is not supported.

arqan cannot confirm that a copy worked. Under tmux it tells you which option
must be on.

## Build from source

You need a C17 compiler and the libcurl development files. Lexbor 3.0.0 and
Tree-sitter are vendored.

```sh
make            # bin/arqan and bin/arqan-highlight
make minimal    # bin/arqan only
./bin/arqan
```

`arqan-highlight` does syntax highlighting in code blocks. arqan looks for it
next to its own binary, then on `PATH`. Without it, code shows without colour.

## Development

The app is a unity build: `src/main.c` includes every `.c` file, and
`src/agent.h` is the shared header. Memory comes from arenas set up at start,
not `malloc`. `AGENTS.md` has the full rules.

```sh
make test         # end-to-end TUI suite
make test-unit    # arena, string and JSON tests
make test-asan    # ASan and UBSan
make bench        # benchmarks and stress cases
make fmt          # format with clang-format
make check-format # check formatting only
```

`make fmt` needs clang-format 22, since other versions format differently:
`pipx install clang-format==22.1.8`. CI runs `make check-format`.

See [tests/README.md](tests/README.md) for writing test cases,
[bench/README.md](bench/README.md) for the benchmarks, and
[packaging/linux/README.md](packaging/linux/README.md) for building and
publishing release packages.

## Performance

Measured at `af04882` on Linux 7.2.0-1-cachyos with GCC 16.2.1 and a
Ryzen 7 7800X3D.
Build times are medians of three clean 16-job builds. Runtime measurements use
the real TUI and agent loop against the local mock provider, so they exclude
network and model latency. The harness measures the first visible frame before
its 60 ms quiet window and reports agent CPU time separately from wall time.

- Clean build: 1.90 s for `make -j16 minimal`; 2.50 s for `make -j16`.
- Executables: 1.28 MiB for `bin/arqan`; 12.40 MiB for
  `bin/arqan-highlight`.
- Startup: 1.8 ms median and 2.2 ms p95 to the first TUI frame over 20 runs;
  agent CPU was about 1.2 ms, and idle memory was 1.6 MiB private dirty with
  4.4 MiB peak RSS.
- Streaming: a 2,000-word reply in 500 deltas took 211 ms wall and 12.8 ms
  CPU, or 0.03 ms CPU per delta.
- Tool loop: six `read` rounds in one turn took 195 ms wall and 5.5 ms CPU,
  or 0.92 ms CPU per round.
- Large context: replaying a 400k-token, 1.7 MB session took 196 ms wall and
  14.2 ms CPU at 6.4 MiB private dirty with 9.2 MiB peak RSS.
- Benchmarks: all 61 default cases completed within budget in 122.6 s.
- Test suite: 988/988 end-to-end cases passed in 7.1 s.

Private dirty is memory written for this process alone; RSS also counts
resident shared libraries and file-backed pages. Run `make bench` to reproduce
the runtime measurements; see [bench/README.md](bench/README.md) for workloads,
metrics and slow stress cases.

## License

Except where otherwise noted, arqan is licensed under the
[Mozilla Public License 2.0](LICENSE). Vendored Tree-sitter components keep
their licenses in [`vendor/tree-sitter/licenses/`](vendor/tree-sitter/licenses/).
The vendored Lexbor 3.0.0 HTML parser keeps its Apache-2.0 license and notice
in [`vendor/lexbor/`](vendor/lexbor/). See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Links

- [CHANGELOG.md](CHANGELOG.md)
- Issues: <https://github.com/bissakov/arqan/issues>
