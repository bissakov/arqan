"""Transcript search: the box, the count, the walk and the highlight.

The terminal's own find only sees the frame on screen, so the UI owns this
one. What it searches is the transcript, which is exactly what scrolling
reaches: a match is always a line the reader can also scroll to.
"""

import re


def find_row(s) -> str:
    """The search box, wherever the overlay stack put it."""
    for i in range(s.term.rows):
        row = s.row(i)
        if "find:" in row:
            return row.strip()
    return ""


def a_transcript(ctx, s, replies=("alpha needle one", "beta two", "gamma needle three")):
    """One turn per reply, so the transcript holds known, ordered text."""
    for i, reply in enumerate(replies):
        ctx.scenario("text=" + reply.replace(" ", "+"))
        s.submit(f"question {i}")
        s.wait_turn_done()
    return s


def open_find(s, query: str = ""):
    s.key("ctrl-r").sync()
    if query:
        s.type(query)
    return s.sync()


def test_ctrl_r_opens_the_box_and_counts_matches(ctx):
    """The box says what it found before the reader has to look for it."""
    s = ctx.spawn()
    a_transcript(ctx, s)
    open_find(s, "needle")
    assert "find: needle" in find_row(s), find_row(s)
    assert "2 of 2" in find_row(s), find_row(s)


def test_the_query_is_matched_case_insensitively(ctx):
    """A reader searching for what they read does not also spell its case."""
    s = ctx.spawn()
    a_transcript(ctx, s, ("alpha NEEDLE one", "beta two", "gamma Needle three"))
    open_find(s, "needle")
    assert "find: needle" in find_row(s), "the box shows the query as typed"
    assert "2 of 2" in find_row(s), "either case of either byte matches"

    s.key("ctrl-u").sync()
    s.type("NEEDLE").sync()
    assert "2 of 2" in find_row(s), find_row(s)


def test_a_query_with_no_match_says_so(ctx):
    """An empty result is an answer, not an empty box."""
    s = ctx.spawn()
    a_transcript(ctx, s)
    open_find(s, "haystack")
    assert "no match" in find_row(s), find_row(s)


def test_enter_walks_older_and_wraps(ctx):
    """Enter steps back through the conversation and comes round again."""
    s = ctx.spawn()
    a_transcript(ctx, s)
    open_find(s, "needle")
    assert "2 of 2" in find_row(s), "a search starts on the newest match"

    s.key("enter").sync()
    assert "1 of 2" in find_row(s), find_row(s)

    s.key("enter").sync()
    assert "2 of 2" in find_row(s), find_row(s)
    assert "wrapped" in find_row(s), "running off the top says so"

    s.key("down").sync()
    assert "1 of 2" in find_row(s), "Down walks the other way"


def test_a_match_off_screen_brings_the_viewport_to_it(ctx):
    """The point of a search is the line it puts on screen."""
    ctx.scenario("words=400,paragraphs=4,chunk=16")
    s = ctx.spawn()
    s.submit("write a lot")
    s.wait_turn_done()
    ctx.scenario("text=the+quotient+of+it")
    s.submit("and one more")
    s.wait_turn_done()

    assert "question zero" not in s.text()
    ctx.scenario("text=short")
    s.submit("question zero")
    s.wait_turn_done()

    open_find(s, "write a lot")
    assert "write a lot" in s.text(), "the first message should be back on screen"


def test_a_match_already_on_screen_leaves_the_viewport_alone(ctx):
    """Typing another letter narrows a search; it does not shuffle the page."""
    s = ctx.spawn()
    a_transcript(ctx, s)
    open_find(s, "gamm")
    before = [s.row(i) for i in range(s.transcript_height())]
    s.type("a").sync()
    assert [s.row(i) for i in range(s.transcript_height())] == before, s.text()
    assert "1 of 1" in find_row(s), find_row(s)


def test_the_current_match_is_highlighted(ctx):
    """A found line still has to be found on the line."""
    s = ctx.spawn()
    a_transcript(ctx, s)
    open_find(s, "needle")
    hit = None
    for r in range(s.term.rows):
        row = s.row(r)
        if "find:" in row:
            continue   # the box shows the query back; that is not a match
        c = row.find("needle")
        if c >= 0:
            hit = (r, c)
    assert hit, "a match should be on screen"
    r, c = hit
    assert s.term.attr_at(r, c).bg is not None, "the current match takes a background"
    assert s.term.attr_at(r, c - 1).bg != s.term.attr_at(r, c).bg, (
        "the highlight covers the match, not the row"
    )


def match_cell(s, line: str, word: str):
    """Screen row and column of `word` on the transcript line holding `line`."""
    for r in range(s.term.rows):
        row = s.row(r)
        if "find:" in row:
            continue
        if line in row:
            return r, row.index(word)
    raise AssertionError(f"{line!r} not on screen\n{s.text()}")


def test_closing_the_box_drops_the_highlight(ctx):
    """The highlight belongs to the box: no box, no paint on the transcript."""
    s = ctx.spawn()
    a_transcript(ctx, s)
    open_find(s, "needle")
    r, c = match_cell(s, "gamma needle three", "needle")
    assert s.term.attr_at(r, c).bg is not None, "an open box paints its matches"

    s.key("esc").sync()
    r, c = match_cell(s, "gamma needle three", "needle")
    assert s.term.attr_at(r, c).bg is None, "Escape leaves the transcript plain"

    s.key("ctrl-r").sync()
    r, c = match_cell(s, "gamma needle three", "needle")
    assert s.term.attr_at(r, c).bg is not None, "the reopened box paints again"


def test_the_box_does_not_hold_the_viewport(ctx):
    """The box owns the keys, not the page: the reader can still scroll away.

    A search puts a match on screen once. After that the reader may want the
    lines around it, so scrolling has to move the view and stay moved.
    """
    ctx.scenario("words=400,paragraphs=4,chunk=16")
    s = ctx.spawn()
    s.submit("write a lot")
    s.wait_turn_done()
    ctx.scenario("text=the+last+needle")
    s.submit("and one more")
    s.wait_turn_done()

    open_find(s, "needle")
    assert "1 of 1" in find_row(s), find_row(s)
    at_match = s.row(0)

    s.key("pageup").sync()
    scrolled = s.row(0)
    assert scrolled != at_match, "Page Up moves the view off the match"

    s.sync()
    assert s.row(0) == scrolled, "a repaint does not drag it back to the match"

    s.key("wheel-up").sync()
    assert s.row(0) != scrolled, "the wheel keeps going"
    assert "find: needle" in find_row(s), "the box stays open and keeps its query"


def test_escape_closes_the_box_and_keeps_the_draft(ctx):
    """The composer is covered by the box, never typed into by it."""
    s = ctx.spawn()
    a_transcript(ctx, s)
    s.type("a draft").sync()
    open_find(s, "needle")
    assert s.composer_text() == "a draft", s.composer_text()

    s.key("esc").sync()
    assert find_row(s) == "", "Escape closes the box"
    assert s.composer_text() == "a draft", s.composer_text()

    s.key("ctrl-r").sync()
    assert "find: needle" in find_row(s), "a reopened box keeps its query"


def test_backspace_and_ctrl_u_edit_the_query(ctx):
    """The box is an editor, so what it holds can be taken back."""
    s = ctx.spawn()
    a_transcript(ctx, s)
    open_find(s, "needlez")
    assert "no match" in find_row(s), find_row(s)

    s.key("backspace").sync()
    assert "2 of 2" in find_row(s), find_row(s)

    s.key("ctrl-u").sync()
    assert "type to search" in find_row(s), find_row(s)


def test_a_streaming_reply_keeps_the_search_where_it_is(ctx):
    """Reading back through the conversation outranks the newest row.

    New output pins the viewport to the bottom, which would drag the reader
    off the match they went looking for, and the count has to grow with the
    bytes the turn appends: the scan carries forward rather than starting
    again, so an append that was never scanned would never be counted.
    """
    s = ctx.spawn()
    a_transcript(ctx, s)
    ctx.scenario("text=a+third+needle+arrives,words=300,paragraphs=3,chunk=4,delay=0.02")
    s.submit("more")
    s.wait_for(lambda t: s.status_kind() != "ready", "the turn to start")
    open_find(s, "needle")
    s.key("enter").sync()          # onto an older match, off the newest rows
    scrolled = s.row(0)
    index = find_row(s)

    s.wait_turn_done()
    assert s.row(0) == scrolled, "the viewport followed the stream"
    assert "of 3" in find_row(s), find_row(s)
    place = re.search(r"(\d+) of", find_row(s)).group(1)
    assert place == re.search(r"(\d+) of", index).group(1), (
        "an appended match sits after the current one, which keeps its place"
    )


def test_the_box_does_not_hide_the_match_it_found(ctx):
    """An overlay covers the last transcript row; a search may not lose it.

    Every other overlay is drawn over the bottom of the transcript, which is
    where the newest match usually is. A box answering "1 of 1" over the row
    it matched would be answering nothing.
    """
    s = ctx.spawn()
    a_transcript(ctx, s, tuple(f"reply {i}" for i in range(6)) + ("the marker",))
    assert "the marker" in s.text(), "the newest reply starts on screen"
    open_find(s, "marker")
    assert "1 of 1" in find_row(s), find_row(s)
    assert "the marker" in s.text(), "the match is under the box"


def test_slash_find_opens_the_same_box(ctx):
    """A command for the reader who does not know the key yet."""
    s = ctx.spawn()
    a_transcript(ctx, s)
    s.submit("/find")
    s.wait_for(lambda t: "find:" in find_row(s), "the search box")


def test_a_match_inside_a_tool_block_is_highlighted(ctx):
    """A tool header is a click target, and still a line with words in it."""
    ctx.write_file("notes.txt", "hello from disk\n")
    ctx.scenario('tool=read:{"path":"notes.txt"},final_text=done')
    s = ctx.spawn()
    s.submit("what is in notes.txt?")
    s.wait_text("done")
    s.wait_turn_done()

    open_find(s, "notes")
    row = col = None
    for r in range(s.term.rows):
        line = s.row(r)
        if "find:" in line:
            continue
        c = line.find("notes.txt")
        if c >= 0 and "\u25c6" in line:
            row, col = r, c
            break
    assert row is not None, s.text()
    assert s.term.attr_at(row, col).bg is not None, "the header's match is painted"
    assert s.term.attr_at(row, col + 5).bg is None, (
        "the highlight ends with the query, not with the row"
    )


def test_the_query_survives_a_rerendered_transcript(ctx):
    """A replay rebuilds every offset the count was taken over."""
    s = ctx.spawn()
    a_transcript(ctx, s)
    open_find(s, "needle")
    assert "2 of 2" in find_row(s), find_row(s)

    s.key("esc").sync()
    s.settings_toggle("Verbose tool output")   # a re-render of the whole conversation
    s.key("ctrl-r").sync()
    assert "2 of 2" in find_row(s), find_row(s)


def test_ctrl_e_reaches_output_the_transcript_capped(ctx):
    """A tool's output is rendered under a cap, and a cap hides matches.

    The box says so and offers the key that lifts it, so every line the
    conversation can show is a line the search can reach.
    """
    body = "".join(f"line {i} of the file\n" for i in range(40))
    body += "sunken treasure\n"
    ctx.write_file("long.txt", body)
    ctx.scenario('tool=read:{"path":"long.txt"},final_text=read+it')
    s = ctx.spawn()
    s.submit("read long.txt")
    s.wait_text("read it")
    s.wait_turn_done()
    assert "sunken treasure" not in s.text(), "the render should have capped it"

    open_find(s, "sunken")
    assert "no match" in find_row(s), "capped output is not in the transcript"
    assert "^E" in find_row(s), find_row(s)

    s.key("ctrl-e").sync()
    s.wait_for(lambda t: "1 of 1" in find_row(s), "the match the cap hid")
    assert "sunken treasure" in s.text(), s.text()
    assert "^E" not in find_row(s), "nothing left to reveal"


def transcript_row(s, needle: str) -> int:
    """1-based screen row of `needle`, ignoring the search box's echo."""
    for r in range(s.term.rows):
        row = s.row(r)
        if "find:" in row:
            continue
        if needle in row:
            return r + 1
    raise AssertionError(f"{needle!r} not on screen\n{s.text()}")


def test_the_pointer_still_selects_while_the_box_is_open(ctx):
    """The box holds the keyboard, not the pointer: a drag still copies."""
    s = ctx.spawn()
    a_transcript(ctx, s)
    open_find(s, "needle")

    row = transcript_row(s, "alpha needle one")
    col = s.row(row - 1).index("alpha") + 1
    s.mouse("down", row, col)
    s.mouse("drag", row, col + len("alpha") - 1)
    s.mouse("up", row, col + len("alpha") - 1).sync()

    assert s.term.clipboard == "alpha", repr(s.term.clipboard)
    assert "find: needle" in find_row(s), "the query survives a selection"


def test_a_selection_survives_pointer_motion_but_not_a_keystroke(ctx):
    """Hovering leaves the range alone; typing drops it, as in the composer."""
    s = ctx.spawn()
    a_transcript(ctx, s)
    open_find(s, "needle")

    row = transcript_row(s, "alpha needle one")
    col = s.row(row - 1).index("alpha") + 1
    s.mouse("down", row, col)
    s.mouse("drag", row, col + len("alpha") - 1).sync()
    assert s.term.attr_at(row - 1, col - 1).reverse

    s.mouse("move", row, col + 12).sync()
    assert s.term.attr_at(row - 1, col - 1).reverse, "hovering keeps the range"

    s.type("x").sync()
    assert not s.term.attr_at(row - 1, col - 1).reverse, "a keystroke drops it"
