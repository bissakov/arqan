"""The system prompt: where it comes from and how its placeholders expand.

The fixture pins YOKE_SYSTEM_PROMPT, so a case clears it to reach the files.
"""


def system_message(ctx, s, text="hello"):
    """Run one turn and return the system message the provider received."""
    s.submit(text)
    s.wait_text("ok")
    s.wait_turn_done()
    msg = ctx.mock.requests[-1]["messages"][0]
    assert msg["role"] == "system"
    return msg["content"]


def write_global(ctx, text):
    (ctx.xdg / "yoke").mkdir(parents=True, exist_ok=True)
    (ctx.xdg / "yoke" / "SYSTEM.md").write_text(text)


def test_builtin_prompt_names_tools_and_cwd(ctx):
    """With no SYSTEM.md the built-in template is used, fully expanded."""
    ctx.scenario("text=ok")
    s = ctx.spawn(YOKE_SYSTEM_PROMPT=None)
    content = system_message(ctx, s)

    assert "expert coding assistant" in content, content
    for name in ("read", "write", "bash", "patch"):
        assert f"- {name}: " in content, content
    assert f"Current working directory: {ctx.work}" in content, content
    assert "{tools}" not in content and "{cwd}" not in content, content


def test_global_system_md_replaces_the_prompt(ctx):
    """$XDG_CONFIG_HOME/yoke/SYSTEM.md is the prompt, not an addition to it."""
    write_global(ctx, "Always answer in haiku.\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(YOKE_SYSTEM_PROMPT=None)
    content = system_message(ctx, s)

    assert content == "Always answer in haiku."
    assert "expert coding assistant" not in content, content


def test_project_system_md_wins_over_the_global_one(ctx):
    """.yoke/SYSTEM.md is the more local statement, so it is the one used."""
    write_global(ctx, "GLOBAL PROMPT\n")
    ctx.write_file(".yoke/SYSTEM.md", "PROJECT PROMPT\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(YOKE_SYSTEM_PROMPT=None)
    content = system_message(ctx, s)

    assert content == "PROJECT PROMPT"


def test_project_system_md_is_found_in_a_parent(ctx):
    """Starting in a subdirectory still finds the project's prompt."""
    ctx.write_file(".yoke/SYSTEM.md", "Root rules apply.\n")
    sub = ctx.work / "src" / "deep"
    sub.mkdir(parents=True)
    ctx.scenario("text=ok")
    s = ctx.spawn(YOKE_SYSTEM_PROMPT=None, cwd=str(sub))
    content = system_message(ctx, s)

    assert content == "Root rules apply."


def test_placeholders_expand_in_a_custom_prompt(ctx):
    """{tools} and {cwd} are substituted wherever the prompt puts them."""
    ctx.write_file(".yoke/SYSTEM.md", "Tools:\n{tools}Dir: {cwd}\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(YOKE_SYSTEM_PROMPT=None)
    content = system_message(ctx, s)

    assert content.startswith("Tools:\n- read: "), content
    assert content.endswith(f"Dir: {ctx.work}"), content


def test_unknown_braces_are_left_alone(ctx):
    """A prompt that talks about braces keeps them verbatim."""
    ctx.write_file(".yoke/SYSTEM.md", 'Emit {"a": 1} and {unknown} as is.\n')
    ctx.scenario("text=ok")
    s = ctx.spawn(YOKE_SYSTEM_PROMPT=None)
    content = system_message(ctx, s)

    assert content == 'Emit {"a": 1} and {unknown} as is.'


def test_explicit_prompt_outranks_the_files(ctx):
    """--system replaces every file, and its placeholders expand too."""
    write_global(ctx, "GLOBAL PROMPT\n")
    ctx.write_file(".yoke/SYSTEM.md", "PROJECT PROMPT\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(YOKE_SYSTEM_PROMPT=None, args=["--system", "Only this: {cwd}"])
    content = system_message(ctx, s)

    assert content == f"Only this: {ctx.work}"


def test_oversized_system_md_refuses_startup(ctx):
    """A SYSTEM.md past the limit is an error, not a truncated prompt."""
    ctx.write_file(".yoke/SYSTEM.md", "x" * (1 << 17))
    out = ctx.run_cli("-p", "hi", YOKE_SYSTEM_PROMPT=None)

    assert out.returncode == 2, (out.returncode, out.stderr)
    assert "SYSTEM.md" in out.stderr and "limit" in out.stderr, out.stderr
    assert out.stdout == "", "no turn may run with a prompt that failed to load"


def test_agents_md_is_appended_to_the_prompt(ctx):
    """AGENTS.md is project context, so it joins the prompt instead of
    replacing it."""
    ctx.write_file("AGENTS.md", "Build with make.\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(YOKE_SYSTEM_PROMPT=None)
    content = system_message(ctx, s)

    assert "expert coding assistant" in content, content
    assert "Build with make." in content, content
    assert f'path="{ctx.work}/AGENTS.md"' in content, content


def test_agents_md_applies_to_an_explicit_prompt(ctx):
    """--system is the operator's prompt; the project's context still
    applies."""
    ctx.write_file("AGENTS.md", "Build with make.\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(YOKE_SYSTEM_PROMPT=None, args=["--system", "Only this."])
    content = system_message(ctx, s)

    assert content.startswith("Only this."), content
    assert "Build with make." in content, content


def test_agents_md_chain_applies_nearest_last(ctx):
    """A subdirectory refines its parent, so both apply and the nearest is
    read last."""
    ctx.write_file("AGENTS.md", "ROOT CONTEXT\n")
    ctx.write_file("src/AGENTS.md", "SRC CONTEXT\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(YOKE_SYSTEM_PROMPT=None, cwd=str(ctx.work / "src"))
    content = system_message(ctx, s)

    assert content.index("ROOT CONTEXT") < content.index("SRC CONTEXT"), content


def test_agents_md_is_not_a_template(ctx):
    """A project doc talking about braces keeps them; only the prompt
    expands."""
    ctx.write_file("AGENTS.md", "Emit {cwd} and {tools} verbatim.\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(YOKE_SYSTEM_PROMPT=None, args=["--system", "P"])
    content = system_message(ctx, s)

    assert "Emit {cwd} and {tools} verbatim." in content, content


def test_oversized_agents_md_refuses_startup(ctx):
    """The size limit is the prompt's, and it covers the context too."""
    ctx.write_file("AGENTS.md", "x" * (1 << 17))
    out = ctx.run_cli("-p", "hi", YOKE_SYSTEM_PROMPT=None)

    assert out.returncode == 2, (out.returncode, out.stderr)
    assert "AGENTS.md" in out.stderr and "limit" in out.stderr, out.stderr
    assert out.stdout == "", "no turn may run with a prompt that failed to load"


def test_env_prompt_outranks_the_files(ctx):
    """YOKE_SYSTEM_PROMPT does the same, which is what the fixture relies on."""
    ctx.write_file(".yoke/SYSTEM.md", "PROJECT PROMPT\n")
    ctx.scenario("text=ok")
    s = ctx.spawn()
    content = system_message(ctx, s)

    assert content == "You are a test fixture."
