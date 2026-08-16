# Tests

End-to-end tests run `bin/arqan-test` in a pseudo-terminal against a local mock
provider. They assert on an emulated terminal screen, not escape sequences.
Python 3 is the only test dependency.

`bin/arqan-test` is the `-DAGENT_TESTING` build. The trust store and the web
endpoints reach their fixtures through hooks compiled in only there, so the
suite needs that binary and refuses any other. `make test` builds it and sets
`ARQAN_TEST_BIN`; `tests/run.py` reads the same variable.

```sh
make test                       # all cases
make test T="-k composer"       # matching cases
make test T=--list              # list cases
make test-update                # update intended golden screens
make test-asan                  # ASan + UBSan suite
make test-fil                   # Fil-C memory-safety suite
make test T="-v -x"             # verbose; stop on failure
make test T="--repeat 5"        # check a suspected flake
make test T="-j 1"              # serial debugging
```

Options `T` cannot express go to the script directly:

```sh
ARQAN_TEST_BIN=bin/arqan-test python3 tests/run.py --repeat 5 -k composer
```

The sanitizer build is a separate tree: `make test-asan` compiles into
`build/asan/` and `bin/asan/` and never touches `bin/arqan`, so a plain `make`
stays valid afterwards and `make test` keeps driving the uninstrumented build.
To run part of the suite against the instrumented build, build it once with
`make asan` and point the runner at it:

```sh
make asan
ASAN_OPTIONS=detect_leaks=0 ARQAN_TEST_BIN=bin/asan/arqan-test \
    python3 tests/run.py -k composer
make clean-asan                 # drop the instrumented tree only
```

`make test-fil` is the same arrangement under Fil-C, in `build/fil/` and
`bin/fil/`. It needs the Fil-C `/opt/fil` distribution, the only one shipping a
Fil-C libcurl; set `FILCC` if the compiler lives elsewhere. Fil-C carries
bounds on the pointer, so it catches an out-of-bounds access that lands inside
another valid object, which ASan cannot.

Each case receives an isolated home directory, working directory, mock-server
port, and pty. Cases normally run in parallel.

## Write a case

Add a `test_*` function under `cases/`:

```python
def test_something(ctx):
    """One-line behaviour summary."""
    ctx.scenario("text=hello+there")
    s = ctx.spawn()
    s.submit("say hi")
    s.wait_text("hello there")
    s.wait_turn_done()
    ctx.check_screen(s)
```

Wait for observable state; never use fixed sleeps. `s.sync()` waits for a
repaint and quiet, `s.wait_text()` and `s.wait_status()` wait for screen
content, and `s.wait_turn_done()` waits for the agent loop to become idle.
`ctx.check_screen(s)` creates or compares `golden/<case>.txt`; inspect its
diff before accepting an intentional update.

A wait ends as soon as the child says it is settled: the test build announces
every park on its input with an APC beacon carrying the input bytes it has
consumed, so a wait after `send` ends when the child has read everything the
case wrote and painted the frame behind it. A build without the beacon - an
older binary, another toolchain - falls back to waiting a quiet window out,
which is the same answer more slowly.

A read of the pty lands wherever the child happens to be writing, so the
harness feeds the emulator whole frames only: `arqan` wraps each repaint in
synchronized output and the bytes of an unfinished frame are held back until
its closing mark arrives. The screen a case inspects is therefore always one
the terminal finished painting, and a bare assertion after a wait reads a
settled row rather than a half-written one.

## Mock scenarios

`ctx.scenario()` controls the next completion. Common options:

| option | purpose |
| --- | --- |
| `text=` | literal response; `+` means a space and `\n` a newline |
| `words=`, `paragraphs=`, `seed=` | generated lorem shape |
| `chunk=`, `delay=`, `first_delay=` | streaming cadence |
| `tool=NAME:JSON`, `tool_rounds=`, `tool_ids=`, `final_text=` | tool-call rounds |
| `reasoning=`, `reasoning_field=` | streamed reasoning trace |
| `usage=P/C` | token counts |
| `status=`, `fail_times=`, `fail_status=`, `fail_mode=` | failures and retries |
| `abort_after=` | cut a streaming response short |
| `models=`, `model_count=`, `models_status=` | model-list response |
| `hold`, `hold_final`, `hold_round=`, `hold_after=` | pause the response at a gate |

Recorded requests are available through `ctx.mock.requests`; auth headers and
tool results have matching helpers on `ctx.mock`.

A case that needs a turn to still be running holds the response at the gate
rather than guessing a delay: `hold` stops the first response before its
first byte, `hold_final` stops the round that carries the text, `hold_round=N`
stops round N, and `hold_after=N` stops after N words have streamed. The case
asserts whatever the running turn should show, then `ctx.mock.release()` lets
the response finish. A held response is released when the mock stops, and a
gate nothing releases times out rather than hanging the suite.

Run the mock provider manually without an API key:

```sh
make mock MOCK_ARGS="--port 8080 --scenario words=80,chunk=2,delay=0.05"
ARQAN_BASE_URL=http://127.0.0.1:8080/v1 ARQAN_API_KEY=x ARQAN_MODEL=mock ./bin/arqan
```
