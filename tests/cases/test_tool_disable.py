"""Turning tools off: the settings screen, the config key and the flag.

A disabled tool is withheld from the schemas a turn is sent with and refused
if the model calls it anyway, which is what keeps a careless model away from
the shell.
"""

import json


def tool_names(request: dict) -> list[str]:
    return sorted(t["function"]["name"] for t in request.get("tools", []))


def open_tools(s):
    """Open /settings and step into the Tools screen."""
    s.open_settings().settings_select("Tools").key("space").sync()
    return s.wait_text("[x] bash")


def test_the_tools_screen_lists_what_can_be_turned_off(ctx):
    """Every runnable tool has a checkbox; the plan tools are not rows."""
    s = ctx.spawn()
    open_tools(s)
    text = s.text()
    for name in ("read", "grep", "find", "bash", "patch", "write"):
        assert f"[x] {name}" in text, text
    assert "submit_plan" not in text and "ask_user" not in text, text
    ctx.check_screen(s)


def test_the_settings_row_says_which_tools_are_off(ctx):
    """The screen behind the row is summarised by it."""
    s = ctx.spawn()
    s.open_settings()
    assert "Every tool is available" in s.text(), s.text()
    s.settings_select("Tools").key("space").sync()
    s.wait_text("[x] bash")
    s.settings_select("bash").key("space").sync()
    s.wait_text("[ ] bash")
    s.key("esc")
    s.wait_for(lambda t: t.contains("off: bash"), "the summary to name bash")
    assert "5 of 6" in s.text(), s.text()


def test_a_disabled_tool_is_not_sent(ctx):
    """What the model is offered is what the user left on."""
    ctx.scenario("text=fine")
    s = ctx.spawn()
    open_tools(s)
    s.settings_select("bash").key("space").sync()
    s.wait_text("[ ] bash")
    s.key("esc").key("esc")
    s.wait_gone("Verbose tool output")

    s.submit("say something")
    s.wait_turn_done()
    names = tool_names(ctx.mock.requests[-1])
    assert "bash" not in names, names
    assert "read" in names, names


def test_a_disabled_tool_is_refused_when_called_anyway(ctx):
    """A schema offered before the toggle is still in the model's context."""
    args = json.dumps({"command": "touch ran.txt"})
    ctx.scenario(f"tool=bash:{args},final_text=understood")
    s = ctx.spawn()
    open_tools(s)
    s.settings_select("bash").key("space").sync()
    s.wait_text("[ ] bash")
    s.key("esc").key("esc")
    s.wait_gone("Verbose tool output")

    s.submit("run it")
    s.wait_text("understood")
    s.wait_turn_done()

    assert "bash is disabled" in s.text(), s.text()
    assert not (ctx.work / "ran.txt").exists(), "a disabled tool ran"


def test_a_tool_turned_back_on_is_sent_again(ctx):
    """The checkbox is the setting: flipping it twice is no change at all."""
    ctx.scenario("text=fine")
    s = ctx.spawn()
    open_tools(s)
    s.settings_select("bash").key("space").sync()
    s.wait_text("[ ] bash")
    s.key("space").sync()
    s.wait_text("[x] bash")
    s.key("esc").key("esc")
    s.wait_gone("Verbose tool output")

    s.submit("say something")
    s.wait_turn_done()
    assert "bash" in tool_names(ctx.mock.requests[-1]), ctx.mock.requests[-1]


def test_the_flag_disables_a_list(ctx):
    """--disable-tools takes several names at once."""
    ctx.scenario("text=fine")
    s = ctx.spawn(args=["--disable-tools", "bash,write,patch"])
    s.submit("say something")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert names == ["find", "grep", "read"], names


def test_the_flag_keeps_them_out_of_the_prompt(ctx):
    """The listing the model reads describes the tools it actually has."""
    ctx.scenario("text=fine")
    s = ctx.spawn(args=["--disable-tools", "bash"], YOKE_SYSTEM_PROMPT=None)
    s.submit("say something")
    s.wait_turn_done()

    system = ctx.mock.requests[-1]["messages"][0]["content"]
    assert "- read:" in system, system
    assert "- bash:" not in system, system


def test_an_unknown_tool_name_is_refused(ctx):
    """A typo in a list whose point is that bash cannot run is not ignored."""
    out = ctx.run_cli("--disable-tools", "bash,shel", "-p", "hi")
    assert out.returncode == 2, out
    assert "no tool named 'shel'" in out.stderr, out.stderr


def test_a_plan_tool_cannot_be_disabled(ctx):
    """The agent loop answers those, so 'off' would mean a mode that ends."""
    out = ctx.run_cli("--disable-tools", "submit_plan", "-p", "hi")
    assert out.returncode == 2, out
    assert "no tool named 'submit_plan'" in out.stderr, out.stderr


def test_the_config_key_disables_tools(ctx):
    """disable_tools in the config file is the setting that outlives a run."""
    ctx.write_config("disable_tools = bash, write\n")
    ctx.scenario("text=fine")
    s = ctx.spawn()
    s.submit("say something")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert "bash" not in names and "write" not in names, names
    assert "patch" in names, names


def test_the_environment_beats_the_config_file(ctx):
    """YOKE_DISABLE_TOOLS is per invocation, so it replaces the file's list."""
    ctx.write_config("disable_tools = read\n")
    ctx.scenario("text=fine")
    s = ctx.spawn(YOKE_DISABLE_TOOLS="bash")
    s.submit("say something")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert "read" in names and "bash" not in names, names
