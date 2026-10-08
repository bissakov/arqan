"""Desktop notifications: OSC 9 to the terminal, and the notify_command hook."""

import json
import os
import time

from tests.mockprovider import Scenario

TMUX_WRAPPED = b"\x1bPtmux;\x1b\x1b]9;arqan: "


def bare_osc9_written(raw: bytes) -> bool:
    return b"\x1b]9;" in raw.replace(b"\x1b\x1b]9;", b"")


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
