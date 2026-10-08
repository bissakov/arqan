"""The system prompt: where it comes from and how its placeholders expand.

The fixture pins ARQAN_SYSTEM_PROMPT, so a case clears it to reach the files.
"""

import json

INTERNAL_WORDS = (
    "Build mode", "build mode", "harness", "rendering", "spill note",
    "turn deadline", "images enabled", "shell_timeout_ms",
    "permissions are set",
)


def system_message(ctx, s, text="hello"):
    """Run one turn and return the system message the provider received."""
    s.submit(text)
    s.wait_text("ok")
    s.wait_turn_done()
    msg = ctx.mock.requests[-1]["messages"][0]
    assert msg["role"] == "system"
    return msg["content"]


def model_view(ctx, **env):
    """The system message and the tools, by name, of one turn."""
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None, **env)
    content = system_message(ctx, s)
    s.close()
    tools = {t["function"]["name"]: t["function"]
             for t in ctx.mock.requests[-1]["tools"]}
    return content, tools


def write_global(ctx, text):
    (ctx.xdg / "arqan").mkdir(parents=True, exist_ok=True)
    (ctx.xdg / "arqan" / "SYSTEM.md").write_text(text)


def test_builtin_prompt_names_tools_and_cwd(ctx):
    """With no SYSTEM.md the built-in template is used, fully expanded."""
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None)
    content = system_message(ctx, s)

    assert "expert coding assistant" in content, content
    for name in ("read", "write", "bash", "patch"):
        assert f"- {name}: " in content, content
    assert "Call ask_user instead of ending your turn with a question" in content
    assert f"Current working directory: {ctx.work}" in content, content
    assert not any(
        field in content for field in ("{tools}", "{ask_user_guidance}", "{cwd}")
    ), content


def test_global_system_md_replaces_the_prompt(ctx):
    """$XDG_CONFIG_HOME/arqan/SYSTEM.md is the prompt, not an addition to it."""
    write_global(ctx, "Always answer in haiku.\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None)
    content = system_message(ctx, s)

    assert content == "Always answer in haiku."
    assert "expert coding assistant" not in content, content


def test_builtin_prompt_says_the_reasoning_is_not_shown(ctx):
    """A list or answer written only in the reasoning never reaches the user."""
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None)
    content = system_message(ctx, s)

    assert "The user sees your replies and tool calls, never your " \
        "reasoning" in content, content
    assert "the user cannot see your reasoning" in content, content


def test_builtin_prompt_says_when_to_stop_and_report(ctx):
    """Carrying a task through has a limit, and the prompt names it."""
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None)
    content = system_message(ctx, s)

    carry = content.index("- Carry a task through to the end")
    stop = content.index("- Stop and report instead of trying again when the "
                         "same approach has failed three times")
    assert carry < stop, content
    assert "what is done and verified, what is done but not verified" \
        in content, content
    assert "Put that decision to the user with ask_user." in content, content


def test_the_stop_rule_names_ask_user_only_when_it_is_offered(ctx):
    """A one-shot run has no picker, so the rule ends without naming one."""
    ctx.scenario("text=ok")
    out = ctx.run_cli("-p", "hello", ARQAN_SYSTEM_PROMPT=None)
    assert out.returncode == 0, out
    content = ctx.mock.requests[-1]["messages"][0]["content"]

    assert "the decision the user needs to make.\n" in content, content
    assert "to the user with ask_user" not in content, content


def test_builtin_prompt_guides_readable_sequential_batches(ctx):
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None)
    content = system_message(ctx, s)

    assert "- batch: " in content, content
    assert "whose arguments are already known" in content, content
    assert "Do not batch a read with an edit" in content, content
    assert "inline scripts or shell redirection" in content, content
    assert "Do not rerun completed steps" in content, content


def test_disabled_batch_has_no_batch_guidance(ctx):
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None, ARQAN_DISABLE_TOOLS="batch")
    content = system_message(ctx, s)

    assert "- batch: " not in content, content
    assert "Use batch for" not in content, content
    assert "Do not rerun completed steps" not in content, content


def test_disabled_editing_tools_have_no_shell_editing_guidance(ctx):
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None, ARQAN_DISABLE_TOOLS="patch,write")
    content = system_message(ctx, s)

    assert "inline scripts or shell redirection" not in content, content


def test_approval_wording_holds_under_both_permission_settings(ctx):
    """Free never asks, so approval is only ever a possibility.

    The model cannot see the setting, so the wording names none. The prompt
    and tools stay the same under both settings, so switching keeps the
    provider's prompt cache."""
    seen = []
    for policy in ("ask", "free"):
        ctx.scenario("text=ok")
        s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None, ARQAN_PERMISSIONS=policy)
        content = system_message(ctx, s)
        s.close()
        tools = {t["function"]["name"]: t["function"]["description"]
                 for t in ctx.mock.requests[-1]["tools"]}
        seen.append((content, tools))
    assert seen[0] == seen[1]

    content, tools = seen[0]
    assert "- Some calls may wait for the user's approval" in content, content
    assert "- Reading a path outside the project may need the user's " \
        "approval" in content, content
    assert tools["read"].endswith(
        "A path outside the project may need the user's approval."), \
        tools["read"]
    bash = tools["bash"]
    assert "runs without asking the user" not in bash, bash
    assert "never needs approval and is the only kind allowed in plan " \
        "mode" in bash, bash
    assert "Any other command" in bash, bash
    assert "may need the user's approval." in bash, bash
    for text in (content, *tools.values()):
        assert "needs the user's approval" not in text, text
        assert "calls wait for" not in text, text
        assert "permissions are set" not in text, text


def test_model_facing_text_names_no_internals(ctx):
    """The model has none of arqan's source, so internal words tell it nothing."""
    content, tools = model_view(ctx)
    text = content + json.dumps(tools)
    for word in INTERNAL_WORDS:
        assert word not in text, word

    assert "- Use batch for ordered tool calls whose arguments are already " \
        "known; each step runs under the same rules as a call on its own\n" \
        in content, content
    assert "a pipeline of reading programs never needs approval, so prefer " \
        "it to an inline script" in content, content
    batch = tools["batch"]["description"]
    assert "Each step runs under the same rules as a call on its own" \
        in batch, batch
    assert "its note naming the full output file" in batch, batch
    timeout = tools["bash"]["parameters"]["properties"]["timeout_ms"]
    assert timeout["description"].startswith(
        "how long to wait before the command becomes a job, in "
        "milliseconds"), timeout


def test_read_describes_images_only_when_they_are_on(ctx):
    """With images off, read has no image behaviour to describe."""
    _, on = model_view(ctx)
    assert "PNG, JPEG, GIF and WebP files return their whole image " \
        "content" in on["read"]["description"], on["read"]
    _, off = model_view(ctx, ARQAN_IMAGES="off")
    assert "image" not in off["read"]["description"].lower(), off["read"]


def test_project_system_md_wins_over_the_global_one(ctx):
    """.arqan/SYSTEM.md is the more local statement, so it is the one used."""
    write_global(ctx, "GLOBAL PROMPT\n")
    ctx.write_file(".arqan/SYSTEM.md", "PROJECT PROMPT\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None)
    content = system_message(ctx, s)

    assert content == "PROJECT PROMPT"


def test_project_system_md_is_found_in_a_parent(ctx):
    """Starting in a subdirectory still finds the project's prompt."""
    ctx.write_file(".arqan/SYSTEM.md", "Root rules apply.\n")
    sub = ctx.work / "src" / "deep"
    sub.mkdir(parents=True)
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None, cwd=str(sub))
    content = system_message(ctx, s)

    assert content == "Root rules apply."


def test_placeholders_expand_in_a_custom_prompt(ctx):
    """{tools} and {cwd} are substituted wherever the prompt puts them."""
    ctx.write_file(".arqan/SYSTEM.md", "Tools:\n{tools}Dir: {cwd}\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None)
    content = system_message(ctx, s)

    assert content.startswith("Tools:\n- read: "), content
    assert content.endswith(f"Dir: {ctx.work}"), content


def test_unknown_braces_are_left_alone(ctx):
    """A prompt that talks about braces keeps them verbatim."""
    ctx.write_file(".arqan/SYSTEM.md", 'Emit {"a": 1} and {unknown} as is.\n')
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None)
    content = system_message(ctx, s)

    assert content == 'Emit {"a": 1} and {unknown} as is.'


def test_explicit_prompt_outranks_the_files(ctx):
    """--system replaces every file, and its placeholders expand too."""
    write_global(ctx, "GLOBAL PROMPT\n")
    ctx.write_file(".arqan/SYSTEM.md", "PROJECT PROMPT\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None, args=["--system", "Only this: {cwd}"])
    content = system_message(ctx, s)

    assert content == f"Only this: {ctx.work}"


def test_oversized_system_md_refuses_startup(ctx):
    """A SYSTEM.md past the limit is an error, not a truncated prompt."""
    ctx.write_file(".arqan/SYSTEM.md", "x" * (1 << 17))
    out = ctx.run_cli("-p", "hi", ARQAN_SYSTEM_PROMPT=None)

    assert out.returncode == 2, (out.returncode, out.stderr)
    assert "SYSTEM.md" in out.stderr and "limit" in out.stderr, out.stderr
    assert out.stdout == "", "no turn may run with a prompt that failed to load"


def test_agents_md_is_appended_to_the_prompt(ctx):
    """AGENTS.md is project context, so it joins the prompt instead of
    replacing it."""
    ctx.write_file("AGENTS.md", "Build with make.\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None)
    content = system_message(ctx, s)

    assert "expert coding assistant" in content, content
    assert "Build with make." in content, content
    assert f'path="{ctx.work}/AGENTS.md"' in content, content


def test_agents_md_applies_to_an_explicit_prompt(ctx):
    """--system is the operator's prompt; the project's context still
    applies."""
    ctx.write_file("AGENTS.md", "Build with make.\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None, args=["--system", "Only this."])
    content = system_message(ctx, s)

    assert content.startswith("Only this."), content
    assert "Build with make." in content, content


def test_agents_md_chain_applies_nearest_last(ctx):
    """A subdirectory refines its parent, so both apply and the nearest is
    read last."""
    ctx.write_file("AGENTS.md", "ROOT CONTEXT\n")
    ctx.write_file("src/AGENTS.md", "SRC CONTEXT\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None, cwd=str(ctx.work / "src"))
    content = system_message(ctx, s)

    assert content.index("ROOT CONTEXT") < content.index("SRC CONTEXT"), content


def test_agents_md_is_not_a_template(ctx):
    """A project doc talking about braces keeps them; only the prompt
    expands."""
    ctx.write_file("AGENTS.md", "Emit {cwd} and {tools} verbatim.\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None, args=["--system", "P"])
    content = system_message(ctx, s)

    assert "Emit {cwd} and {tools} verbatim." in content, content


def test_oversized_agents_md_refuses_startup(ctx):
    """The size limit is the prompt's, and it covers the context too."""
    ctx.write_file("AGENTS.md", "x" * (1 << 17))
    out = ctx.run_cli("-p", "hi", ARQAN_SYSTEM_PROMPT=None)

    assert out.returncode == 2, (out.returncode, out.stderr)
    assert "AGENTS.md" in out.stderr and "limit" in out.stderr, out.stderr
    assert out.stdout == "", "no turn may run with a prompt that failed to load"


def test_env_prompt_outranks_the_files(ctx):
    """ARQAN_SYSTEM_PROMPT does the same, which is what the fixture relies on."""
    ctx.write_file(".arqan/SYSTEM.md", "PROJECT PROMPT\n")
    ctx.scenario("text=ok")
    s = ctx.spawn()
    content = system_message(ctx, s)

    assert content == "You are a test fixture."


def test_agents_md_writable_by_others_is_not_appended(ctx):
    """Anyone could have written it, so it does not get to steer the agent."""
    p = ctx.write_file("AGENTS.md", "PLANTED CONTEXT\n")
    p.chmod(0o666)
    ctx.scenario("text=ok")
    out = ctx.run_cli("-p", "hi", ARQAN_SYSTEM_PROMPT=None)
    assert out.returncode == 0, out.stderr
    content = ctx.mock.requests[-1]["messages"][0]["content"]
    assert "PLANTED CONTEXT" not in content, content
    assert "AGENTS.md" in out.stderr and "writable" in out.stderr, out.stderr


def test_project_system_md_writable_by_others_is_not_used(ctx):
    """The same holds for a project prompt, which replaces the built-in one."""
    p = ctx.write_file(".arqan/SYSTEM.md", "PLANTED PROMPT\n")
    p.chmod(0o666)
    ctx.scenario("text=ok")
    out = ctx.run_cli("-p", "hi", ARQAN_SYSTEM_PROMPT=None)
    assert out.returncode == 0, out.stderr
    content = ctx.mock.requests[-1]["messages"][0]["content"]
    assert "PLANTED PROMPT" not in content, content
    assert "expert coding assistant" in content, content
