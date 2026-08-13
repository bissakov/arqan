"""Ask/Free approval policy for assistant-issued effectful tools."""

import json


def write_call(path="guarded.txt", content="changed") -> str:
    args = json.dumps({"path": path, "content": content})
    return f"tool=write:{args}"


def test_ask_approves_one_guarded_call(ctx):
    """Yes runs the rendered call and lets the model continue."""
    ctx.scenario(write_call() + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("write it")
    s.wait_status("allow write?")

    assert "write guarded.txt" in s.text(), s.text()
    assert "Yes and remember" in s.text(), s.text()
    s.key("enter")
    s.wait_text("done")
    s.wait_turn_done()

    assert (ctx.work / "guarded.txt").read_text() == "changed"
    assert len(ctx.mock.requests) == 2, ctx.mock.requests


def test_no_denies_without_running_and_the_model_can_adjust(ctx):
    """No appends a stable denial result and continues the provider loop."""
    ctx.scenario(write_call() + ",final_text=understood")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("write it")
    s.wait_status("allow write?")
    s.key("down", "down").sync()
    s.key("enter")
    s.wait_text("understood")
    s.wait_turn_done()

    assert not (ctx.work / "guarded.txt").exists()
    assert ctx.mock.tool_results() == [
        "DENIED: the user did not approve this write call. Do not retry it blindly."
    ]


def test_yes_and_remember_grants_the_class_for_the_process(ctx):
    """A remembered write grant avoids a second picker in a later turn."""
    ctx.scenario(write_call("one.txt") + ",final_text=first")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("first")
    s.wait_status("allow write?")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("first")
    s.wait_turn_done()
    s.submit("/clear")
    s.wait_status("ready")

    ctx.scenario(write_call("two.txt") + ",final_text=second")
    s.submit("second")
    s.wait_text("second")
    s.wait_turn_done()
    assert (ctx.work / "one.txt").exists()
    assert (ctx.work / "two.txt").exists()


def test_escape_denies_the_call(ctx):
    ctx.scenario(write_call() + ",final_text=understood")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("write it")
    s.wait_status("allow write?")
    s.key("esc")
    s.wait_text("understood")
    s.wait_turn_done()
    assert not (ctx.work / "guarded.txt").exists()


def test_free_runs_guarded_calls_without_a_picker(ctx):
    ctx.scenario(write_call() + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="free")
    s.submit("write it")
    s.wait_text("done")
    s.wait_turn_done()
    assert (ctx.work / "guarded.txt").exists()


def test_user_shell_is_direct_even_in_plan_mode(ctx):
    """A user-entered bang command bypasses tool approvals and mode tools."""
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.key("shift-tab")
    s.wait_for(lambda t: s.status_field(2) == "plan", "plan mode")
    s.submit("!printf direct > user-shell.txt")
    s.wait_text("exit 0")
    assert (ctx.work / "user-shell.txt").read_text() == "direct"


def test_one_shot_ask_denies_and_stops(ctx):
    """Headless Ask never runs effects or asks the provider to continue."""
    ctx.scenario(write_call() + ",final_text=must+not+appear")
    out = ctx.run_cli("-p", "write it", ARQAN_PERMISSIONS="ask")

    assert out.returncode == 1, out
    assert out.stdout == "", out.stdout
    assert "approval required for assistant write" in out.stderr, out.stderr
    assert not (ctx.work / "guarded.txt").exists()
    assert len(ctx.mock.requests) == 1, ctx.mock.requests


def test_project_cannot_switch_permissions_to_free(ctx):
    """A cloned repository cannot disable approvals."""
    ctx.write_project_config("permissions = free\n")
    ctx.scenario(write_call())
    out = ctx.run_cli("-p", "write it", ARQAN_PERMISSIONS=None)

    assert out.returncode == 1, out
    assert "permissions" in out.stderr and "may not set it" in out.stderr
    assert not (ctx.work / "guarded.txt").exists()


def test_default_policy_and_status_field_are_ask(ctx):
    """Without an override, Ask is visible in the appended status field."""
    s = ctx.spawn(ARQAN_PERMISSIONS=None, ARQAN_STATUS_FIELDS=None)
    assert s.status_line().endswith("ask"), s.status_line()
    s.submit("/statusline")
    s.wait_status("status line")
    s.settings_select("Permissions")
    assert "[x] Permissions" in s.popup_selected(), s.text()


def test_settings_remembers_the_policy(ctx):
    """Permissions uses the normal state setting path."""
    s = ctx.spawn(ARQAN_PERMISSIONS=None)
    s.open_settings().settings_select("Permissions")
    assert s.settings_option("Permissions") == "Ask", s.text()
    s.key("right").sync()
    assert s.settings_option("Permissions") == "Free", s.text()
    assert ctx.state()["permissions"] == "free", ctx.state()
