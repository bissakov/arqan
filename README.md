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

## Linux installation

The project release is for Linux x86_64 systems. The `.deb` and `.rpm` link
the distribution's libraries and want glibc 2.31 or newer and `libcurl.so.4`
from libcurl 7.66 or newer; the portable archive borrows nothing and wants
only the kernel. All three read a CA certificate bundle at run time. macOS,
Windows, aarch64, hosted apt/dnf repositories, and package signing are not
part of this release milestone. Release packages are downloaded directly from
the GitHub release rather than from a configured package repository.

Download `SHA256SUMS` and the package for your system, then verify and install
it. Debian and Ubuntu users can install the native package with:

```sh
sha256sum --ignore-missing -c SHA256SUMS
sudo apt install ./arqan_X.Y.Z-1_amd64.deb
```

RPM-based systems can install with:

```sh
sha256sum --ignore-missing -c SHA256SUMS
sudo dnf install ./arqan-X.Y.Z-1.x86_64.rpm
```

Upgrade by passing the newer local package to the same `apt install` or
`dnf install` command. Remove a package with:

```sh
sudo apt remove arqan       # Debian or Ubuntu
sudo dnf remove arqan       # RPM-based systems
```

Package removal deletes only package-owned programs and documentation. It
preserves configuration under `${XDG_CONFIG_HOME:-$HOME/.config}/arqan`, state
and credentials under `${XDG_STATE_HOME:-$HOME/.local/state}/arqan`, sessions
under `${XDG_DATA_HOME:-$HOME/.local/share}/arqan`, and project `.arqan`
directories.

The portable archive is an installer-free fallback and the choice where the
native packages do not install. Its two programs are static-pie musl builds:
they name no interpreter, need no shared library, and so run on a musl
distribution, on a glibc older than the packages ask for, and in a container
with no libcurl. They still find the system CA store at run time, and
`SSL_CERT_FILE` or `SSL_CERT_DIR` names one elsewhere. Verify the archive
with the same manifest, extract it, and run it in place:

```sh
sha256sum --ignore-missing -c SHA256SUMS
tar -xzf arqan-X.Y.Z-linux-x86_64.tar.gz
cd arqan-X.Y.Z-linux-x86_64
./bin/arqan
```

On first launch, use `/provider` to add a connection and `/model` to pick a
model. A source checkout can instead be built and run with `make` and
`./bin/arqan`.

Maintainers can run `scripts/build-musl.sh`, which builds and tests the static
pair in the pinned Alpine container and leaves it in `bin/musl`, then
`make package-linux` to produce the tarball, `.deb`, `.rpm`, and checksum
manifest. The native packages take the host binaries from `bin`, the archive
takes the static ones, and packaging fails rather than shipping an archive
that borrows a library. `make test-package-linux` checks all three formats and
reproducibility. `make release-linux` (or `scripts/release-linux.sh`) performs
a clean build, all tests, both builds, and packaging in the pinned Debian 11
and Alpine containers, then tests native install, reinstall, and removal in
disposable Debian, Ubuntu, and Fedora-family containers and runs the archive
in Alpine and Debian 11.

## Configure

A provider is a connection: a base URL, an API flavour, and a key. A model is
a `(provider, model)` pair. `/provider` adds, edits, and removes connections;
`/model` lists every configured provider and picks a pair from any of them, so
there is no provider to switch. Alternatively set:

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
api = "openai"
```

A project overrides that in `.arqan/config.toml`, which is looked for in the
working directory and every directory above it, nearest last. It arrives with
a `git clone`, so it may not set `api_key` or anything naming a key store;
such a line is reported and ignored.

Precedence, lowest first: defaults, `$XDG_CONFIG_DIRS/arqan/config.toml`,
`$XDG_CONFIG_HOME/arqan/config.toml`, `.arqan/config.toml`, remembered UI
choices in `$XDG_STATE_HOME/arqan/state.toml`, the chosen provider's section,
`ARQAN_*`, then command-line options. `/provider` writes provider definitions
to the user's config and keys to `$XDG_STATE_HOME/arqan/credentials.toml`
(mode 0600); `/model` remembers the chosen pair in the state file. Run
`arqan --help` for all options.

## Performance

Measured at `1566515` on Linux 7.1.8 with GCC 16.2.1 and a Ryzen 7 7800X3D.
Build times are medians of three clean 16-job builds. Runtime measurements use
the real TUI and agent loop against the local mock provider, so they exclude
network and model latency. The harness measures the first visible frame before
its 60 ms quiet window and reports agent CPU time separately from wall time.

- Clean build: 4.45 s for `make -j16 minimal`; 4.69 s for `make -j16`.
- Executables: 1.56 MiB for `bin/arqan`; 12.64 MiB for
  `bin/arqan-highlight`.
- Startup: 5.0 ms median and 6.0 ms p95 to the first TUI frame over 80 runs;
  agent CPU was about 3.0 ms, and idle memory was 2.5 MiB private dirty with
  12 MiB peak RSS.
- Streaming: a 2,000-word reply in 500 deltas took 210 ms wall and 11.3 ms
  CPU, or 0.02 ms CPU per delta.
- Tool loop: six `read` rounds in one turn took 204 ms wall and 4.5 ms CPU,
  or 0.75 ms CPU per round.
- Large context: replaying a 400k-token, 1.7 MB session took 198 ms wall and
  13.4 ms CPU at 7.3 MiB private dirty with 17 MiB peak RSS.
- Benchmarks: all 58 default cases completed within budget in 107.3 s.
- Test suite: 754/754 end-to-end cases passed in 30.9 s.

Private dirty is memory written for this process alone; RSS also counts
resident shared libraries and file-backed pages. Run `make bench` to reproduce
the runtime measurements; see [bench/README.md](bench/README.md) for workloads,
metrics and slow stress cases.

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
