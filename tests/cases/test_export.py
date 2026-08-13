"""Markdown session exports."""

import re


def test_export_writes_the_session_to_the_requested_path(ctx):
    """An explicit path receives the user and assistant Markdown verbatim."""
    out = ctx.work / "exports" / "session notes.md"
    out.parent.mkdir()
    ctx.scenario("text=An+**answer**+with+Markdown.")
    s = ctx.spawn()
    s.submit("A question with `code`.")
    s.wait_turn_done()

    s.submit("/export exports/session notes.md")
    s.wait_text("exported session to exports/session notes.md")

    assert out.read_text() == (
        "# arqan session\n\n"
        "## User\n\nA question with `code`.\n\n"
        "## Assistant\n\nAn **answer** with Markdown.\n"
    )
    assert len(ctx.mock.requests) == 1, ctx.mock.requests


def test_export_without_a_path_creates_a_named_file_in_cwd(ctx):
    """The automatic name is a Markdown file in the launch directory."""
    ctx.scenario("text=automatic+export")
    s = ctx.spawn()
    s.submit("save this")
    s.wait_turn_done()

    s.submit("/export")
    s.wait_text("exported session to arqan-session-")

    exports = list(ctx.work.glob("arqan-session-*.md"))
    assert len(exports) == 1, exports
    assert re.fullmatch(r"arqan-session-\d{8}-\d{6}\.md", exports[0].name)
    text = exports[0].read_text()
    assert "## User\n\nsave this" in text, text
    assert "## Assistant\n\nautomatic export" in text, text
