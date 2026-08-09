# Tests

End-to-end tests for the terminal UI. `bin/yoke` runs unmodified inside a
pseudo-terminal, talking to a dummy provider that speaks either API, and every
byte it writes is replayed into a small terminal emulator. Assertions are
therefore made against *what a user would see*, never against escape
sequences.

Python 3 only, no third-party packages.

```
make test                       # run everything
make test T="-k composer"       # run matching cases
make test T=--list              # list cases with their one-line summaries
make test-update                # rewrite golden screens after an intended change
python3 tests/run.py -v -x      # verbose, stop at the first failure
python3 tests/run.py --repeat 5 # flush out flakiness
python3 tests/run.py -j 1       # one case at a time, for debugging
```

Cases run in parallel (`-j`, default three times the CPU count, capped at 48): each
owns its temp `HOME`, its provider port and its pty, so the only thing they
share is the read-only golden directory. A case spends ~99% of its time
waiting, so the suite is bounded by its slowest case, not by their sum.

## Layout

| path | what it is |
| --- | --- |
| `run.py` | discovery, isolation, reporting; the whole test framework |
| `context.py` | per-test fixture: temp `HOME`/cwd, mock provider, pty sessions, golden files |
| `harness/vt.py` | terminal emulator: CUP/ED/EL/SGR, alt screen, DEC modes, OSC 52 |
| `harness/session.py` | spawns `yoke` on a pty, sends keys, waits on screen states |
| `harness/keys.py` | symbolic key names and SGR mouse reports to bytes |
| `mockprovider/server.py` | the dummy provider |
| `mockprovider/lorem.py` | deterministic lorem ipsum (own LCG, so seeds are stable) |
| `cases/test_*.py` | the tests; every `test_*` function is one case |
| `golden/` | expected screen dumps |

## Writing a case

A case is a function taking the fixture. It gets a fresh temp directory, a
fresh mock provider on an ephemeral port and a scrubbed environment.

```python
def test_something(ctx):
    """One line describing the behaviour, shown by the runner."""
    ctx.scenario("text=hello+there")       # what the provider will stream
    s = ctx.spawn()                        # yoke on an 80x24 pty
    s.submit("say hi")                     # type + Enter, returns once accepted
    s.wait_text("hello there")
    s.wait_turn_done()
    assert "●  Assistant" in s.text()
    ctx.check_screen(s)                    # compare against golden/<case>.txt
```

### Determinism

Nothing sleeps for a fixed time; tests wait for states.

* `s.sync()`: wait for the repaint caused by the input just sent, then for
  quiet. This is what makes "send a key, then assert" safe.
* `s.wait_for(pred)` / `s.wait_text()` / `s.wait_status()`: poll the emulated
  screen until it holds.
* `s.submit(text)`: returns once the composer clears, which is the signal a
  turn actually started.
* `s.wait_turn_done()`: returns when the agent loop is idle again.
* `s.settings_toggle(label)`: opens `/settings`, flips that checkbox, waits
  for the box to read back, and closes. `open_settings`, `settings_select` and
  `settings_act` are the same gesture in pieces, for a value row that opens a
  picker or a question.

"Quiet" means no output for a short window. The window has to outlast the
largest gap the frames being waited on can contain, so it follows the
scenario: a bare one settles in 60 ms, and `delay=` widens it to 2.5x the
pacing. Set `YOKE_TEST_QUIET=0.2` to raise the floor on a machine too slow or
too loaded for the default.

Retries are off in the fixture (`YOKE_RETRIES=0`), so a scenario that fails is
read as an answer; a case about the retry loop turns it on and pins
`YOKE_RETRY_DELAY_MS` so the backoff costs nothing.

The environment is pinned so the rendered frame is reproducible: fixed
`TERM`, `LC_ALL=C.UTF-8`, an isolated `HOME`/`XDG_CONFIG_HOME` that the state
and cache dirs default under, a cwd of
`~/work` (the status line shows it) and a fixed model name and system prompt.
The mock listens on an ephemeral port, and the status line renders loopback as
`local`, so the port never leaks into a golden file.

Colour stays enabled: golden snapshots record glyphs only, and the attribute
grid is what lets tests assert the transcript's role colours.

### Golden screens

`ctx.check_screen(s)` writes `golden/<case>.txt` the first time and compares
afterwards, printing a unified diff on mismatch. Use `make test-update` to
accept an intended change, and read the diff before you do. The one thing a
rerun cannot reproduce is a clock, so an elapsed time (`· 12ms`, `· 1.4s`) is
normalised to `· <t>` before the comparison; assert on its shape in the case
instead, with `s.activity()` for the spinner row.

## The dummy provider

`mockprovider/server.py` serves `POST /v1/chat/completions` with SSE deltas,
tool calls, `stream_options.include_usage` and `[DONE]`, or, when the request
asks for `stream: false`, the same reply as one `chat.completion` document. It
also serves `POST /v1/messages` with the content-block events the Anthropic
API streams, from the same scenario, so a case picks the wire format with
`YOKE_API=anthropic` and says nothing else about it. What it streams comes
from a scenario string:

```python
ctx.scenario("words=40,chunk=5,delay=0.02")
ctx.scenario("text=exactly+this+text,usage=1200/40")
ctx.scenario('tool=read:{"path":"notes.txt"},final_text=I+read+it')
ctx.scenario("status=500")
```

| key | meaning |
| --- | --- |
| `text=` | literal reply; `+` is a space, `\n` a line break |
| `words=`, `paragraphs=`, `seed=` | size and shape of generated lorem ipsum |
| `chunk=` | words per SSE delta (`chunk=1` streams word by word) |
| `delay=`, `first_delay=` | seconds between deltas / before the first one |
| `reasoning=` | thinking trace streamed before the reply |
| `reasoning_field=` | the delta key it arrives in (`reasoning_content` by default, `reasoning` for OpenRouter-style providers) |
| `tool=NAME:JSON` | emit a tool call; repeatable for parallel calls |
| `tool_rounds=` | how many rounds of tool calls before answering |
| `final_text=` | reply sent after the tool results come back |
| `usage=P/C` | pin prompt/completion tokens (otherwise estimated) |
| `status=` | fail with this HTTP status instead of streaming |
| `fail_times=` | fail the first N completion requests, then behave normally |
| `fail_status=`, `fail_mode=` | how they fail: that status, or `close` to answer nothing |
| `abort_after=` | reset the connection after that many content deltas |
| `models=` | ids `GET /v1/models` serves, separated by `\|` |
| `model_count=` | serve that many generated ids (`model-000`, ...) |
| `models_status=` | fail `GET /v1/models` with this HTTP status |

Requests are recorded: `ctx.mock.requests` is the parsed request bodies,
`ctx.mock.auth` the `Authorization` header each of them carried (`None` when
there was none), `ctx.mock.keys` and `ctx.mock.versions` the `x-api-key` and
`anthropic-version` headers the other API sends instead, and
`ctx.mock.tool_results()` the tool outputs that were fed back, in either
shape.

### Driving the UI by hand

The same server runs standalone, which is the quickest way to poke at the UI
without an API key:

```
make mock MOCK_ARGS="--port 8080 --scenario words=80,chunk=2,delay=0.05"

YOKE_BASE_URL=http://127.0.0.1:8080/v1 YOKE_API_KEY=x YOKE_MODEL=mock ./bin/yoke
```

The model name doubles as a scenario, so you can change the reply without
restarting the server:

```
YOKE_MODEL='lorem:words=200,chunk=1,delay=0.02' ./bin/yoke
YOKE_MODEL='lorem:tool=bash:{"command":"ls"},final_text=there+you+go' ./bin/yoke
```
