"""Viewport scrolling: wheel, PageUp/PageDown and the scrollbar."""


def fill_transcript(ctx, s, words=400):
    ctx.scenario(f"words={words},paragraphs=4,chunk=16")
    s.submit("write a lot")
    s.wait_turn_done()
    return s



def test_wheel_scrolls_back_and_forth(ctx):
    """The wheel moves the viewport and returns it to the bottom."""
    s = ctx.spawn()
    fill_transcript(ctx, s)
    bottom = s.text()

    s.mouse("wheel-up", 5, 10).sync()
    scrolled = s.text()
    assert scrolled != bottom, "the wheel should have moved the viewport"

    s.mouse("wheel-down", 5, 10).sync()
    assert s.text() == bottom, "scrolling back down restores the pinned view"


def test_page_up_and_down(ctx):
    """PageUp walks back a screenful; PageDown returns."""
    s = ctx.spawn()
    fill_transcript(ctx, s)
    bottom = s.text()

    s.key("pageup").sync()
    up = s.text()
    assert up != bottom, "PageUp should move the viewport"

    s.key("pagedown").sync()
    assert s.text() == bottom, "PageDown returns exactly one page"


def test_scroll_is_clamped_at_the_top(ctx):
    """Scrolling past the start of the transcript stops at the first row."""
    s = ctx.spawn()
    fill_transcript(ctx, s)
    for _ in range(40):
        s.key("pageup")
    s.sync()
    top = s.text()
    assert "write a lot" in top
    s.key("pageup").sync()
    assert s.text() == top, "already at the top: nothing more to scroll"


def test_scrollbar_thumb_tracks_position(ctx):
    """The thumb sits at the bottom when pinned and moves up when scrolled."""
    s = ctx.spawn()
    fill_transcript(ctx, s)
    bar = s.scrollbar()
    assert bar[-1] == "\u2503", f"thumb should reach the bottom: {bar}"
    assert bar[0] == "\u2502", f"track expected above the thumb: {bar}"

    for _ in range(30):
        s.key("pageup")
    s.sync()
    bar = s.scrollbar()
    assert bar[0] == "\u2503", f"thumb should reach the top: {bar}"
    assert bar[-1] == "\u2502", f"track expected below the thumb: {bar}"


def test_no_scrollbar_when_everything_fits(ctx):
    """A short transcript gets a blank bar column, not a full-height thumb."""
    ctx.scenario("text=short")
    s = ctx.spawn()
    s.submit("hi")
    s.wait_turn_done()
    assert set(s.scrollbar()) == {" "}, s.scrollbar()


def test_new_output_returns_to_the_bottom(ctx):
    """Streaming while scrolled back snaps the viewport to the newest text."""
    s = ctx.spawn()
    fill_transcript(ctx, s)
    for _ in range(10):
        s.key("pageup")
    s.sync()
    assert "write a lot" in s.text()

    ctx.scenario("text=fresh+output+marker")
    s.submit("more please")
    s.wait_text("fresh output marker")
    s.wait_turn_done()
    assert "fresh output marker" in s.text()


def test_scrolled_view_survives_a_resize(ctx):
    """A resize re-wraps the transcript and the viewport stays usable."""
    s = ctx.spawn()
    ctx.scenario("words=400,paragraphs=4,chunk=16,final_text=x")
    s.submit("write a lot")
    s.wait_turn_done()
    tail = s.text().split()[-1]

    s.key("pageup").sync()
    s.resize(60, 20)
    s.wait_for(lambda t: t.rows == 20, "resize")
    s.settle()
    assert s.screen.cols == 60
    assert any(line.strip() for line in s.screen.lines()[:15]), "transcript kept"

    # scrolling back to the bottom still lands on the newest text
    for _ in range(30):
        s.key("pagedown")
    s.sync()
    assert tail in s.text(), f"expected {tail!r} at the bottom\n{s.text()}"


def test_user_boxes_survive_deep_scrollback(ctx):
    """Scrolled far back, an old user turn is still painted as its own block.

    The painter starts from an index checkpoint rather than from byte zero, so
    a row's offset has to stay in transcript coordinates the whole way down,
    otherwise the boxed rows drift away from the text they belong to.
    """
    s = ctx.spawn()
    for i in range(4):
        ctx.scenario(f"words=120,paragraphs=2,chunk=16,seed={i}")
        s.submit(f"question number {i}")
        s.wait_turn_done()
    bottom = s.text()

    # walk back until the first turn is on screen again
    for _ in range(40):
        s.key("pageup")
    s.sync()
    text = s.text()
    assert "question number 0" in text, text

    row = s.screen.find_row("question number 0")
    assert row >= 0, text
    assert s.screen.attr_at(row, 2).bg == 238, "the user turn kept its panel"
    assert s.screen.attr_at(row - 1, 2).bg == 238, "padding row above"

    # and walking forward again lands back on exactly the pinned view
    for _ in range(40):
        s.key("pagedown")
    s.sync()
    assert s.text() == bottom, "returning to the bottom should restore the view"
