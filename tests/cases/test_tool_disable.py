"""Turning tools off: the settings screen, the config key and the flag.

A disabled tool is withheld from the schemas a turn is sent with and refused
if the model calls it anyway, which is what keeps a careless model away from
the shell.
"""

import json
import re


def tool_names(request: dict) -> list[str]:
    return sorted(t["function"]["name"] for t in request.get("tools", []))


def open_tools(s):
    """Open /settings, where the tools are rows like every other setting."""
    s.open_settings()
    return s.settings_select("bash")


def test_the_tools_are_rows_of_the_settings_screen(ctx):
    """Every runnable tool has a checkbox; the plan tools are not rows."""
    s = ctx.spawn()
    s.open_settings().settings_select("write")   # the last row: all eight show
    text = s.text()
    for name in (
        "read", "grep", "find", "internet_search", "page_fetch", "bash", "patch", "write"
    ):
        assert f"[x] {name}" in text, text
    assert "submit_plan" not in text and "ask_user" not in text, text
    ctx.check_screen(s)


def test_a_tool_is_turned_off_where_it_is_read(ctx):
    """No screen behind a row: the checkbox flips in the list it sits in."""
    s = ctx.spawn()
    open_tools(s)
    s.key("right").sync()
    s.wait_text("[ ] bash")
    assert "[x] read" in s.text(), s.text()
    s.key("left").sync()
    s.wait_text("[x] bash")


def test_a_disabled_tool_is_not_sent(ctx):
    """What the model is offered is what the user left on."""
    ctx.scenario("text=fine")
    s = ctx.spawn()
    open_tools(s)
    s.key("space").sync()
    s.wait_text("[ ] bash")
    s.key("esc").sync()
    s.wait_status("ready")

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
    s.key("space").sync()
    s.wait_text("[ ] bash")
    s.key("esc").sync()
    s.wait_status("ready")

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
    s.key("space").sync()
    s.wait_text("[ ] bash")
    s.key("space").sync()
    s.wait_text("[x] bash")
    s.key("esc").sync()
    s.wait_status("ready")

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
    assert names == [
        "ask_user", "find", "grep", "internet_search", "job", "page_fetch",
        "read", "task", "todo"
    ], names


def test_the_flag_keeps_them_out_of_the_prompt(ctx):
    """The listing the model reads describes the tools it actually has."""
    ctx.scenario("text=fine")
    s = ctx.spawn(args=["--disable-tools", "bash"], ARQAN_SYSTEM_PROMPT=None)
    s.submit("say something")
    s.wait_turn_done()

    system = ctx.mock.requests[-1]["messages"][0]["content"]
    assert "- read:" in system, system
    assert "- bash:" not in system, system


def test_the_guidelines_name_only_the_tools_offered(ctx):
    """A guideline about a disabled tool would send the model after it.

    read, grep and find ask before they leave the project, so they go too
    for the approval guideline to have no tool left to describe."""
    ctx.scenario("text=fine")
    s = ctx.spawn(
        args=["--disable-tools", "bash,patch,write,read,grep,find"],
        ARQAN_SYSTEM_PROMPT=None,
    )
    s.submit("say something")
    s.wait_turn_done()

    system = ctx.mock.requests[-1]["messages"][0]["content"]
    guidelines = system[: system.index("Current working directory:")]
    guidelines = guidelines[guidelines.index("Available tools:"):]
    listing = [line for line in guidelines.splitlines() if line.startswith("- ")]
    for line in listing:
        if any(line.startswith(f"- {name}: ") for name in (
            "read", "grep", "find", "internet_search", "page_fetch", "job",
            "todo", "ask_user", "task",
        )):
            continue
        for name in ("bash", "patch", "write"):
            assert not re.search(rf"\b{name}\b", line), (name, line)
    assert "approval" not in guidelines, guidelines


def test_the_guidelines_say_outside_reads_need_approval(ctx):
    """With only the read tools left, approval still applies to them."""
    ctx.scenario("text=fine")
    s = ctx.spawn(
        args=["--disable-tools", "bash,patch,write"], ARQAN_SYSTEM_PROMPT=None
    )
    s.submit("say something")
    s.wait_turn_done()

    system = ctx.mock.requests[-1]["messages"][0]["content"]
    guidelines = system[system.index("Guidelines:"):]
    assert "outside the project needs the user's approval" in guidelines, \
        guidelines


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
    """ARQAN_DISABLE_TOOLS is per invocation, so it replaces the file's list."""
    ctx.write_config("disable_tools = read\n")
    ctx.scenario("text=fine")
    s = ctx.spawn(ARQAN_DISABLE_TOOLS="bash")
    s.submit("say something")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert "read" in names and "bash" not in names, names
