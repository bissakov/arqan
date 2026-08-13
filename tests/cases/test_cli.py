"""Command line: help, version, option errors, overrides and one-shot runs."""

import json


def test_help_exits_cleanly(ctx):
    """--help prints the usage on stdout and exits 0 without starting the UI."""
    out = ctx.run_cli("--help")
    assert out.returncode == 0, out
    assert out.stdout.startswith("usage: arqan"), out.stdout
    assert "--model" in out.stdout, out.stdout
    assert "\x1b[" not in out.stdout, "help is not a UI"


def test_short_help_matches_long(ctx):
    """-h is the same output as --help."""
    assert ctx.run_cli("-h").stdout == ctx.run_cli("--help").stdout


def test_version(ctx):
    """--version and -v print the version and nothing else."""
    out = ctx.run_cli("--version")
    assert out.returncode == 0, out
    assert out.stdout == "arqan 0.2.0\n", out.stdout
    assert ctx.run_cli("-v").stdout == out.stdout


def test_unknown_option_is_refused(ctx):
    """An unrecognised option fails with a usage hint instead of opening the UI."""
    out = ctx.run_cli("--etc")
    assert out.returncode == 2, out
    assert "unknown option '--etc'" in out.stderr, out.stderr
    assert "--help" in out.stderr, out.stderr
    assert out.stdout == "", out.stdout


def test_missing_value_is_refused(ctx):
    """An option whose value is missing is an error, not an empty value."""
    out = ctx.run_cli("--model")
    assert out.returncode == 2, out
    assert "requires a value" in out.stderr, out.stderr


def test_bad_max_tokens_is_refused(ctx):
    """--max-tokens validates its number rather than clamping it silently."""
    out = ctx.run_cli("--max-tokens", "zero")
    assert out.returncode == 2, out
    assert "token count" in out.stderr, out.stderr


def test_model_flag_beats_environment(ctx):
    """-m outranks ARQAN_MODEL: it is the more local statement."""
    s = ctx.spawn(args=["-m", "flag-model"])
    assert "flag-model" in s.status_line(), s.status_line()


def test_attached_values(ctx):
    """--model=NAME and -mNAME name the same option value."""
    s = ctx.spawn(args=["--model=long-form"])
    assert "long-form" in s.status_line(), s.status_line()
    s2 = ctx.spawn(args=["-mshort-form"])
    assert "short-form" in s2.status_line(), s2.status_line()


def test_max_tokens_reaches_the_request(ctx):
    """--max-tokens lands in the completion request."""
    ctx.scenario("text=ok")
    s = ctx.spawn(args=["--max-tokens", "77"])
    s.submit("hi")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["max_tokens"] == 77, ctx.mock.requests[-1]


def test_default_max_tokens_fits_a_long_reply(ctx):
    """An unconfigured run sends a cap a long answer does not hit."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.submit("hi")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["max_tokens"] == 32768, ctx.mock.requests[-1]


def test_prompt_flag_runs_one_turn(ctx):
    """-p streams one reply to stdout, with no banner and no prompt echo."""
    ctx.scenario("text=one+shot+reply")
    out = ctx.run_cli("-p", "say it")
    assert out.returncode == 0, out
    assert out.stdout == "one shot reply\n", out.stdout
    assert "arqan 0.2.0" not in out.stdout, "the banner is UI, not output"
    assert "say it" not in out.stdout, "the prompt is the input, not the output"
    assert "hi" not in out.stdout
    body = json.dumps(ctx.mock.requests[-1])
    assert "say it" in body, body


def test_positional_prompt(ctx):
    """A bare argument is the prompt, the same as -p."""
    ctx.scenario("text=positional+works")
    out = ctx.run_cli("positional please")
    assert out.returncode == 0, out
    assert "positional works" in out.stdout, out.stdout


def test_double_dash_ends_options(ctx):
    """After --, a leading dash is prompt text and not an option."""
    ctx.scenario("text=dashed")
    out = ctx.run_cli("--", "--not-an-option")
    assert out.returncode == 0, out
    assert "dashed" in out.stdout, out.stdout
    assert "--not-an-option" in json.dumps(ctx.mock.requests[-1])


def test_two_prompts_are_refused(ctx):
    """Two prompts would silently drop one, so both are refused."""
    out = ctx.run_cli("first", "second")
    assert out.returncode == 2, out
    assert "unexpected argument 'second'" in out.stderr, out.stderr


def test_empty_prompt_is_refused(ctx):
    """An empty prompt has nothing to ask, so it never reaches the provider."""
    out = ctx.run_cli("-p", "")
    assert out.returncode == 2, out
    assert "empty" in out.stderr, out.stderr
    assert ctx.mock.requests == []


def test_prompt_failure_sets_exit_status(ctx):
    """A provider error in a one-shot run is reported by the exit status."""
    ctx.scenario("status=500")
    out = ctx.run_cli("-p", "will fail")
    assert out.returncode == 1, out
    assert out.stdout == "", out.stdout
    assert "provider error" in out.stderr, out.stderr


def test_prompt_stays_plain_on_a_tty(ctx):
    """-p on a terminal prints the reply, not the alternate screen."""
    ctx.scenario("text=tty+oneshot")
    s = ctx.spawn(args=["-p", "on a tty"], wait=False)
    s.wait_text("tty oneshot")
    assert s.wait_exit() == 0
    assert not s.screen.alt_active, "one-shot output must not claim the UI"
    assert "Message arqan" not in s.text(), s.text()


def test_prompt_runs_tools(ctx):
    """A one-shot run drives the same agent loop, tool calls included."""
    ctx.write_file("notes.txt", "written down\n")
    ctx.scenario('tool=read:{"path":"notes.txt"},final_text=I+read+it')
    out = ctx.run_cli("-p", "read the notes")
    assert out.returncode == 0, out
    assert out.stdout == "I read it\n", out.stdout
    assert "tool call read" in out.stderr, out.stderr
    assert "written down" in out.stderr, out.stderr


def test_prompt_withholds_the_interactive_question_tool(ctx):
    """A one-shot request cannot advertise a picker nobody can answer."""
    ctx.scenario("text=no+question+needed")
    out = ctx.run_cli("-p", "make the choice", ARQAN_SYSTEM_PROMPT=None)
    assert out.returncode == 0, out
    names = [
        tool["function"]["name"]
        for tool in ctx.mock.requests[-1].get("tools", [])
    ]
    assert "ask_user" not in names, names
    system = ctx.mock.requests[-1]["messages"][0]["content"]
    assert "Call ask_user" not in system, system


def test_prompt_hides_reasoning_and_intermediate_prose(ctx):
    """Only the last successful non-tool assistant message reaches stdout."""
    ctx.write_file("notes.txt", "result bytes\n")
    ctx.scenario(
        'reasoning=private+trace,prefix=intermediate+,tool=read:{"path":"notes.txt"},final_text=final+only'
    )
    out = ctx.run_cli("-p", "work through it")
    assert out.returncode == 0, out
    assert out.stdout == "final only\n", out.stdout
    assert "private trace" not in out.stdout, out.stdout
    assert "intermediate" not in out.stdout, out.stdout


def test_partial_failed_stream_leaves_stdout_empty(ctx):
    """Bytes received before a stream failure are diagnostics, not an answer."""
    ctx.scenario("text=partial+must+not+leak,chunk=1,abort_after=1")
    out = ctx.run_cli("-p", "fail part way")
    assert out.returncode == 1, out
    assert out.stdout == "", out.stdout
    assert "provider error" in out.stderr, out.stderr


def test_api_flag_picks_the_wire_format(ctx):
    """--api anthropic sends the messages request, key header and all."""
    ctx.scenario("text=from+the+flag")
    out = ctx.run_cli("--api", "anthropic", "-p", "hello")
    assert out.returncode == 0, out
    assert "from the flag" in out.stdout, out.stdout
    assert ctx.mock.keys[-1] == "test-key", ctx.mock.keys
    assert "system" in ctx.mock.requests[-1], ctx.mock.requests[-1]


def test_api_flag_outranks_the_environment(ctx):
    """A flag is the most local statement, here about which API answers."""
    ctx.scenario("text=openai+after+all")
    out = ctx.run_cli("--api", "openai", "-p", "hello", ARQAN_API="anthropic")
    assert out.returncode == 0, out
    assert "openai after all" in out.stdout, out.stdout
    assert ctx.mock.auth[-1] == "Bearer test-key", ctx.mock.auth


def test_bad_api_is_refused(ctx):
    """A wire format nothing speaks is a typo, not a reason to guess."""
    out = ctx.run_cli("--api", "claude")
    assert out.returncode == 2, out
    assert "openai or anthropic" in out.stderr, out.stderr
