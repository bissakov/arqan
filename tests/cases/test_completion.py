"""Slash-command completion popup."""


def test_popup_opens_on_slash(ctx):
    """Typing '/' offers the commands, as many as the popup holds."""
    s = ctx.spawn()
    s.type("/").sync()
    text = s.text()
    assert "/clear" in text and "/model" in text and "/provider" in text, text
    assert "/copy" in text, text
    assert "Start a fresh conversation" in text, text
    assert "Toggle raw Markdown" in text, text
    # More commands than rows: the rest are reached by moving the selection.
    assert "/exit" not in text, text
    ctx.check_screen(s)


def test_popup_filters(ctx):
    """Each character narrows the list."""
    s = ctx.spawn()
    s.type("/e").sync()
    text = s.text()
    assert "/exit" in text, text
    assert "/clear" not in text, text
    ctx.check_screen(s)


def test_popup_is_case_insensitive(ctx):
    """'/CL' still offers '/clear'."""
    s = ctx.spawn()
    s.type("/CL").sync()
    assert "/clear" in s.text(), s.text()


def test_popup_closes_on_no_match(ctx):
    """A prefix nothing matches shows no popup."""
    s = ctx.spawn()
    s.type("/zzz").sync()
    assert "/clear" not in s.text() and "/exit" not in s.text(), s.text()


def test_popup_closes_on_space(ctx):
    """A command name ends at the first space, so the popup goes away."""
    s = ctx.spawn()
    s.type("/clear ").sync()
    assert "Start a fresh conversation" not in s.text(), s.text()


def test_tab_accepts_selection(ctx):
    """Tab completes the highlighted entry and dismisses the popup."""
    s = ctx.spawn()
    s.type("/e").sync()
    s.key("tab").sync()
    assert s.composer_text() == "/exit", s.composer_lines()
    assert "Quit yoke" not in s.text(), "popup should be gone after accepting"


def test_enter_accepts_and_submits(ctx):
    """With a partial name, Enter completes the entry and runs it at once."""
    s = ctx.spawn()
    s.type("/ex").sync()
    s.key("enter")
    assert s.wait_exit() == 0, "Enter should have completed and run '/exit'"


def test_enter_submits_the_highlighted_entry(ctx):
    """Enter runs whatever the popup highlights, not what was typed."""
    s = ctx.spawn()
    s.type("/").sync()
    s.key(*(["down"] * 8)).sync()         # highlight '/exit'
    s.key("enter")
    assert s.wait_exit() == 0, "Enter should have run the highlighted entry"


def test_enter_submits_a_fully_typed_command(ctx):
    """A name typed out in full has nothing to complete, so Enter runs it."""
    ctx.scenario("text=answered")
    s = ctx.spawn()
    s.submit("/exit")                     # one Enter, not two
    assert s.wait_exit() == 0


def test_enter_on_exact_match_sends_a_message(ctx):
    """The same rule applies to a command that is not handled locally."""
    ctx.scenario("text=answered")
    s = ctx.spawn()
    s.type("/clear").sync()
    assert "Start a fresh conversation" in s.text(), "popup still visible"
    s.key("enter")
    s.wait_for(lambda t: s.composer_text() == "", "the command to run")


def test_tab_on_exact_match_just_closes_the_popup(ctx):
    """Tab with nothing left to complete dismisses instead of doing nothing."""
    s = ctx.spawn()
    s.type("/exit").sync()
    s.key("tab").sync()
    assert s.composer_text() == "/exit", s.composer_lines()
    assert "Quit yoke" not in s.text(), "popup should be closed"
    assert s.proc.poll() is None, "Tab must not submit"


def test_arrows_move_the_selection(ctx):
    """Down moves the highlight; Tab then accepts the second entry."""
    s = ctx.spawn()
    s.type("/").sync()
    s.key("down").sync()
    ctx.check_screen(s, "selected_second")
    s.key("tab").sync()
    assert s.composer_text() == "/resume", s.composer_lines()


def test_ctrl_n_p_move_the_selection(ctx):
    """Ctrl-N / Ctrl-P cycle the popup the same way as the arrows."""
    s = ctx.spawn()
    s.type("/").sync()
    s.key(*(["ctrl-n"] * 9)).sync()   # wraps to the first
    s.key("tab").sync()
    assert s.composer_text() == "/clear", s.composer_lines()


def test_esc_dismisses_until_text_changes(ctx):
    """Esc closes the popup; editing the text brings it back."""
    s = ctx.spawn()
    s.type("/c").sync()
    assert "/clear" in s.text()
    s.key("esc").sync()
    assert "Start a fresh conversation" not in s.text(), s.text()
    s.type("l").sync()
    assert "/clear" in s.text(), "editing reopens the popup"


def test_popup_leaves_the_welcome_screen_in_place(ctx):
    """The art is centred on the body, so overlays do not push it upward.

    The rows have to be there for that to hold: on a screen too short for the
    block plus a row of air the art does move up, so this asks for room.
    """
    s = ctx.spawn(rows=32)
    before = [i for i, row in enumerate(s.screen.lines()) if "|___/" in row]
    s.type("/").sync()
    assert "/clear" in s.text(), s.text()
    after = [i for i, row in enumerate(s.screen.lines()) if "|___/" in row]
    assert after == before, (before, after)


def test_popup_eats_into_transcript_not_composer(ctx):
    """The popup grows upward: the composer keeps its row."""
    s = ctx.spawn()
    s.type("/").sync()
    assert s.composer_text() == "/", s.composer_lines()
    # the popup entries sit immediately above the composer padding row
    rows = s.screen.lines()
    # the popup holds eight entries, so the ninth ('/exit') waits below
    assert "/raw" in rows[s.screen.rows - 6], rows[s.screen.rows - 13 :]
    assert "/verbose" in rows[s.screen.rows - 7], rows[s.screen.rows - 13 :]
    assert "/copy" in rows[s.screen.rows - 8], rows[s.screen.rows - 13 :]
    assert "/rewind" in rows[s.screen.rows - 9], rows[s.screen.rows - 14 :]
    assert "/provider" in rows[s.screen.rows - 10], rows[s.screen.rows - 14 :]
    assert "/model" in rows[s.screen.rows - 11], rows[s.screen.rows - 14 :]
    assert "/resume" in rows[s.screen.rows - 12], rows[s.screen.rows - 14 :]
    assert "/clear" in rows[s.screen.rows - 13], rows[s.screen.rows - 14 :]
