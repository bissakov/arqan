"""Session names: /title by hand, and the small model naming a session.

The name lives in a `<session>.title` sidecar beside the session file, so the
picker reads it back rather than deriving it. The mock answers the naming
request from the active scenario, so what a session ends up called is fixed
by the case rather than by a model.
"""

import re

TIMESTAMP = re.compile(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d")


def sessions_dir(ctx):
    """The per-cwd session directory under $HOME/.local/share."""
    root = ctx.home / ".local" / "share" / "arqan" / "sessions"
    dirs = [p for p in root.iterdir() if p.is_dir()] if root.exists() else []
    assert len(dirs) == 1, sorted(p.name for p in dirs)
    return dirs[0]


def session_files(ctx, suffix: str):
    return sorted(p for p in sessions_dir(ctx).iterdir() if p.name.endswith(suffix))


def one_turn(ctx, prompt="remember the cat", reply="noted", **env):
    """A session with one completed turn, which is the least a name needs."""
    ctx.scenario(f"text={reply.replace(' ', '+')}")
    s = ctx.spawn(**env)
    s.submit(prompt)
    s.wait_text(reply)
    s.wait_turn_done()
    return s


def row_with(s, needle: str) -> str:
    """The lowest row holding `needle`.

    The lowest, because a prompt shown in a picker row is also on screen in
    the transcript above it, and the row is what these cases are about.
    """
    rows = [line for line in s.screen.lines() if needle in line]
    return rows[-1] if rows else ""

# ---- /title ---------------------------------------------------------------
def test_title_names_the_live_session_and_resume_lists_it(ctx):
    """The name is the row; the timestamp and the first prompt sit beside it."""
    s = one_turn(ctx)
    s.submit("/title my session")
    s.wait_text("session named: my session")

    titles = session_files(ctx, ".title")
    assert len(titles) == 1, titles
    assert titles[0].read_text() == "my session", titles[0].read_text()

    s.submit("/resume")
    s.wait_status("pick a session")
    row = row_with(s, "my session")
    assert row, s.text()
    assert TIMESTAMP.search(row), row
    assert "remember the cat" in row, row


def test_title_before_any_message_answers_in_the_popup_slot(ctx):
    """A session with no file behind it has nothing a name would name."""
    s = ctx.spawn()
    s.submit("/title")
    s.wait_text("nothing to name yet")
    rows = s.screen.lines()
    assert "nothing to name yet" in rows[s.screen.rows - 6], rows[s.screen.rows - 8:]
    assert s.status_kind() == "ready", s.status_line()


def test_bare_title_edits_the_name_and_an_empty_answer_clears_it(ctx):
    """The editor opens prefilled; accepting nothing leaves the session unnamed."""
    s = one_turn(ctx)
    s.submit("/title alpha bravo")
    s.wait_text("session named: alpha bravo")

    # Not submit(): the editor refills the composer with the current name, so
    # the empty composer submit() waits for may never be on screen.
    s.type("/title").sync()
    s.key("enter")
    s.wait_text("session title")
    assert s.composer_text() == "alpha bravo", s.composer_lines()

    s.key("ctrl-u").sync()
    s.key("enter")
    s.wait_text("session title cleared")
    assert session_files(ctx, ".title") == [], session_files(ctx, ".title")

    s.submit("/resume")
    s.wait_status("pick a session")
    row = row_with(s, "remember the cat")
    assert row and TIMESTAMP.search(row), s.text()
    assert "alpha bravo" not in s.text(), s.text()


# ---- automatic naming -----------------------------------------------------
def test_the_first_turn_asks_the_small_model_for_a_name(ctx):
    """One extra request, to the small model, carrying the exchange to name."""
    s = one_turn(ctx, ARQAN_SMALL_MODEL="mock-small")
    s.wait_text("session named: noted")

    req = ctx.mock.requests[-1]
    assert req["model"] == "mock-small", req["model"]
    assert "tools" not in req, sorted(req)
    messages = req["messages"]
    assert [m["role"] for m in messages] == ["system", "user"], messages
    assert "name a conversation" in messages[0]["content"], messages[0]["content"]
    assert "Name the conversation below." in messages[1]["content"], messages[1]
    assert "remember the cat" in messages[1]["content"], messages[1]
    assert "noted" in messages[1]["content"], messages[1]

    jsonl = session_files(ctx, ".jsonl")
    titles = session_files(ctx, ".title")
    assert len(jsonl) == 1 and len(titles) == 1, (jsonl, titles)
    assert titles[0].name == jsonl[0].name[: -len(".jsonl")] + ".title", titles
    assert titles[0].read_text() == "noted", titles[0].read_text()


def test_a_named_session_is_not_named_again(ctx):
    """One name per session: the second turn spends no request on it."""
    s = one_turn(ctx, ARQAN_SMALL_MODEL="mock-small")
    s.wait_text("session named: noted")
    after_first = len(ctx.mock.requests)

    ctx.scenario("text=still+here")
    s.submit("and again")
    s.wait_text("still here")
    s.wait_turn_done()
    assert len(ctx.mock.requests) == after_first + 1, [
        r["model"] for r in ctx.mock.requests
    ]
    assert session_files(ctx, ".title")[0].read_text() == "noted"


def test_without_a_small_model_nothing_is_named(ctx):
    """The default session directory holds the transcript and nothing else."""
    s = one_turn(ctx)
    assert len(ctx.mock.requests) == 1, [r["model"] for r in ctx.mock.requests]
    files = sorted(p.name for p in sessions_dir(ctx).iterdir())
    assert len(files) == 1 and files[0].endswith(".jsonl"), files
    assert "session named" not in s.text(), s.text()


def test_auto_title_off_suppresses_naming(ctx):
    """A small model is configured, and the setting still decides."""
    ctx.write_config("auto_title = false\n")
    s = one_turn(ctx, ARQAN_SMALL_MODEL="mock-small")
    assert len(ctx.mock.requests) == 1, [r["model"] for r in ctx.mock.requests]
    assert session_files(ctx, ".title") == [], session_files(ctx, ".title")
    assert "session named" not in s.text(), s.text()


def test_title_auto_names_on_demand_and_renames(ctx):
    """/title auto asks even for a session that was named by hand."""
    # Naming stays on demand here, so the rename is the only request it makes.
    s = one_turn(ctx, ARQAN_SMALL_MODEL="mock-small", ARQAN_AUTO_TITLE="false")
    s.submit("/title by hand")
    s.wait_text("session named: by hand")

    ctx.scenario("text=Cat+memory+notes")
    s.submit("/title auto")
    s.wait_text("session named: Cat memory notes")
    assert ctx.mock.requests[-1]["model"] == "mock-small", ctx.mock.requests[-1]
    assert session_files(ctx, ".title")[0].read_text() == "Cat memory notes"


def test_title_auto_without_a_small_model_says_so(ctx):
    """The manual path reports the missing piece; the automatic one is silent."""
    s = one_turn(ctx)
    s.submit("/title auto")
    s.wait_text("no small model is configured")
    assert len(ctx.mock.requests) == 1, [r["model"] for r in ctx.mock.requests]
    assert session_files(ctx, ".title") == [], session_files(ctx, ".title")


def test_a_long_quoted_multiline_answer_becomes_one_short_line(ctx):
    """The sanitizer is what stands between a model and a popup row."""
    long_name = "The " + "very " * 30 + "long name"
    ctx.scenario(f'text="{long_name.replace(" ", "+")}".\\nSecond+line+here')
    s = ctx.spawn(ARQAN_SMALL_MODEL="mock-small")
    s.submit("remember the cat")
    s.wait_turn_done()
    s.wait_text("session named:")

    stored = session_files(ctx, ".title")[0].read_text()
    assert "\n" not in stored, repr(stored)
    assert len(stored) <= 64, (len(stored), stored)
    assert stored[0] != '"' and not stored.endswith("."), repr(stored)
    assert stored.startswith("The very very"), repr(stored)
    assert "Second line" not in stored, repr(stored)


def test_a_fork_keeps_the_name(ctx):
    """A fork is the same thread continuing, so it carries the name over."""
    s = one_turn(ctx)
    s.submit("/title kept name")
    s.wait_text("session named: kept name")
    s.submit("/fork")
    s.wait_text("forked: this copy continues")

    jsonl = session_files(ctx, ".jsonl")
    titles = session_files(ctx, ".title")
    assert len(jsonl) == 2 and len(titles) == 2, (jsonl, titles)
    assert [p.read_text() for p in titles] == ["kept name", "kept name"], titles


# ---- /model ---------------------------------------------------------------
def test_ctrl_s_sets_and_clears_the_providers_small_model(ctx):
    """The small model belongs to the provider, so it lands in its section."""
    ctx.scenario("models=alpha|beta|gamma")
    ctx.write_config(
        f'provider = "mock"\n\n[providers.mock]\n'
        f'base_url = "{ctx.mock.base_url}"\napi = "openai"\nmodel = "alpha"\n'
    )
    s = ctx.spawn(ARQAN_MODEL=None, ARQAN_BASE_URL=None)
    s.submit("/model")
    s.wait_status("pick a model")
    s.key("down").sync()
    assert "beta" in s.popup_selected(), s.popup_selected()

    s.key("ctrl-s").sync()
    s.wait_for(lambda t: "small" in row_with(s, "beta"), "the row to be marked")
    section = ctx.settings(ctx.config_file()).get("providers.mock", {})
    assert section.get("small_model") == "beta", section
    assert section.get("model") == "alpha", "only the one key is written"

    s.key("ctrl-s").sync()
    s.wait_for(lambda t: "small" not in row_with(s, "beta"), "the mark to go")
    section = ctx.settings(ctx.config_file()).get("providers.mock", {})
    assert "small_model" not in section, section
    assert section.get("base_url") == ctx.mock.base_url, section
