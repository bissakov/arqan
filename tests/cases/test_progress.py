"""The run ledger: facts about the work since the user's last message.

The model never sees a fact it did not write, so a long run drifts without it
noticing: how many calls it has made, how often the context was compacted,
or that the same command has failed again and again. The harness counts
these and appends a short note to a tool result when one is due. The note is
for the model, so it is on the wire and stripped from the screen.
"""

import json
import signal

from .test_compact_auto import FULL, configure, spawn

TRUE = json.dumps({"command": "true"})
FALSE = json.dumps({"command": "false"})


def run_results(ctx):
    """Tool results in the last request since its last user message."""
    messages = ctx.mock.requests[-1]["messages"]
    start = max(i for i, m in enumerate(messages) if m.get("role") == "user")
    return [m["content"] for m in messages[start:] if m.get("role") == "tool"]


def notes(results):
    return [r for r in results if "[progress:" in r]


def test_no_note_before_the_call_count_then_one_at_it(ctx):
    """Forty calls in, the run is long enough to say so once."""
    ctx.scenario(f"tool=bash:{TRUE},tool_rounds=40,final_text=done")
    s = ctx.spawn()
    s.submit("do the long thing")
    s.wait_text("done", timeout=60)
    s.wait_turn_done(timeout=60)

    results = run_results(ctx)
    assert len(results) == 40, results
    noted = notes(results)
    assert len(noted) == 1, noted
    assert noted[0] is results[-1], results
    assert "40 tool calls since the user's last message" in noted[0], noted[0]
    assert "failed" not in noted[0], noted[0]
    assert "stop and report" in noted[0], noted[0]


def test_a_command_that_keeps_failing_is_named(ctx):
    """The third failure of the same command earns a note that names it."""
    ctx.scenario(f"tool=bash:{FALSE},tool_rounds=3,final_text=done")
    s = ctx.spawn()
    s.submit("make it pass")
    s.wait_text("done")
    s.wait_turn_done()

    results = run_results(ctx)
    assert len(results) == 3, results
    noted = notes(results)
    assert len(noted) == 1 and noted[0] is results[-1], results
    assert "3 tool calls since the user's last message, 3 failed" in noted[0], \
        noted[0]
    assert '"false" failed 3 times' in noted[0], noted[0]


def test_a_command_that_passes_does_not_count(ctx):
    """Success is not a failure to report, however often it repeats."""
    ctx.scenario(f"tool=bash:{TRUE},tool_rounds=5,final_text=done")
    s = ctx.spawn()
    s.submit("check it")
    s.wait_text("done")
    s.wait_turn_done()

    assert not notes(run_results(ctx)), run_results(ctx)


def test_the_note_stays_off_the_screen(ctx):
    """The note is addressed to the model; the result keeps its own status."""
    ctx.scenario(f"tool=bash:{FALSE},tool_rounds=3,final_text=done")
    s = ctx.spawn()
    s.submit("make it pass")
    s.wait_text("done")
    s.wait_turn_done()

    assert notes(run_results(ctx)), run_results(ctx)
    text = s.text()
    assert "progress" not in text, text
    heads = [l for l in text.splitlines() if "\u2514\u2500" in l]
    assert heads and "exit 1" in heads[-1], text


def test_the_note_stays_off_the_screen_in_a_batch(ctx):
    """A batch child is a tool call too, and its result renders the same way."""
    batch = "batch:" + json.dumps(
        {"steps": [{"tool": "bash", "args": {"command": "false"}}]})
    ctx.scenario(f"tool={batch},tool_rounds=3,final_text=done")
    s = ctx.spawn()
    s.submit("make it pass")
    s.wait_text("done")
    s.wait_turn_done()

    results = run_results(ctx)
    assert len(results) == 3, results
    child = json.loads(results[-1])["steps"][0]["result"]
    assert '"false" failed 3 times' in child, child
    text = s.text()
    assert "progress" not in text, text
    assert "exit 1" in text, text


def test_a_compaction_is_reported_on_the_next_result(ctx):
    """The summary replaced what the model remembers, so the next result
    says that it did."""
    configure(ctx, compact_model="small", small_model="mock:text=summary")
    ctx.scenario('tool=bash:{"command":"seq 1 300"},tool_rounds=9,' + FULL)
    s = spawn(ctx)

    def reported(request):
        return any("[progress:" in m.get("content", "")
                   and "context compacted 1 time." in m["content"]
                   for m in request["messages"] if m.get("role") == "tool")

    s.submit("go")
    s.wait_for(lambda _: any(reported(r) for r in ctx.mock.requests),
               "a result that reports the compaction")
    s.signal(signal.SIGINT)
    s.wait_turn_done()


def test_a_new_user_message_starts_a_new_run(ctx):
    """The counts belong to the run, and the user's next message ends it."""
    ctx.scenario(f"tool=bash:{FALSE},tool_rounds=3,final_text=done")
    s = ctx.spawn()
    s.submit("make it pass")
    s.wait_text("done")
    s.wait_turn_done()
    assert notes(run_results(ctx)), run_results(ctx)

    ctx.scenario(f"tool=bash:{FALSE},tool_rounds=3,final_text=again")
    s.submit("try once more")
    s.wait_text("again")
    s.wait_turn_done()

    noted = notes(run_results(ctx))
    assert len(noted) == 1, run_results(ctx)
    assert "3 tool calls since the user's last message, 3 failed" in noted[0], \
        noted[0]
    assert '"false" failed 3 times' in noted[0], noted[0]
