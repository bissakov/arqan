"""Detached commands: the deadline, the job tool and what outlives a session.

A command that outruns the shell deadline is handed to the job table instead
of holding the turn open, because an idle model costs a provider's prompt
cache. These cases pin that it is detached and not killed, that its output
continues to arrive, and that nothing it started survives the session.
"""

import json
import time


def bash(command: str) -> str:
    return "bash:" + json.dumps({"command": command})


def bash_for(command: str, timeout_ms: int) -> str:
    return "bash:" + json.dumps({"command": command, "timeout_ms": timeout_ms})


def job(**args) -> str:
    return "job:" + json.dumps(args)


def results(ctx) -> list[str]:
    return ctx.mock.tool_results()


def test_a_slow_command_is_detached_into_a_job(ctx):
    """Past the deadline, bash answers with a job id rather than waiting."""
    ctx.scenario(
        "tool=" + bash("sleep 1; echo done-late") + ",tool=" + job(id=1)
        + ",final_text=followed"
    )
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="300")
    s.submit("run something slow")
    s.wait_text("followed")
    s.wait_turn_done()

    detached, followed = results(ctx)[0], results(ctx)[1]
    assert "still running as job 1" in detached, detached
    assert "[exit" not in detached, detached
    # the transcript summarises it where a finished command shows its exit
    assert "\u2514\u2500 still running as job 1" in s.text(), s.text()
    # and the follow-up call is shown by the job it acts on
    assert "\u25c6  job 1" in s.text(), s.text()
    # the command ran on to completion; its later output reaches the follow-up
    assert "done-late" in followed, followed
    assert "[job 1 exit 0" in followed, followed


def test_a_detached_command_keeps_running(ctx):
    """Detaching is not killing: the work finishes after bash has answered."""
    ctx.scenario(
        "tool=" + bash("sleep 0.6; touch finished.txt") + ",tool=" + job(id=1)
        + ",final_text=followed"
    )
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="200")
    s.submit("run something slow")
    s.wait_text("followed")
    s.wait_turn_done()

    assert "still running as job 1" in results(ctx)[0], results(ctx)[0]
    assert (ctx.work / "finished.txt").exists(), "the detached command was killed"


def test_a_wait_answers_with_the_tail_the_command_wrote(ctx):
    """What a command writes just before exiting is still in the pipe when it
    is reaped, so a wait covers the drain too. It is not held by what the
    command left behind: a child holding the pipe open must not hold the
    answer past the command it came from."""
    ctx.scenario(
        "tool=" + bash("sleep 3 & sleep 0.4; echo tail-line")
        + ",tool=" + job(id=1) + ",final_text=followed"
    )
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="200")
    s.submit("run something slow")
    started = time.monotonic()
    s.wait_text("followed")
    s.wait_turn_done()
    elapsed = time.monotonic() - started

    followed = results(ctx)[1]
    assert "tail-line" in followed, followed
    assert "[job 1 exit 0" in followed, followed
    assert elapsed < 3, elapsed


def test_a_quick_command_is_unchanged(ctx):
    """Inside the deadline nothing detaches: one result, with its exit line."""
    ctx.scenario("tool=" + bash("echo quick") + ",final_text=ran")
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="5000")
    s.submit("run something quick")
    s.wait_text("ran")
    s.wait_turn_done()

    out = results(ctx)[0]
    assert out.startswith("quick\n"), out
    assert out.endswith("[exit 0]"), out
    assert "job" not in out, out


def test_a_zero_deadline_waits_for_the_command(ctx):
    """0 turns the deadline off, which is how it behaved before jobs."""
    ctx.scenario("tool=" + bash("sleep 0.4; echo waited") + ",final_text=ran")
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="0")
    s.submit("run something slow")
    s.wait_text("ran")
    s.wait_turn_done()

    out = results(ctx)[0]
    assert "waited" in out, out
    assert out.endswith("[exit 0]"), out


def test_killing_a_job_stops_its_work(ctx):
    """kill ends the whole session the command leads, not just the shell."""
    ctx.scenario(
        "tool=" + bash("sleep 5; touch never.txt") + ",tool="
        + job(id=1, action="kill") + ",final_text=stopped"
    )
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="200")
    s.submit("run something slow")
    s.wait_text("stopped")
    s.wait_turn_done()

    killed = results(ctx)[1]
    assert "[job 1 killed by signal" in killed, killed
    time.sleep(0.3)
    assert not (ctx.work / "never.txt").exists(), "the killed job ran on"


def test_listing_and_unknown_ids(ctx):
    """A session with no jobs says so; an id it never had is an error."""
    ctx.scenario("tool=" + job() + ",tool=" + job(id=9) + ",final_text=looked")
    s = ctx.spawn(TMPDIR=str(ctx.work))
    s.submit("what is running?")
    s.wait_text("looked")
    s.wait_turn_done()

    listing, missing = results(ctx)[0], results(ctx)[1]
    assert "[no jobs in this session]" in listing, listing
    assert missing.startswith("ERROR:"), missing
    assert "no job 9 in this session" in missing, missing


def test_a_full_table_waits_instead_of_orphaning(ctx):
    """With every slot taken the deadline lapses: the command is waited out,
    never detached with nowhere to record it."""
    slow = ",".join("tool=" + bash("sleep 0.4; echo run-%d" % i)
                    for i in range(9))
    ctx.scenario(slow + ",final_text=all+nine")
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="100")
    s.submit("run nine slow things")
    s.wait_text("all nine")
    s.wait_turn_done()

    out = results(ctx)
    assert len(out) == 9, out
    detached = [r for r in out if "still running as job" in r]
    assert len(detached) == 8, detached
    assert out[8].startswith("run-8\n"), out[8]
    assert out[8].endswith("[exit 0]"), out[8]


def test_a_job_does_not_outlive_the_session(ctx):
    """Exiting kills what was detached, so no orphan writes into the tree."""
    ctx.scenario(
        "tool=" + bash("sleep 2; touch orphan.txt") + ",final_text=detached"
    )
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="200")
    s.submit("run something slow")
    s.wait_text("detached")
    s.wait_turn_done()
    assert "still running as job 1" in results(ctx)[0], results(ctx)[0]

    s.submit("/exit")
    s.wait_exit()
    time.sleep(2.2)
    assert not (ctx.work / "orphan.txt").exists(), "a job outlived the session"
    logs = list(ctx.work.glob("arqan-bash-*.log"))
    assert not logs, f"job logs left behind: {logs}"


def test_a_call_can_hand_the_turn_back_sooner(ctx):
    """A caller that knows the command is slow detaches it on its own terms,
    well inside the configured deadline."""
    ctx.scenario(
        "tool=" + bash_for("sleep 1; echo late", 200) + ",tool=" + job(id=1)
        + ",final_text=followed"
    )
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="60000")
    s.submit("start the slow one")
    s.wait_text("followed")
    s.wait_turn_done()

    detached, followed = results(ctx)[0], results(ctx)[1]
    assert "still running as job 1" in detached, detached
    assert "late" in followed, followed
    assert "[job 1 exit 0" in followed, followed


def test_a_call_cannot_hold_the_turn_longer(ctx):
    """The deadline is a ceiling: a longer wait is granted as the ceiling.
    Refusing it would cost a round and the command runs either way."""
    ctx.scenario(
        "tool=" + bash_for("sleep 1; echo late", 60000)
        + ",tool=" + job(id=1) + ",final_text=followed"
    )
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="300")
    s.submit("wait as long as you like")
    s.wait_text("followed")
    s.wait_turn_done()

    detached, followed = results(ctx)[0], results(ctx)[1]
    assert not detached.startswith("ERROR:"), detached
    assert "still running as job 1" in detached, detached
    assert "late" in followed, followed


def test_a_job_wait_past_the_maximum_is_granted_as_the_maximum(ctx):
    """The job tool clamps the same way, so a wildly long wait still answers
    the moment the job ends."""
    ctx.scenario(
        "tool=" + bash("sleep 0.5; echo late") + ",tool="
        + job(id=1, timeout_ms=999999999) + ",final_text=followed"
    )
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="200")
    s.submit("run something slow")
    s.wait_text("followed")
    s.wait_turn_done()

    followed = results(ctx)[1]
    assert not followed.startswith("ERROR:"), followed
    assert "[job 1 exit 0" in followed, followed


def test_no_deadline_means_no_per_call_deadline(ctx):
    """With detaching turned off, a call asking for it is told so."""
    ctx.scenario("tool=" + bash_for("echo hi", 100) + ",final_text=refused")
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="0")
    s.submit("detach this")
    s.wait_text("refused")
    s.wait_turn_done()

    out = results(ctx)[0]
    assert out.startswith("ERROR:"), out
    assert "shell_timeout_ms is 0" in out, out


def test_a_deadline_past_the_cache_window_is_refused(ctx):
    """The deadline exists to keep the model inside a prompt cache, so it
    cannot be set beyond one. Waiting longer than that is what 0 is for."""
    ctx.write_config("shell_timeout_ms = 900000\n")
    ctx.scenario("text=ok")
    out = ctx.run_cli("-p", "hello")
    assert "shell_timeout_ms" in out.stderr, out.stderr
