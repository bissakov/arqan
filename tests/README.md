# Tests

End-to-end tests run `bin/yoke` in a pseudo-terminal against a local mock
provider. They assert on an emulated terminal screen, not escape sequences.
Python 3 is the only test dependency.

```sh
make test                       # all cases
make test T="-k composer"       # matching cases
make test T=--list              # list cases
make test-update                # update intended golden screens
make test-asan                  # ASan + UBSan suite
python3 tests/run.py -v -x      # verbose; stop on failure
python3 tests/run.py --repeat 5 # check a suspected flake
python3 tests/run.py -j 1       # serial debugging
```

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
| `tool=NAME:JSON`, `tool_rounds=`, `final_text=` | tool-call rounds |
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
YOKE_BASE_URL=http://127.0.0.1:8080/v1 YOKE_API_KEY=x YOKE_MODEL=mock ./bin/yoke
```
