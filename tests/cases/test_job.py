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
