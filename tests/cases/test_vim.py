"""'/vim': modal keys over the whole screen, insert confined to the composer.

The notice row names the mode, so every case can read it off the screen the
way a user does.
"""

NORMAL = "-- NORMAL --"
INSERT = "-- INSERT --"
VISUAL = "-- VISUAL --"


def enable(s):
    """Turn the layer on and land in normal mode."""
    s.submit("/vim")
    s.wait_text(NORMAL)
    return s


def fill_transcript(ctx, s, words=400):
    ctx.scenario(f"words={words},paragraphs=4,chunk=16")
    s.submit("write a lot")
    s.wait_turn_done()
    return s


def test_vim_toggles_and_names_the_mode(ctx):
    """'/vim' announces itself in the notice row and toggles back off."""
    s = ctx.spawn()
    assert NORMAL not in s.text()
    enable(s)
    assert ctx.mock.requests == [], "a slash command is not a turn"
    ctx.check_screen(s)

    s.key(":").sync()                          # normal mode opens a command
    assert s.composer_text() == ":", s.composer_lines()
    s.submit("vim")
    s.wait_for(lambda _: NORMAL not in s.text() and INSERT not in s.text(),
               "the mode row to go with the layer")


def test_normal_mode_swallows_text_until_insert(ctx):
    """Letters are commands in normal mode; 'i' hands them back to the composer."""
    s = ctx.spawn()
    enable(s)
    s.type("hjkl").sync()
    assert s.composer_text() == "", s.composer_lines()

    s.key("i").sync()
    assert INSERT in s.text()
    s.type("hello").sync()
    assert s.composer_text() == "hello", s.composer_lines()

    s.key("esc").sync()
    assert NORMAL in s.text()
    assert s.composer_text() == "hello", "leaving insert keeps the draft"


def test_word_delete_and_paste_in_the_composer(ctx):
    """'dw' cuts a word into the register and 'p' puts it back."""
    s = ctx.spawn()
    enable(s)
    s.key("i").sync()
    s.type("alpha beta").sync()
    s.key("esc").sync()

    s.key("0").sync()
    s.key("d", "w").sync()
    assert s.composer_text() == "beta", s.composer_lines()

    s.key("$").sync()
    s.key("p").sync()
    # the yanked "alpha " keeps its trailing space; the panel pads it away
    assert s.composer_text() == "betaalpha", s.composer_lines()

    s.key("u").sync()
    assert s.composer_text() == "beta", "u undoes the paste"


def test_x_and_dollar_and_insert_entry_keys(ctx):
    """x deletes under the cursor; A appends at the end of the line."""
    s = ctx.spawn()
    enable(s)
    s.key("i").sync()
    s.type("xabc").sync()
    s.key("esc").sync()
    s.key("0").sync()
    s.key("x").sync()
    assert s.composer_text() == "abc", s.composer_lines()

    s.key("A").sync()
    assert INSERT in s.text()
    s.type("def").sync()
    assert s.composer_text() == "abcdef", s.composer_lines()


def test_enter_submits_from_normal_mode(ctx):
    """A composed line is sent without a detour through insert."""
    ctx.scenario("text=got+it")
    s = ctx.spawn()
    enable(s)
    s.key("i").sync()
    s.type("ship it").sync()
    s.key("esc").sync()
    s.submit()
    s.wait_text("got it")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["messages"][-1]["content"] == "ship it"
    assert NORMAL in s.text(), "submitting stays in normal mode"


def test_hjkl_scrolls_the_transcript(ctx):
    """k at the top row walks back through the scrollback, G returns."""
    s = ctx.spawn()
    fill_transcript(ctx, s)
    enable(s)
    bottom = s.text()

    for _ in range(30):
        s.key("k")
    s.sync()
    assert s.text() != bottom, "the viewport should have moved"

    s.key("G").sync()
    assert s.text() == bottom, "G pins the transcript back to the bottom"


def test_editing_outside_the_composer_is_refused(ctx):
    """An operator aimed at the transcript says so and changes nothing."""
    ctx.scenario("text=keep+every+word")
    s = ctx.spawn()
    s.submit("say something")
    s.wait_text("keep every word")
    s.wait_turn_done()
    enable(s)

    for _ in range(4):
        s.key("k")
    s.sync()
    s.key("d", "d").sync()
    s.wait_text("read-only")
    assert "keep every word" in s.text(), "the transcript is untouched"
    assert s.composer_text() == ""


def test_visual_yanks_the_transcript_to_the_clipboard(ctx):
    """v extends a highlight over the model's own words and y copies them."""
    ctx.scenario("text=copy+this+line")
    s = ctx.spawn()
    s.submit("say something")
    s.wait_text("copy this line")
    s.wait_turn_done()
    enable(s)

    row = next(i for i, line in enumerate(s.screen.lines()) if "copy this line" in line)
    col = s.screen.lines()[row].index("copy this line")
    s.mouse("down", row + 1, col + 1).sync()   # seat the cursor with one click
    s.key("v").sync()
    assert VISUAL in s.text()
    for _ in range("copy this line".index("line") + len("line") - 1):
        s.key("l")
    s.sync()
    s.key("y").sync()
    assert s.screen.clipboard == "copy this line", repr(s.screen.clipboard)
    assert NORMAL in s.text(), "y leaves visual mode"


def test_visual_delete_in_the_composer(ctx):
    """A highlight over composer text is cut like any other range."""
    s = ctx.spawn()
    enable(s)
    s.key("i").sync()
    s.type("keep drop").sync()
    s.key("esc").sync()
    s.key("$").sync()
    s.key("v").sync()
    for _ in range(3):
        s.key("h")
    s.sync()
    s.key("d").sync()
    assert s.composer_text() == "keep", s.composer_lines()
    assert NORMAL in s.text()


def test_typing_stays_live_while_a_turn_streams(ctx):
    """Normal mode navigates mid-turn and Enter is still refused."""
    ctx.scenario("words=200,chunk=4,delay=0.01")
    s = ctx.spawn()
    enable(s)
    s.key("i").sync()
    s.type("go").sync()
    s.key("esc").sync()
    s.submit()

    s.key("i").sync()
    s.type("queued").sync()
    s.key("enter").sync()
    assert s.composer_text() == "queued", "Enter is not honoured mid-turn"
    s.wait_turn_done()
    assert len(ctx.mock.requests) == 1


def test_esc_leaves_insert_even_with_the_popup_open(ctx):
    """Esc ends insert whatever else is on screen; the popup goes with it."""
    s = ctx.spawn()
    enable(s)
    s.key("i").sync()
    s.type("/cl").sync()
    assert "Start a fresh conversation" in s.text(), "the popup should be open"

    s.key("esc").sync()
    assert NORMAL in s.text(), "Esc must not be eaten by the popup"
    assert "Start a fresh conversation" not in s.text()
    assert s.composer_text() == "/cl", "the draft survives the mode change"


def test_colon_opens_the_same_commands(ctx):
    """':' lists yoke's commands, spelled with the colon that opened them."""
    s = ctx.spawn()
    enable(s)
    s.key(":").sync()
    text = s.text()
    assert ":clear" in text and ":vim" in text, text
    assert "/clear" not in text, "the list echoes the prefix that was typed"

    s.type("cl").sync()
    s.key("tab").sync()
    assert s.composer_text() == ":clear", s.composer_lines()


def test_colon_q_quits(ctx):
    """The ex spellings people type without thinking reach '/exit'."""
    s = ctx.spawn()
    enable(s)
    s.key(":").sync()
    s.submit("q")
    assert s.wait_exit() == 0


def test_counts_and_find_motions(ctx):
    """3w, f<char> and ; move by count and by target."""
    s = ctx.spawn()
    enable(s)
    s.key("i").sync()
    s.type("one two three four").sync()
    s.key("esc").sync()

    s.key("0").sync()
    s.key("3", "w").sync()          # onto "four"
    s.key("D").sync()
    assert s.composer_text() == "one two three", s.composer_lines()

    s.key("0").sync()
    s.key("f", "t").sync()          # first 't', in "two"
    s.key("d", "w").sync()
    assert s.composer_text() == "one three", s.composer_lines()


def test_text_objects_and_single_key_edits(ctx):
    """ciw, ~ and J are the edits fingers reach for without a motion."""
    s = ctx.spawn()
    enable(s)
    s.key("i").sync()
    s.type("alpha bravo").sync()
    s.key("esc").sync()

    s.key("0").sync()
    s.key("c", "i", "w").sync()
    assert INSERT in s.text(), "c leaves the cursor in insert"
    s.type("omega").sync()
    s.key("esc").sync()
    assert s.composer_text() == "omega bravo", s.composer_lines()

    s.key("0").sync()
    s.key("3", "~").sync()
    assert s.composer_text() == "OMEga bravo", s.composer_lines()


def test_linewise_delete_and_put(ctx):
    """dd takes the whole composer line and P puts it back above."""
    s = ctx.spawn()
    enable(s)
    s.key("i").sync()
    s.type("first")
    s.key("newline")
    s.type("second").sync()
    s.key("esc").sync()

    s.key("d", "d").sync()
    # the last line takes the break above it, so no empty row is left behind
    assert s.composer_text() == "first", s.composer_lines(2)
    s.key("P").sync()
    assert s.composer_body(2) == ["second", "first"], s.composer_lines(2)


def test_pending_command_shows_in_the_mode_row(ctx):
    """A half-typed operator is visible instead of looking like a dropped key."""
    s = ctx.spawn()
    enable(s)
    s.key("2", "d").sync()
    row = next(line for line in s.screen.lines() if NORMAL in line)
    assert "2d" in row, row
    s.key("esc").sync()
    row = next(line for line in s.screen.lines() if NORMAL in line)
    assert "2d" not in row, "Esc drops the pending command first"
