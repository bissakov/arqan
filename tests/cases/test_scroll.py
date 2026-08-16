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


def test_home_and_end_reach_both_ends_of_the_transcript(ctx):
    """On an empty composer Home is the first row and End the last."""
    s = ctx.spawn()
    fill_transcript(ctx, s)
    bottom = s.text()

    s.key("home").sync()
    top = s.text()
    assert top != bottom, "Home should move the viewport"
    assert "write a lot" in top, top

    for _ in range(40):
        s.key("pageup")
    s.sync()
    assert s.text() == top, "Home already went as far as PageUp can"

    s.key("end").sync()
    assert s.text() == bottom, "End returns to the newest row"


def test_home_and_end_edit_the_line_while_a_draft_is_open(ctx):
    """With text in the box they are the line's ends, not the transcript's."""
    s = ctx.spawn()
    fill_transcript(ctx, s)
    s.type("abc").sync()
    bottom = s.text()          # with the draft on screen, since it is drawn

    s.key("home").sync()
    assert s.text() == bottom, "the viewport must not move with a draft open"
    s.type("X").sync()
    assert s.composer_text() == "Xabc", s.composer_lines()

    s.key("end").sync()
    s.type("Y").sync()
    assert s.composer_text() == "XabcY", s.composer_lines()


def test_ctrl_home_and_ctrl_end_scroll_with_a_draft_open(ctx):
    """The unshadowed pair reaches the ends whatever the composer holds."""
    s = ctx.spawn()
    fill_transcript(ctx, s)
    s.type("a draft").sync()
    bottom = s.text()

    s.key("ctrl-home").sync()
    top = s.text()
    assert top != bottom, "Ctrl-Home should move the viewport"
    assert "write a lot" in top, top
    assert s.composer_text() == "a draft", s.composer_lines()

    s.key("ctrl-end").sync()
    assert s.text() == bottom, "Ctrl-End returns to the newest row"
    assert s.composer_text() == "a draft", s.composer_lines()


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


def test_the_thumb_reaches_the_bottom_while_streaming(ctx):
    """A running turn keeps the newest row visible, not under the spinner.

    The spinner is stacked below the transcript rather than drawn over it, so
    counting its rows as part of the scroll window pins the view one block too
    high and leaves the newest line off screen until the turn ends.
    """
    # Held part way through the reply, so the assertion looks at a turn that
    # is genuinely mid-stream without pacing the whole of it.
    ctx.scenario("words=600,paragraphs=8,chunk=4,hold_after=100")
    s = ctx.spawn()
    s.submit("write a lot")
    s.wait_for(lambda t: "\u2503" in s.scrollbar(), "a scrollable transcript")
    # The spinner takes the last two body rows: its own and the air above it.
    bar = s.scrollbar(popup_rows=2)
    assert bar[-1] == "\u2503", (
        f"thumb should reach the bottom while pinned: {bar}\n{s.text()}")
    ctx.mock.release()
    s.wait_turn_done()


def test_no_scrollbar_when_everything_fits(ctx):
    """A short transcript gets a blank bar column, not a full-height thumb."""
    ctx.scenario("text=short")
    s = ctx.spawn()
    s.submit("hi")
    s.wait_turn_done()
    assert set(s.scrollbar()) == {" "}, s.scrollbar()


def test_new_output_returns_to_the_bottom(ctx):
    """Submitting from a scrolled view returns to the newest text."""
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


def test_streaming_does_not_steal_a_scrolled_view(ctx):
    """Output arriving below the viewport leaves the reader where they are.

    Every appended run used to pin the viewport back to the newest row, so a
    streaming reply dragged the reader off the lines they were reading.
    """
    # Paced, not held: this case is about output that keeps arriving while
    # the reader is scrolled up, so the reply has to grow in steps. The
    # workload is the smallest one that overflows the viewport.
    ctx.scenario("words=400,paragraphs=1,chunk=4,delay=0.02")
    s = ctx.spawn()
    s.submit("write a lot")
    s.wait_for(lambda t: "\u2503" in s.scrollbar(popup_rows=2),
               "a scrollable transcript")
    s.key("pageup").sync()

    top = s.screen.lines()[:5]
    assert any(line.strip() for line in top), s.text()
    bar = s.scrollbar(popup_rows=2)
    # The rows on screen do not move, so growth shows up on the scrollbar.
    s.wait_for(lambda t: s.scrollbar(popup_rows=2) != bar, "more output below")
    assert s.screen.lines()[:5] == top, (
        f"the stream moved the viewport\n{top}\n---\n{s.screen.lines()[:5]}")
    s.key("ctrl-c")
    s.wait_turn_done()


def test_scrolling_back_down_resumes_following(ctx):
    """Returning to the bottom sticks there as new output arrives."""
    s = ctx.spawn()
    fill_transcript(ctx, s)
    for _ in range(5):
        s.key("pageup")
    s.sync()
    for _ in range(20):
        s.key("pagedown")
    s.sync()

    ctx.scenario("text=tail+marker")
    s.submit("more please")
    s.wait_turn_done()
    assert "tail marker" in s.text(), s.text()


def test_scrolled_view_survives_a_resize(ctx):
    """A resize re-wraps the transcript and the viewport stays usable."""
    s = ctx.spawn()
    ctx.scenario("words=400,paragraphs=4,chunk=16,final_text=x")
    s.submit("write a lot")
    s.wait_turn_done()
    tail = "x"

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


def test_a_popup_does_not_shift_the_transcript(ctx):
    """An overlay covers the last rows instead of pushing the view up."""
    s = ctx.spawn()
    fill_transcript(ctx, s)
    before = s.screen.lines()

    s.type("/").sync()
    after = s.screen.lines()
    assert "/clear" in "\n".join(after), "the popup is open"
    # every row the popup does not sit on holds exactly what it held before
    kept = s.screen.rows - 16
    assert after[:kept] == before[:kept], (before[:kept], after[:kept])

    s.key("esc").sync()
    assert s.screen.lines()[:kept] == before[:kept], "closing it uncovers it"
    assert "/clear" not in "\n".join(s.screen.lines()), "the popup is gone"


def test_a_notice_lifts_the_transcript(ctx):
    """A notice answers the last command and is read beside the transcript,
    so the row it takes is lifted rather than covered."""
    s = ctx.spawn()
    fill_transcript(ctx, s)
    before = s.screen.lines()

    s.submit("/copy")   # answers in the notice slot, writes no transcript
    s.wait_text("copied the last response")
    after = s.screen.lines()

    kept = s.screen.rows - 16
    assert after[:kept] == before[1:kept + 1], (before[:kept], after[:kept])

    s.key("esc").sync()
    assert "copied the last response" not in "\n".join(s.screen.lines())
    assert s.screen.lines()[:kept] == before[:kept], "retiring it drops back"


def test_the_viewport_scrolls_with_a_popup_open(ctx):
    """The transcript still takes wheel and PageUp while the popup is up."""
    s = ctx.spawn()
    fill_transcript(ctx, s)
    s.type("/").sync()
    pinned = s.text()

    s.mouse("wheel-up", 5, 10).sync()
    assert s.text() != pinned, "the wheel should move the viewport"
    assert "/clear" in s.text(), "and leave the popup where it is"
    s.mouse("wheel-down", 5, 10).sync()
    assert s.text() == pinned, "and back down"

    s.key("pageup").sync()
    assert s.text() != pinned, "PageUp should move it too"
    s.key("pagedown").sync()
    assert s.text() == pinned, s.text()
    assert s.composer_text() == "/", s.composer_lines()


def test_the_viewport_scrolls_with_a_picker_open(ctx):
    """A modal list is drawn over the transcript, which still scrolls."""
    s = ctx.spawn()
    fill_transcript(ctx, s)
    s.key("esc").sync()
    s.key("esc")
    s.wait_status("rewind to a message")
    pinned = s.text()

    s.key("pageup").sync()
    assert s.text() != pinned, "PageUp should move the viewport"
    assert "write a lot" in s.text(), s.text()
    assert s.status_line().count("rewind to a message"), s.status_line()
    s.key("pagedown").sync()
    assert s.text() == pinned, s.text()


def test_a_setting_that_replays_keeps_the_scrolled_view(ctx):
    """Toggling raw or verbose re-renders without dropping the reader back.

    The replay rebuilds the whole transcript, so the viewport has to be put
    back on the message it was looking at instead of snapping to the newest
    row.
    """
    s = ctx.spawn()
    for i in range(4):
        ctx.scenario(f"words=120,paragraphs=2,chunk=16,seed={i}")
        s.submit(f"question number {i}")
        s.wait_turn_done()

    for _ in range(12):
        s.key("pageup")
    s.sync()
    before = s.text()
    assert "question number 0" in before, before

    s.settings_toggle("Verbose tool output")
    s.wait_gone("Verbose tool output")
    assert s.text() == before, f"the viewport moved\n{before}\n---\n{s.text()}"

    s.settings_toggle("Display raw")
    s.wait_gone("Display raw")
    assert "question number 0" in s.text(), s.text()


def test_show_instructions_keeps_the_scrolled_view(ctx):
    """Rows appearing above the viewport move with it, not under it."""
    ctx.write_file(".arqan/SYSTEM.md", "BUILD PROMPT\nline two\nline three\n")
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None)
    for i in range(4):
        ctx.scenario(f"words=120,paragraphs=2,chunk=16,seed={i}")
        s.submit(f"question number {i}")
        s.wait_turn_done()

    for _ in range(12):
        s.key("pageup")
    s.sync()
    before = s.text()
    row = s.term.find_row("question number 0")
    assert row >= 0, before

    s.settings_toggle("Show instructions")
    s.wait_gone("Show instructions")
    text = s.text()
    assert "question number 0" in text, text
    assert abs(s.term.find_row("question number 0") - row) <= 1, (
        f"expected row {row}\n{before}\n---\n{text}")


def test_a_setting_applied_at_the_bottom_stays_at_the_bottom(ctx):
    """A viewport nobody scrolled keeps following the newest output."""
    s = ctx.spawn()
    fill_transcript(ctx, s)
    bottom = s.text()
    s.settings_toggle("Verbose tool output")
    s.wait_gone("Verbose tool output")
    assert s.text() == bottom, f"{bottom}\n---\n{s.text()}"
