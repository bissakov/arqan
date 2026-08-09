"""/settings: the toggles and values of a session, in one screen."""

import re

from .test_provider import select_provider, write_provider


def test_settings_lists_the_toggles_and_the_values(ctx):
    """Every row says what it is and what it currently says."""
    s = ctx.spawn()
    s.open_settings()
    text = s.text()
    for row in ("[ ] Verbose tool output", "[ ] Raw Markdown",
                "[x] Stream replies", "[ ] Ignored files", "[ ] Telemetry",
                "Text wrap", "Mode"):
        assert row in text, text
    assert "Space or Left/Right changes the selected row" in text, text
    ctx.check_screen(s)
    # More rows than the popup holds: the last is reached by moving down.
    s.key(*(["down"] * 14)).sync()
    text = s.text()
    assert "Max tokens" in text and "[x] bash" in text, text


def test_the_description_column_holds_while_scrolling(ctx):
    """Moving one row down moves the selection and nothing else."""
    s = ctx.spawn()
    s.open_settings()

    def column(label):
        for line in s.term.lines():
            if label in line:
                return line.index(label)
        raise AssertionError(f"no row holding {label!r}\n{s.text()}")

    top = column("Every line a tool printed")
    s.key(*(["down"] * 14)).sync()
    assert column("Write a file whole") == top, s.text()


def test_a_tool_row_says_what_the_tool_is_in_one_line(ctx):
    """The model's description is written for a model; a row is not."""
    s = ctx.spawn()
    s.open_settings().settings_select("patch")
    text = s.text()
    assert "Change files with a diff" in text, text
    assert "@@ numbers" not in text, text


def test_the_endpoint_is_not_a_setting(ctx):
    """The model and the provider are chosen by their own commands."""
    s = ctx.spawn()
    s.open_settings()
    s.key(*(["down"] * 13)).sync()
    text = s.text()
    assert "Model" not in text and "Provider" not in text, text


def test_space_toggles_the_selected_row_only(ctx):
    """The checkbox is the answer: it flips, and its neighbours do not."""
    s = ctx.spawn()
    s.open_settings().settings_select("Raw Markdown")
    s.key("space").sync()
    text = s.text()
    assert "[x] Raw Markdown" in text, text
    assert "[ ] Verbose tool output" in text, text


def test_reasoning_rows_do_not_shift_tool_toggle_ids(ctx):
    """A tool row still changes that tool when provider controls precede it."""
    write_provider(ctx, "work", ctx.mock.base_url, key="sk", api="openai")
    with ctx.config_file().open("a") as f:
        f.write("reasoning_efforts = low,high\nreasoning_effort = high\n")
    select_provider(ctx, "work")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.open_settings().settings_select("bash")
    s.key("space").sync()
    assert "[ ] bash" in s.text(), s.text()
    assert "[x] read" in s.text(), s.text()


def test_the_screen_stays_open_across_a_toggle(ctx):
    """Changing one setting is not a reason to ask for the screen again."""
    s = ctx.spawn()
    s.open_settings().settings_select("Verbose tool output")
    s.key("space").sync()
    s.key("space").sync()
    assert "[ ] Verbose tool output" in s.text(), s.text()


def test_enter_and_escape_close_it(ctx):
    """Both keys close; neither submits the composed line."""
    for key in ("enter", "esc"):
        s = ctx.spawn()
        s.open_settings()
        s.key(key)
        s.wait_gone("Verbose tool output")
        assert s.composer_text() == "", s.composer_lines()
        assert s.proc.poll() is None, f"{key} must not end the session"
        s.close()


def test_the_toggles_are_not_commands_of_their_own(ctx):
    """They live here now, so the composer offers no '/raw' to type."""
    s = ctx.spawn()
    s.type("/").sync()
    assert "/settings" not in s.text(), "it is below the eight visible rows"
    s.type("r").sync()
    text = s.text()
    assert "/raw" not in text and "/verbose" not in text, text
    assert "/rewind" in text, text


def test_the_mode_row_switches_mode(ctx):
    """The same switch Shift+Tab makes, named on the status line."""
    s = ctx.spawn()
    assert s.status_field(2) == "build", s.status_line()
    s.settings_act("Mode")
    s.wait_for(lambda t: s.status_field(2) == "plan", "plan mode")
    assert "Plan: read-only" in s.text(), s.text()


def test_show_instructions_replays_sources_without_changing_the_request(ctx):
    """Instruction blocks are presentation-only copies of each source."""
    ctx.write_file(".yoke/SYSTEM.md", "BUILD {cwd}\n{tools}")
    ctx.write_file(".yoke/PLAN.md", "PLAN {cwd}\n{tools}")
    ctx.write_file("AGENTS.md", "ROOT AGENT\n")
    ctx.write_file("src/AGENTS.md", "SRC AGENT\n")
    s = ctx.spawn(cwd=str(ctx.work / "src"), YOKE_SYSTEM_PROMPT=None)

    s.open_settings().settings_select("Show instructions")
    assert "[ ] Show instructions" in s.popup_selected(), s.text()
    s.key("space").sync()
    s.wait_text("[x] Show instructions")
    s.key("esc")
    s.wait_gone("Verbose tool output")
    bottom = s.text()
    assert "ROOT AGENT" in bottom and "SRC AGENT" in bottom, bottom
    assert "AGENTS.md" in bottom, bottom
    s.key(*(["pageup"] * 10)).sync()
    text = s.text()
    assert "Instructions · Build" in text, text
    assert "Project prompt" in text and ".yoke/SYSTEM.md" in text, text
    assert "BUILD " in text, text

    s.settings_act("Mode").key("esc")
    s.key(*(["pageup"] * 10)).sync()
    s.wait_for(lambda t: t.contains("Instructions · Plan"), "plan instructions")
    text = s.text()
    assert "PLAN " in text, text
    assert "BUILD " not in text, text

    ctx.scenario("text=done")
    s.submit("continue")
    s.wait_turn_done()
    req = ctx.mock.requests[-1]
    assert [m["role"] for m in req["messages"]] == ["system", "user"], req
    system = req["messages"][0]["content"]
    assert system.startswith("PLAN "), system
    assert "Instructions ·" not in system, system

    s.settings_toggle("Show instructions")
    assert "Instructions · Plan" not in s.text(), s.text()


def test_max_tokens_steps_in_place_and_is_sent(ctx):
    """The arrows walk the value where it is read, and a turn carries it."""
    ctx.scenario("text=short+answer")
    s = ctx.spawn()
    s.open_settings().settings_select("Max tokens")
    s.key("left").sync()
    # The row and its value, so a frame caught mid-repaint is not an answer.
    s.wait_for(lambda t: re.search(r"Max tokens\s+16384", t.text()),
               "the max tokens row reading 16384")
    s.key("right").key("right").sync()
    s.wait_for(lambda t: re.search(r"Max tokens\s+65536", t.text()),
               "the max tokens row reading 65536")
    s.key("esc")
    s.wait_gone("Max tokens")

    s.submit("say something")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["max_tokens"] == 65536, ctx.mock.requests[-1]


def test_max_tokens_holds_at_the_ends(ctx):
    """A step past the last rung is not a wrap to the other end."""
    s = ctx.spawn()
    s.open_settings().settings_select("Max tokens")
    s.key(*(["left"] * 8)).sync()
    s.wait_for(lambda t: re.search(r"Max tokens\s+1024", t.text()),
               "the max tokens row at its floor")
    s.key("left").sync()
    assert re.search(r"Max tokens\s+1024", s.text()), s.text()


def test_streaming_off_sends_one_document(ctx):
    """The reply arrives whole, and the request says it asked for that."""
    ctx.scenario("text=all+at+once,usage=200/12")
    s = ctx.spawn()
    s.settings_toggle("Stream replies")
    s.submit("say it")
    s.wait_text("all at once")
    s.wait_turn_done()

    assert ctx.mock.requests[-1]["stream"] is False, ctx.mock.requests[-1]


def test_streaming_off_still_runs_tool_calls(ctx):
    """A tool call is a message field rather than a delta; the loop is one."""
    ctx.write_file("notes.txt", "written down\n")
    ctx.scenario('tool=read:{"path":"notes.txt"},final_text=I+read+it')
    s = ctx.spawn()
    s.settings_toggle("Stream replies")
    s.submit("read the notes")
    s.wait_text("I read it")
    s.wait_turn_done()

    text = s.text()
    assert "written down" in text, text
    assert ctx.mock.tool_results()[0].strip() == "written down", (
        ctx.mock.tool_results()
    )


def test_streaming_back_on_keeps_working(ctx):
    """The toggle is per turn, not per session: both paths stay live."""
    ctx.scenario("text=first+one")
    s = ctx.spawn()
    s.settings_toggle("Stream replies")
    s.submit("one")
    s.wait_text("first one")
    s.wait_turn_done()

    ctx.scenario("text=second+one")
    s.settings_toggle("Stream replies")
    s.submit("two")
    s.wait_text("second one")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["stream"] is True, ctx.mock.requests[-1]


def test_settings_choices_survive_a_restart(ctx):
    """Presentation, request, mode, token, and tool choices share UI state."""
    s = ctx.spawn()
    for label in (
        "Verbose tool output", "Raw Markdown", "Stream replies",
        "Ignored files", "Text wrap", "Mode", "Show instructions",
        "Max tokens", "bash",
    ):
        s.open_settings().settings_select(label)
        s.key("space").sync()
        s.wait_status("settings")
        s.key("esc").sync()
        s.wait_status("ready")
    s.submit("/exit")
    s.wait_exit()

    again = ctx.spawn()
    again.open_settings()
    text = again.text()
    for label in ("Verbose tool output", "Raw Markdown", "Ignored files",
                  "Show instructions"):
        assert f"[x] {label}" in text, text
    assert "[ ] Stream replies" in text, text
    again.settings_select("Text wrap")
    assert "Justified" in again.popup_selected(), again.text()
    again.settings_select("Mode")
    assert "Plan:" in again.popup_selected(), again.text()
    again.settings_select("Max tokens")
    assert "65536" in again.popup_selected(), again.text()
    again.settings_select("bash")
    assert "[ ] bash" in again.popup_selected(), again.text()
    again.key("esc")

    ctx.scenario("text=restored")
    again.submit("verify")
    again.wait_turn_done()
    req = ctx.mock.requests[-1]
    assert req["stream"] is False, req
    assert req["max_tokens"] == 65536, req
    names = [t["function"]["name"] for t in req["tools"]]
    assert "bash" not in names, names


def test_state_precedence_is_below_environment_and_cli(ctx):
    state = ctx.state_file()
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text(
        "ui_stream = false\nui_mode = plan\nui_max_tokens = 2048\n"
        "ui_disable_tools = read\n"
    )
    ctx.write_config(
        "stream = false\nmode = plan\nmax_tokens = 1024\n"
        "disable_tools = write\n"
    )
    ctx.scenario("text=precedence")
    s = ctx.spawn(
        args=["--max-tokens", "8192", "--disable-tools", "bash"],
        YOKE_STREAM="true", YOKE_MODE="build", YOKE_MAX_TOKENS="4096",
    )
    s.submit("verify")
    s.wait_turn_done()
    req = ctx.mock.requests[-1]
    assert req["stream"] is True, req
    assert req["max_tokens"] == 8192, req
    names = [t["function"]["name"] for t in req["tools"]]
    assert "bash" not in names and "read" in names and "write" in names, names
    assert s.status_field(2) == "build", s.status_line()


def test_persistence_failure_is_reported_but_the_change_applies(ctx):
    s = ctx.spawn(HOME="/proc/yoke-unwritable", XDG_STATE_HOME=None)
    s.open_settings().settings_select("Stream replies")
    s.key("space").sync()
    s.wait_text("setting changed but was not remembered")
    assert "[ ] Stream replies" in s.text(), s.text()
