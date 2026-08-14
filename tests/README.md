# Tests

End-to-end tests run `bin/arqan` in a pseudo-terminal against a local mock
provider. They assert on an emulated terminal screen, not escape sequences.
Python 3 is the only test dependency.

```sh
make test                       # all cases
make test T="-k composer"       # matching cases
make test T=--list              # list cases
make test-update                # update intended golden screens
make test-asan                  # ASan + UBSan suite
make test-fil                   # Fil-C memory-safety suite
python3 tests/run.py -v -x      # verbose; stop on failure
python3 tests/run.py --repeat 5 # check a suspected flake
python3 tests/run.py -j 1       # serial debugging
```

The sanitizer build is a separate tree: `make test-asan` compiles into
`build/asan/` and `bin/asan/` and never touches `bin/arqan`, so a plain `make`
stays valid afterwards and a bare `python3 tests/run.py` keeps driving the
shipped binary. To run part of the suite against the instrumented build, build
it once with `make asan` and point the runner at it:

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

Recorded requests are available through `ctx.mock.requests`; auth headers and
tool results have matching helpers on `ctx.mock`.

Run the mock provider manually without an API key:

```sh
make mock MOCK_ARGS="--port 8080 --scenario words=80,chunk=2,delay=0.05"
ARQAN_BASE_URL=http://127.0.0.1:8080/v1 ARQAN_API_KEY=x ARQAN_MODEL=mock ./bin/arqan
```
