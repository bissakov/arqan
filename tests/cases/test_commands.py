"""Reserved command prefixes and literal escapes."""


def test_unknown_slash_command_never_reaches_the_provider(ctx):
    s = ctx.spawn()
    s.submit("/does-not-exist")
    s.wait_text("unknown command: /does-not-exist")
    assert "type / to see commands" in s.text(), s.text()
    assert ctx.mock.requests == [], ctx.mock.requests


def test_about_is_a_modal_page_and_keeps_the_conversation(ctx):
    """The credits are UI chrome, not a message sent to the provider."""
    ctx.scenario("text=conversation+stays")
    s = ctx.spawn()
    s.submit("start here")
    s.wait_turn_done()

    s.submit("/about")
    s.wait_text("Alikhan Bissakov")
    assert len(ctx.mock.requests) == 1, ctx.mock.requests
    s.key("esc")
    s.wait_gone("Alikhan Bissakov")
    assert "conversation stays" in s.text(), s.text()


def test_help_starts_with_a_user_message_and_waits(ctx):
    """The live diagnostic prompt is saved and shown without a request."""
    config = ctx.write_config(
        "verbose_tools = true\n"
        "raw_markdown = true\n"
        "show_ignored = true\n"
        "wrap = justified\n"
        "disable_tools = bash\n"
        "\n"
        "[provider configured-one]\n"
        "base_url = https://one.example/v1\n"
        "model = first-model\n"
        "api = openai\n"
        "\n"
        "[provider configured-two]\n"
        "base_url = https://two.example/v1\n"
        "model = second-model\n"
        "api = anthropic\n"
    )
    ctx.scenario("text=old+conversation")
    s = ctx.spawn()
    s.submit("old question")
    s.wait_turn_done()

    s.submit("/help")
    s.wait_text("message before taking action.")
    assert "old conversation" not in s.text(), s.text()
    assert len(ctx.mock.requests) == 1, ctx.mock.requests

    ctx.scenario("text=I+can+help")
    s.submit("How do I configure a provider?")
    s.wait_turn_done()
    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == ["system", "user", "user"], messages
    help_text = messages[1]["content"]
    assert help_text.startswith("# yoke help context\n"), help_text
    for expected in (
        "## Effective configuration",
        "verbose tool output: on",
        "raw Markdown: on",
        "ignored files in picker: on",
        "text wrap: justified",
        "bash: disabled",
        "/provider: Switch provider, or add one",
        "configured-one",
        "https://one.example/v1",
        "configured-two",
        "API key: missing",
        str(config),
        "regular file",
        "## Effective prompt sources",
        "## Resolved paths",
    ):
        assert expected in help_text, (expected, help_text)
    assert "test-key" not in help_text, help_text
    assert messages[2]["content"] == "How do I configure a provider?"
