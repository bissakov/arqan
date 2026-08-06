"""Slash-command completion popup."""


def test_popup_opens_on_slash(ctx):
    """Typing '/' offers every registered command."""
    s = ctx.spawn()
    s.type("/").sync()
    text = s.text()
    assert "/new" in text and "/exit" in text, text
    assert "Start a fresh conversation" in text, text
    assert "Quit ah" in text, text
    ctx.check_screen(s)


def test_popup_filters(ctx):
    """Each character narrows the list."""
    s = ctx.spawn()
    s.type("/e").sync()
    text = s.text()
    assert "/exit" in text, text
    assert "/new" not in text, text
    ctx.check_screen(s)


def test_popup_is_case_insensitive(ctx):
    """'/NE' still offers '/new'."""
    s = ctx.spawn()
    s.type("/NE").sync()
    assert "/new" in s.text(), s.text()


def test_popup_closes_on_no_match(ctx):
    """A prefix nothing matches shows no popup."""
    s = ctx.spawn()
    s.type("/zzz").sync()
    assert "/new" not in s.text() and "/exit" not in s.text(), s.text()


def test_popup_closes_on_space(ctx):
    """A command name ends at the first space, so the popup goes away."""
    s = ctx.spawn()
    s.type("/new ").sync()
    assert "Start a fresh conversation" not in s.text(), s.text()


def test_tab_accepts_selection(ctx):
    """Tab completes the highlighted entry and dismisses the popup."""
    s = ctx.spawn()
    s.type("/e").sync()
    s.key("tab").sync()
    assert s.composer_text() == "/exit", s.composer_lines()
    assert "Quit ah" not in s.text(), "popup should be gone after accepting"


def test_enter_completes_instead_of_submitting(ctx):
    """With a partial name, Enter completes rather than submits."""
    s = ctx.spawn()
    s.type("/n").sync()
    s.key("enter").sync()
    assert s.composer_text() == "/new", s.composer_lines()
    assert ctx.mock.requests == [], "Enter must not have started a turn"


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
    s.type("/new").sync()
    assert "Start a fresh conversation" in s.text(), "popup still visible"
    s.key("enter")
    s.wait_for(lambda t: s.composer_text() == "", "the command to run")


def test_tab_on_exact_match_just_closes_the_popup(ctx):
    """Tab with nothing left to complete dismisses instead of doing nothing."""
    s = ctx.spawn()
    s.type("/exit").sync()
    s.key("tab").sync()
    assert s.composer_text() == "/exit", s.composer_lines()
    assert "Quit ah" not in s.text(), "popup should be closed"
    assert s.proc.poll() is None, "Tab must not submit"


def test_arrows_move_the_selection(ctx):
    """Down moves the highlight; Tab then accepts the second entry."""
    s = ctx.spawn()
    s.type("/").sync()
    s.key("down").sync()
    ctx.check_screen(s, "selected_second")
    s.key("tab").sync()
    assert s.composer_text() == "/exit", s.composer_lines()


def test_ctrl_n_p_move_the_selection(ctx):
    """Ctrl-N / Ctrl-P cycle the popup the same way as the arrows."""
    s = ctx.spawn()
    s.type("/").sync()
    s.key("ctrl-n", "ctrl-n").sync()      # wraps back to the first entry
    s.key("tab").sync()
    assert s.composer_text() == "/new", s.composer_lines()


def test_esc_dismisses_until_text_changes(ctx):
    """Esc closes the popup; editing the text brings it back."""
    s = ctx.spawn()
    s.type("/n").sync()
    assert "/new" in s.text()
    s.key("esc").sync()
    assert "Start a fresh conversation" not in s.text(), s.text()
    s.type("e").sync()
    assert "/new" in s.text(), "editing reopens the popup"


def test_popup_eats_into_transcript_not_composer(ctx):
    """The popup grows upward: the composer keeps its row."""
    s = ctx.spawn()
    s.type("/").sync()
    assert s.composer_text() == "/", s.composer_lines()
    # the two popup entries sit immediately above the composer padding row
    rows = s.screen.lines()
    assert "/exit" in rows[s.screen.rows - 6], rows[s.screen.rows - 8 :]
    assert "/new" in rows[s.screen.rows - 7], rows[s.screen.rows - 8 :]
