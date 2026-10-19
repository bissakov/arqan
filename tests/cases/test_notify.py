"""Desktop notifications: OSC 9 to the terminal, and the notify_command hook."""

import json
import os
import time

from tests.mockprovider import Scenario

TMUX_WRAPPED = b"\x1bPtmux;\x1b\x1b]9;arqan: "


def bare_osc9_written(raw: bytes) -> bool:
    return b"\x1b]9;" in raw.replace(b"\x1b\x1b]9;", b"")


def wait_notification(s, text: str) -> list[str]:
    """The escape is written just before the picker draws, so poll for it."""
    s.wait_for(lambda _: text in s.screen.notifications, repr(text))
    return s.screen.notifications


def test_a_finished_turn_notifies_over_osc9(ctx):
    """The terminal is handed the notification; arqan links no OS API."""
    ctx.scenario("text=all+done+here")
    s = ctx.spawn(ARQAN_NOTIFY_MIN_MS="0")
    s.submit("do the thing")
    s.wait_text("all done here")
    s.wait_turn_done()
    assert s.screen.notifications, "no OSC 9 was written"
    said = s.screen.notifications[-1]
    assert said.startswith("arqan: "), said
    assert "all done here" in said, said
    assert TMUX_WRAPPED not in bytes(s.raw), bytes(s.raw)[-400:]


def test_under_tmux_the_notification_goes_through_passthrough(ctx):
    """tmux drops a bare OSC 9; only its passthrough wrapper gets out."""
    ctx.scenario("text=all+done+here")
    s = ctx.spawn(ARQAN_NOTIFY_MIN_MS="0", TMUX="/tmp/tmux-1000/default,1,0")
    s.submit("do the thing")
    s.wait_text("all done here")
    s.wait_turn_done()
    raw = bytes(s.raw)
    assert TMUX_WRAPPED in raw, raw[-400:]
    assert not bare_osc9_written(raw), raw[-400:]


def test_over_ssh_from_tmux_term_still_names_tmux(ctx):
    """ssh drops $TMUX but carries TERM, so a remote session wraps as well."""
    ctx.scenario("text=all+done+here")
    s = ctx.spawn(ARQAN_NOTIFY_MIN_MS="0", TERM="tmux-256color", TMUX=None)
    s.submit("do the thing")
    s.wait_text("all done here")
    s.wait_turn_done()
    raw = bytes(s.raw)
    assert TMUX_WRAPPED in raw, raw[-400:]
    assert not bare_osc9_written(raw), raw[-400:]


def test_term_screen_alone_is_not_taken_for_tmux(ctx):
    """GNU screen sets TERM=screen too and would print the wrapper as text."""
    ctx.scenario("text=all+done+here")
    s = ctx.spawn(ARQAN_NOTIFY_MIN_MS="0", TERM="screen-256color", TMUX=None)
    s.submit("do the thing")
    s.wait_text("all done here")
    s.wait_turn_done()
    raw = bytes(s.raw)
    assert TMUX_WRAPPED not in raw, raw[-400:]
    assert s.screen.notifications, "no OSC 9 was written"


def test_a_short_turn_passes_in_silence(ctx):
    """Under notify_min_ms the user was watching, so nothing interrupts them."""
    ctx.scenario("text=quick")
    s = ctx.spawn()   # the default floor is ten seconds
    s.submit("be quick")
    s.wait_text("quick")
    s.wait_turn_done()
    assert s.screen.notifications == [], s.screen.notifications


def test_notify_off_writes_nothing(ctx):
    """'off' silences the escape route entirely."""
    ctx.scenario("text=all+done+here")
    s = ctx.spawn(ARQAN_NOTIFY="off", ARQAN_NOTIFY_MIN_MS="0")
    s.submit("do the thing")
    s.wait_text("all done here")
    s.wait_turn_done()
    assert s.screen.notifications == [], s.screen.notifications


def test_the_hook_is_a_route_of_its_own(ctx):
    """'notify = off' silences the escape; a hook was asked for by name."""
    out = ctx.work / "hook.json"
    script = ctx.work / "hook.sh"
    script.write_text(f'#!/bin/sh\ncat > "{out}"\n')
    os.chmod(script, 0o755)

    ctx.scenario("text=all+done+here")
    s = ctx.spawn(ARQAN_NOTIFY="off", ARQAN_NOTIFY_MIN_MS="0",
                  ARQAN_NOTIFY_COMMAND=str(script))
    s.submit("do the thing")
    s.wait_text("all done here")
    s.wait_turn_done()

    deadline = time.time() + 10.0
    while time.time() < deadline and not out.exists():
        time.sleep(0.05)
    assert out.exists(), "the hook never ran"
    assert s.screen.notifications == [], s.screen.notifications


def test_notify_bel_rings_instead(ctx):
    """A terminal without OSC 9 can still be configured to catch the bell."""
    ctx.scenario("text=all+done+here")
    s = ctx.spawn(ARQAN_NOTIFY="bel", ARQAN_NOTIFY_MIN_MS="0")
    before = s.screen.bell_count
    s.submit("do the thing")
    s.wait_text("all done here")
    s.wait_turn_done()
    assert s.screen.bell_count > before, s.screen.bell_count
    assert s.screen.notifications == [], s.screen.notifications


def test_a_reply_cannot_smuggle_escapes_into_the_sequence(ctx):
    """Model text closing the OSC string would run as commands after it."""
    ctx.scenario(Scenario(text="one\x1b]52;c;aGk=\x07two"))
    s = ctx.spawn(ARQAN_NOTIFY_MIN_MS="0")
    s.submit("say something")
    s.wait_turn_done()
    assert s.screen.notifications, "no OSC 9 was written"
    said = s.screen.notifications[-1]
    assert "\x1b" not in said and "\x07" not in said, repr(said)
    assert s.screen.clipboard in (None, ""), repr(s.screen.clipboard)


def test_a_provider_error_notifies(ctx):
    """Walking away from a turn that failed should not cost the whole wait."""
    ctx.scenario("fail_times=9")
    s = ctx.spawn(ARQAN_NOTIFY_MIN_MS="0")
    s.submit("do the thing")
    s.wait_turn_done()
    assert s.screen.notifications, "no OSC 9 was written"
    assert s.screen.notifications[-1].startswith("arqan: "), (
        s.screen.notifications
    )


def test_a_project_config_may_not_set_notify_command(ctx):
    """It names a program arqan runs, so a clone does not get to choose it."""
    ctx.write_project_config('notify_command = "/bin/echo owned"\n')
    ctx.scenario("text=ok")
    out = ctx.run_cli("-p", "hello")
    assert "notify_command" in out.stderr, out.stderr
    assert "may not set it" in out.stderr, out.stderr


def test_the_hook_runs_with_the_turn_on_its_stdin(ctx):
    """The one route that survives ssh, tmux and a terminal without OSC 9."""
    out = ctx.work / "hook.json"
    script = ctx.work / "hook.sh"
    script.write_text(f'#!/bin/sh\ncat > "{out}"\n')
    os.chmod(script, 0o755)

    ctx.scenario("text=all+done+here")
    s = ctx.spawn(ARQAN_NOTIFY_MIN_MS="0", ARQAN_NOTIFY_COMMAND=str(script))
    s.submit("do the thing")
    s.wait_text("all done here")
    s.wait_turn_done()

    deadline = time.time() + 10.0
    while time.time() < deadline and not out.exists():
        time.sleep(0.05)
    assert out.exists(), "the hook never ran"
    payload = json.loads(out.read_text())
    assert payload["kind"] == "turn-done", payload
    assert "all done here" in payload["text"], payload
    assert payload["cwd"] == str(ctx.work), payload


def test_the_hook_gets_no_api_key(ctx):
    out = ctx.work / "hook.env"
    script = ctx.work / "hook.sh"
    script.write_text(f'#!/bin/sh\nenv > "{out}.part" && mv "{out}.part" "{out}"\n')
    os.chmod(script, 0o755)

    ctx.scenario("text=all+done+here")
    s = ctx.spawn(ARQAN_NOTIFY_MIN_MS="0", ARQAN_NOTIFY_COMMAND=str(script))
    s.submit("do the thing")
    s.wait_text("all done here")
    s.wait_turn_done()

    deadline = time.time() + 10.0
    while time.time() < deadline and not out.exists():
        time.sleep(0.05)
    assert out.exists(), "the hook never ran"
    names = [line.split("=", 1)[0] for line in out.read_text().splitlines()]
    assert "HOME" in names, names
    assert "ARQAN_API_KEY" not in names, names


# Ghostty uses the notification text as its ID, and Plasma shows no popup for
# a replacement of one that timed out. A text that names what is asked differs
# from one request to the next, so each one pops up.


def test_a_question_notification_names_the_question(ctx):
    """The fixed text never popped up again after the first question."""
    question = json.dumps({
        "question": "Which fix should the agent apply?",
        "options": [{"label": "the first"}, {"label": "the second"}],
    })
    ctx.scenario(f"tool=ask_user:{question},final_text=ok")
    s = ctx.spawn()
    s.submit("fix it")
    s.wait_status("pick an answer")
    said = wait_notification(s, "arqan: Which fix should the agent apply?")
    assert "arqan: the assistant asked a question" not in said, said
    s.key("esc")
    s.wait_turn_done()


def test_a_write_approval_notification_names_the_path(ctx):
    """The picker title alone is the same for every write."""
    args = json.dumps({"path": "guarded.txt", "content": "changed"})
    ctx.scenario(f"tool=write:{args},final_text=understood")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("write it")
    s.wait_status("allow write?")
    wait_notification(s, "arqan: allow write: guarded.txt")
    s.key("esc")
    s.wait_text("understood")
    s.wait_turn_done()


def test_a_shell_approval_notification_names_the_command(ctx):
    """The command, not its description, is what the user approves."""
    args = json.dumps({"command": "touch made.txt\necho second line",
                       "description": "make a file"})
    ctx.scenario(f"tool=bash:{args},final_text=understood")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("make it")
    s.wait_status("allow bash?")
    wait_notification(s, "arqan: allow bash: touch made.txt echo second line")
    s.key("esc")
    s.wait_text("understood")
    s.wait_turn_done()
    assert not (ctx.work / "made.txt").exists()


def test_a_patch_approval_notification_names_the_file(ctx):
    """A patch names its file the way the transcript does."""
    patch = ("--- a/one.txt\n+++ b/one.txt\n@@\n-old\n+new\n")
    (ctx.work / "one.txt").write_text("old\n")
    args = json.dumps({"patch": patch})
    ctx.scenario(f"tool=patch:{args},final_text=understood")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("patch it")
    s.wait_status("allow patch?")
    wait_notification(s, "arqan: allow patch: one.txt")
    s.key("esc")
    s.wait_text("understood")
    s.wait_turn_done()


def test_a_plan_notification_names_the_plan(ctx):
    """Heading marks are dropped so the plan's own words lead."""
    plan = "## Fix the stale frame\n\n1. Read the file\n2. Change the line"
    ctx.scenario(f"tool=submit_plan:{json.dumps({'plan': plan})}")
    s = ctx.spawn()
    s.key("shift-tab")
    s.wait_for(lambda _: s.status_field(2) == "plan", "plan mode")
    s.submit("plan the change")
    s.wait_status("continue?")
    wait_notification(
        s, "arqan: plan ready: Fix the stale frame 1. Read the file "
           "2. Change the line")
    s.key("esc")
    s.wait_turn_done()
