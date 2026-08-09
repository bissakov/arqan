"""Reserved command prefixes and literal escapes."""


def test_unknown_slash_command_never_reaches_the_provider(ctx):
    s = ctx.spawn()
    s.submit("/does-not-exist")
    s.wait_text("unknown command: /does-not-exist")
    assert "type / to see commands" in s.text(), s.text()
    assert ctx.mock.requests == [], ctx.mock.requests
