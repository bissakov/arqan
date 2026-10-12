"""Detached commands: the deadline, the job tool and what outlives a session.

A command that outruns the shell deadline is handed to the job table instead
of holding the turn open, because an idle model costs a provider's prompt
cache. These cases pin that it is detached and not killed, that its output
continues to arrive, and that nothing it started survives the session.
"""

import json
import os
import re
import time
from pathlib import Path


# TODO: placeholder description in both helpers; write real ones per case
def bash(command: str) -> str:
    return "bash:" + json.dumps({"command": command, "description": "qzx"})


def bash_for(command: str, timeout_ms: int) -> str:
    return "bash:" + json.dumps({"command": command, "description": "qzx",
                                 "timeout_ms": timeout_ms})


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
    # the header says everything the arguments do, so they are not repeated
    assert '"id"' not in s.text(), s.text()
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
    # jobs that ended meanwhile are named after the exit line, not in place of it
    waited = out[8].split("\n\n[jobs ended: ")
    assert len(waited) <= 2, out[8]
    assert waited[0].endswith("[exit 0]"), out[8]


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


def wait_for_file(path: Path, timeout: float = 10.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return True
        time.sleep(0.05)
    return path.exists()


def test_a_queued_message_ends_a_job_wait_and_not_the_job(ctx):
    """A message the user queued is not held back by a long wait: the wait
    ends, the turn hands over to the message, and the job runs on."""
    ctx.scenario(
        "tool=" + bash("sleep 3; touch finished.txt") + ",tool="
        + job(id=1, timeout_ms=60000) + ",final_text=never"
    )
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="200")
    s.submit("run something slow and wait for it")
    s.wait_activity("running job")
    ctx.scenario("text=heard+you,final_text=heard+you")
    s.submit("something else")
    s.wait_text("heard you")
    s.wait_turn_done()

    waited = results(ctx)[1]
    assert "[job 1 still running after" in waited, waited
    assert "stopped waiting for a user message" in waited, waited
    assert "never" not in s.text(), s.text()
    messages = ctx.mock.requests[-1]["messages"]
    assert messages[-1]["role"] == "user", messages
    assert messages[-1]["content"] == "something else", messages
    assert wait_for_file(ctx.work / "finished.txt"), "the job was killed"


def test_a_queued_message_detaches_a_running_command(ctx):
    """A command inside its deadline becomes a job when the user queues a
    message, rather than holding the message until the deadline."""
    ctx.scenario(
        "tool=" + bash("sleep 3; touch finished.txt") + ",final_text=never"
    )
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="60000")
    s.submit("run something slow")
    s.wait_activity("running bash")
    ctx.scenario("text=heard+you,final_text=heard+you")
    s.submit("something else")
    s.wait_text("heard you")
    s.wait_turn_done()

    detached = results(ctx)[0]
    assert "still running as job 1" in detached, detached
    assert "detached for a user message" in detached, detached
    assert wait_for_file(ctx.work / "finished.txt"), "the command was killed"


def test_a_queued_message_leaves_a_quick_command_whole(ctx):
    """A command that finishes within a second is not turned into a job just
    because a message is waiting."""
    ctx.scenario("hold,tool=" + bash("echo quick") + ",final_text=never")
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="60000")
    s.submit("run something quick")
    s.wait_for(lambda _: len(ctx.mock.requests) >= 1, "the first request")
    s.submit("something else")
    s.wait_text("message queued")
    ctx.scenario("text=heard+you,final_text=heard+you")
    ctx.mock.release()
    s.wait_text("heard you")
    s.wait_turn_done()

    out = results(ctx)[0]
    assert out.startswith("quick\n"), out
    assert out.endswith("[exit 0]"), out
    assert "job" not in out, out


def test_a_job_that_ends_is_named_in_the_next_result(ctx):
    """The model learns a job ended without polling for it: the next tool
    result says so once, and the transcript does not show the note."""
    ctx.scenario(
        "tool=" + bash_for("sleep 0.3; echo one", 100)
        + ",tool=" + bash("sleep 0.8; echo two")
        + ",tool=" + bash("echo three") + ",final_text=noted"
    )
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="5000")
    s.submit("run things")
    s.wait_text("noted")
    s.wait_turn_done()

    detached, second, third = results(ctx)
    assert "still running as job 1" in detached, detached
    assert second.startswith("two\n"), second
    assert "[jobs ended: job 1 exit 0 after" in second, second
    assert "jobs ended" not in third, third
    assert "jobs ended" not in s.text(), s.text()


def test_a_job_the_job_tool_reported_is_not_named_again(ctx):
    """A job whose end the job tool already gave is not repeated as a note."""
    ctx.scenario(
        "tool=" + bash("sleep 0.4; echo late") + ",tool=" + job(id=1)
        + ",tool=" + bash("echo after") + ",final_text=followed"
    )
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="200")
    s.submit("run something slow")
    s.wait_text("followed")
    s.wait_turn_done()

    followed, after = results(ctx)[1], results(ctx)[2]
    assert "[job 1 exit 0" in followed, followed
    assert "jobs ended" not in followed, followed
    assert "jobs ended" not in after, after


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


# The test build shortens the time the waits of one response share, which is
# a little under the provider's five-minute prompt cache in a real session.
ROUND_WAIT_MS = "2000"
SPENT = "this response's 2 seconds of waiting ran out"


def test_the_waits_of_one_response_share_one_time_limit(ctx):
    """Several waits in one response run one after another, and the next
    request waits for all of them. Together they stop at the round limit, so
    the request goes out while the prompt cache is still warm."""
    ctx.scenario(
        "tool=" + bash("sleep 60") + ",tool=" + job(id=1, timeout_ms=300)
        + ",tool=" + job(id=1, timeout_ms=10000) + ",tool="
        + job(id=1, timeout_ms=10000) + ",final_text=followed"
    )
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="200",
                  ARQAN_TEST_ROUND_WAIT_MS=ROUND_WAIT_MS)
    started = time.monotonic()
    s.submit("wait for it three times")
    s.wait_text("followed")
    took = time.monotonic() - started
    s.wait_turn_done()

    first, second, third = results(ctx)[1:4]
    assert took < 8.0, f"the waits held the next request for {took:.1f}s"
    assert "[job 1 still running after" in first, first
    assert "of waiting ran out" not in first, first
    assert re.search(r"\[job 1 still running after \d+s; " + SPENT + r"\]$",
                     second), second
    assert third.endswith("; " + SPENT + "]"), third


def test_a_command_becomes_a_job_at_the_round_limit(ctx):
    """A command that would outlast the round limit is detached at it, even
    with a longer shell deadline."""
    ctx.scenario("tool=" + bash("sleep 60") + ",final_text=followed")
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="60000",
                  ARQAN_TEST_ROUND_WAIT_MS=ROUND_WAIT_MS)
    started = time.monotonic()
    s.submit("run something slow")
    s.wait_text("followed")
    took = time.monotonic() - started
    s.wait_turn_done()

    detached = results(ctx)[0]
    assert took < 8.0, f"the command held the next request for {took:.1f}s"
    assert re.search(r"\[still running as job 1 after \d+s; " + SPENT + r"\]$",
                     detached), detached


def test_the_shared_waiting_time_is_in_the_descriptions(ctx):
    """The model reads the limit where it learns the tools, before a result
    first cuts a wait short."""
    ctx.scenario("final_text=done")
    s = ctx.spawn()
    s.submit("hello")
    s.wait_turn_done()

    tools = {t["function"]["name"]: t["function"]
             for t in ctx.mock.requests[0]["tools"]}
    shared = "The calls in one response share 280 seconds of waiting"
    assert shared in tools["bash"]["description"], tools["bash"]
    assert shared in tools["job"]["description"], tools["job"]
    assert "put one long wait in each response" in \
        tools["job"]["description"], tools["job"]


def test_a_quick_command_after_the_round_limit_still_finishes(ctx):
    """Past the limit, a command still gets a moment to finish, so a quick
    check does not turn into a job."""
    ctx.scenario(
        "tool=" + bash("sleep 60") + ",tool=" + job(id=1, timeout_ms=10000)
        + ",tool=" + bash("echo quick-check") + ",final_text=followed"
    )
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="200",
                  ARQAN_TEST_ROUND_WAIT_MS=ROUND_WAIT_MS)
    s.submit("wait, then check")
    s.wait_text("followed")
    s.wait_turn_done()

    checked = results(ctx)[2]
    assert "quick-check" in checked, checked
    assert "[exit 0]" in checked, checked
    assert "still running as job" not in checked, checked


def test_no_deadline_means_no_per_call_deadline(ctx):
    """With detaching turned off, a call asking for it is told so."""
    ctx.scenario("tool=" + bash_for("echo hi", 100) + ",final_text=refused")
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="0")
    s.submit("detach this")
    s.wait_text("refused")
    s.wait_turn_done()

    out = results(ctx)[0]
    assert out.startswith("ERROR:"), out
    assert "timeout_ms is unavailable in this session: every command is " \
        "waited out" in out, out
    assert "shell_timeout_ms" not in out, out


def test_no_deadline_waits_out_a_command_past_the_round_limit(ctx):
    """Turning detaching off means every command is waited out, so the round
    limit does not detach one either."""
    ctx.scenario("tool=" + bash("sleep 2.5; echo waited-out")
                 + ",final_text=followed")
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="0",
                  ARQAN_TEST_ROUND_WAIT_MS=ROUND_WAIT_MS)
    s.submit("run it to the end")
    s.wait_text("followed")
    s.wait_turn_done()

    out = results(ctx)[0]
    assert "waited-out" in out, out
    assert "still running as job" not in out, out


def test_no_deadline_leaves_jobs_out_of_the_bash_description(ctx):
    """With nothing ever detached, bash offers no timeout_ms and no jobs."""
    ctx.scenario("final_text=done")
    s = ctx.spawn(ARQAN_SHELL_TIMEOUT_MS="0")
    s.submit("hello")
    s.wait_turn_done()

    bash = next(t["function"] for t in ctx.mock.requests[0]["tools"]
                if t["function"]["name"] == "bash")
    assert "timeout_ms" not in bash["parameters"]["properties"], bash
    assert "timeout_ms" not in bash["description"], bash["description"]
    assert " job" not in bash["description"], bash["description"]
    assert "every command is waited out" in bash["description"], \
        bash["description"]


def test_a_deadline_past_the_cache_window_is_refused(ctx):
    """The deadline exists to keep the model inside a prompt cache, so it
    cannot be set beyond one. Waiting longer than that is what 0 is for."""
    ctx.write_config("shell_timeout_ms = 900000\n")
    ctx.scenario("text=ok")
    out = ctx.run_cli("-p", "hello")
    assert "shell_timeout_ms" in out.stderr, out.stderr


def drainers_of(pid: int) -> list[int]:
    """Children of `pid` that run the same program: the forked drainers."""
    exe = os.readlink(f"/proc/{pid}/exe")
    out = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
            ppid = int(stat.rsplit(")", 1)[1].split()[1])
            if ppid == pid and os.readlink(entry / "exe") == exe:
                out.append(int(entry.name))
        except (OSError, ValueError, IndexError):
            continue
    return out


def test_a_drainer_holds_only_its_pipe_and_log(ctx):
    """The drainer never execs, so it would otherwise keep every file the
    agent had open, the terminal and provider sockets included."""
    if not Path("/proc/self/fd").exists():
        return
    ctx.scenario("tool=" + bash("sleep 5") + ",final_text=detached")
    # The drainer is a fork, not dumpable either; the switch lets /proc show it.
    s = ctx.spawn(TMPDIR=str(ctx.work), ARQAN_SHELL_TIMEOUT_MS="200",
                  ARQAN_DUMPABLE="1")
    s.submit("run something slow")
    s.wait_text("detached")
    s.wait_turn_done()
    assert "still running as job 1" in results(ctx)[0], results(ctx)[0]

    drainers = drainers_of(s.proc.pid)
    assert len(drainers) == 1, drainers
    fd_dir = Path(f"/proc/{drainers[0]}/fd")
    targets = sorted(os.readlink(fd_dir / fd) for fd in os.listdir(fd_dir))
    assert targets.count("/dev/null") == 3, targets
    rest = [t for t in targets if t != "/dev/null"]
    assert len(rest) == 2, targets
    assert any(t.startswith("pipe:") for t in rest), targets
    assert any("arqan-bash-" in t for t in rest), targets
