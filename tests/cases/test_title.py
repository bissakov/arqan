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


def test_a_reasoning_small_model_is_left_room_to_answer(ctx):
    """A small model that thinks first must still reach its own answer.

    The mock spends the request's `max_tokens` on reasoning before content,
    the way a provider does, so a naming request capped at a line's worth of
    tokens comes back as thinking and no name at all.
    """
    thinking = "+".join(["weighing", "what", "to", "call", "this"] * 20)
    # Longer than the 64 tokens the naming request was once capped to, so the
    # case cannot stop covering the bug without failing.
    assert len(thinking) > 64 * 4, len(thinking)

    ctx.scenario(f"text=Cat+memory+notes,reasoning={thinking}")
    s = ctx.spawn(ARQAN_SMALL_MODEL="mock-small")
    s.submit("remember the cat")
    s.wait_turn_done()
    s.wait_text("session named: Cat memory notes")

    naming = ctx.mock.requests[-1]
    assert naming["model"] == "mock-small", naming["model"]
    assert session_files(ctx, ".title")[0].read_text() == "Cat memory notes"


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
def test_ctrl_s_sets_and_clears_the_small_model(ctx):
    """The choice is one pair in the state file: the model and its provider."""
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
    assert ctx.state().get("small_model") == "beta", ctx.state()
    assert ctx.state().get("small_provider") == "mock", ctx.state()
    section = ctx.settings(ctx.config_file()).get("providers.mock", {})
    assert "small_model" not in section, "the user's own file is left alone"
    assert section.get("model") == "alpha", section

    s.key("ctrl-s").sync()
    s.wait_for(lambda t: "small" not in row_with(s, "beta"), "the mark to go")
    assert "small_model" not in ctx.state(), ctx.state()
    assert "small_provider" not in ctx.state(), ctx.state()


def test_a_provider_section_still_names_a_small_model(ctx):
    """The per-provider key stays readable: it is the fallback under the pair."""
    ctx.scenario("text=noted")
    ctx.write_config(
        f'provider = "mock"\n\n[providers.mock]\n'
        f'base_url = "{ctx.mock.base_url}"\napi = "openai"\n'
        f'model = "alpha"\nsmall_model = "mock-small"\n'
    )
    s = ctx.spawn(ARQAN_MODEL=None, ARQAN_BASE_URL=None)
    s.submit("remember the cat")
    s.wait_text("noted")
    s.wait_turn_done()
    s.wait_text("session named: noted")
    assert ctx.mock.requests[-1]["model"] == "mock-small", ctx.mock.requests[-1]


def test_the_state_pair_outranks_the_active_providers_own_key(ctx):
    """A model chosen in /model is the one errands use, wherever it is served."""
    ctx.scenario("text=noted")
    ctx.write_config(
        f'[providers.mock]\nbase_url = "{ctx.mock.base_url}"\n'
        f'api = "openai"\nmodel = "alpha"\nsmall_model = "section-small"\n'
    )
    p = ctx.state_file()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("provider = mock\nsmall_model = chosen-small\n"
                 "small_provider = mock\n")
    s = ctx.spawn(ARQAN_MODEL=None, ARQAN_BASE_URL=None)
    s.submit("remember the cat")
    s.wait_text("noted")
    s.wait_turn_done()
    s.wait_text("session named: noted")
    assert ctx.mock.requests[-1]["model"] == "chosen-small", \
        ctx.mock.requests[-1]


# ---- a small model at another provider ------------------------------------
def stored_providers(ctx, sections, state, **env):
    """Endpoints with a key each, all served by the mock.

    The URL is shared, so what a request proves about its endpoint is the key
    it carried and the model it named.
    """
    ctx.write_config("".join(
        f'[providers.{name}]\nbase_url = "{ctx.mock.base_url}"\n'
        f'api = "openai"\nmodel = "{model}"\n'
        + (f'small_model = "{small}"\n' if small else "") + "\n"
        for name, model, small in sections))
    creds = ctx.home / ".local" / "state" / "arqan" / "credentials.toml"
    creds.parent.mkdir(parents=True, exist_ok=True)
    creds.write_text("".join(f"[providers.{name}]\nkey = sk-{name}\n\n"
                             for name, _, _ in sections))
    creds.chmod(0o600)
    p = ctx.state_file()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(state)
    return ctx.spawn(ARQAN_MODEL=None, ARQAN_BASE_URL=None, ARQAN_API_KEY=None,
                     **env)


def test_the_errand_follows_the_small_models_provider(ctx):
    """The naming request takes that endpoint's key; the turn keeps its own."""
    ctx.scenario("text=noted")
    s = stored_providers(
        ctx, [("mock", "alpha", None), ("spare", "gamma", None)],
        "provider = mock\nsmall_model = tiny\nsmall_provider = spare\n")
    s.submit("remember the cat")
    s.wait_text("noted")
    s.wait_turn_done()
    s.wait_text("session named: noted")

    turn, naming = ctx.mock.requests[0], ctx.mock.requests[-1]
    assert turn["model"] == "alpha" and naming["model"] == "tiny", \
        (turn["model"], naming["model"])
    assert ctx.mock.auth[0] == "Bearer sk-mock", ctx.mock.auth
    assert ctx.mock.auth[-1] == "Bearer sk-spare", ctx.mock.auth
    assert s.status_field(3) == "mock", "the session stays on its provider"


def test_moving_the_conversation_keeps_the_chosen_small_model(ctx):
    """The pair names its own endpoint, so another provider's key is not it."""
    ctx.scenario("text=noted")
    s = stored_providers(
        ctx, [("mock", "alpha", None), ("spare", "gamma", None),
              ("other", "delta", "others-small")],
        "provider = mock\nsmall_model = tiny\nsmall_provider = spare\n")
    s.submit("/model")
    s.wait_status("pick a model")
    s.key("down", "down").sync()
    assert "@ other" in s.popup_selected(), s.popup_selected()
    s.key("enter")
    s.wait_text("@ other")

    s.submit("remember the cat")
    s.wait_text("noted")
    s.wait_turn_done()
    s.wait_text("session named: noted")
    naming = ctx.mock.requests[-1]
    assert naming["model"] == "tiny", naming["model"]
    assert ctx.mock.auth[-1] == "Bearer sk-spare", ctx.mock.auth


def test_a_small_provider_that_is_gone_is_reported_on_demand(ctx):
    """No endpoint, no errand: the excerpt is not sent somewhere else."""
    ctx.scenario("text=noted")
    s = stored_providers(
        ctx, [("mock", "alpha", None)],
        "provider = mock\nsmall_model = tiny\nsmall_provider = spare\n")
    s.submit("remember the cat")
    s.wait_text("noted")
    s.wait_turn_done()
    s.submit("/title auto")
    s.wait_text("provider spare is not usable")
    assert len(ctx.mock.requests) == 1, [r["model"] for r in ctx.mock.requests]
    assert session_files(ctx, ".title") == [], session_files(ctx, ".title")
