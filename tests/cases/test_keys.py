"""Keybindings: the shadow pairs, and the page that lists them.

A key is bound once per input context. Where one key does two things, the
choice lives inside a single binding and turns on state the reader can see:
the completion popup being open, the composer being empty, which context owns
the keyboard. These cases pin both sides of every such pair, so a binding
added later cannot quietly take a key that already had a meaning.

`/keys` is the same table rendered, so a duplicate that survived the compiler
shows up here as one key listed twice under one context.
"""

import re


# ---- the page ------------------------------------------------------------

def open_keys(s):
    s.submit("/keys")
    return s.wait_text("Ctrl-R")


def whole_page(s) -> list[str]:
    """The page walked row by row, since it is taller than the screen.

    The highlight identifies one row and wraps at the end, so walking it back
    to the heading it started on reads every row exactly once. Collecting
    screenfuls instead would have to drop rows it had already seen, and a
    duplicate binding is precisely a row seen twice.
    """
    def selected() -> str:
        return s.popup_selected().removeprefix("\u203a ").strip()

    first = selected()
    assert first.startswith("["), f"the page opens on a heading, not {first!r}"
    rows = [first]
    for _ in range(500):
        s.key("down").sync()
        row = selected()
        if row == first:
            return rows
        rows.append(row)
    raise AssertionError("the keys page never wrapped back to its first row")


def parse_bindings(rows: list[str]) -> dict[str, list[str]]:
    """Rows to {context: [key, ...]}.

    A context heading is bracketed and stands alone; a binding row is a key
    name, two or more spaces, and what the key does.
    """
    out: dict[str, list[str]] = {}
    context = None
    for row in rows:
        m = re.match(r"^\[(.+?)\]$", row)
        if m:
            context = m.group(1)
            out.setdefault(context, [])
            continue
        if context is None:
            continue
        m = re.match(r"^((?:Ctrl|Alt|Shift)-\w+|[A-Z][a-z]+)\s{2,}\S", row)
        if m:
            out[context].append(m.group(1))
    return out


def test_keys_page_lists_bindings_grouped_by_context(ctx):
    """The page is the binding table, so every context it has is on it."""
    s = ctx.spawn()
    open_keys(s)
    bindings = parse_bindings(whole_page(s))
    assert set(bindings) == {
        "composer", "line editing", "transcript search", "lists and screens"
    }, sorted(bindings)
    assert "Ctrl-R" in bindings["composer"], bindings["composer"]
    assert "Ctrl-G" in bindings["transcript search"], \
        bindings["transcript search"]
    assert "Ctrl-Y" in bindings["line editing"], bindings["line editing"]


def test_no_key_is_bound_twice_in_one_context(ctx):
    """The invariant the compiler enforces, checked against what ships."""
    s = ctx.spawn()
    open_keys(s)
    bindings = parse_bindings(whole_page(s))
    assert bindings, "the page listed nothing to check"
    for context, keys in bindings.items():
        assert keys, f"{context} listed no bindings"
        dupes = {k for k in keys if keys.count(k) > 1}
        assert not dupes, f"{context} binds {sorted(dupes)} more than once"


def test_keys_page_is_chrome_and_sends_no_request(ctx):
    """Reading the bindings is not a message to the model."""
    ctx.scenario("text=conversation+stays")
    s = ctx.spawn()
    s.submit("start here")
    s.wait_turn_done()

    open_keys(s)
    assert len(ctx.mock.requests) == 1, ctx.mock.requests
    s.key("esc")
    s.wait_gone("Ctrl-R")
    assert "conversation stays" in s.text(), s.text()


def test_the_page_opens_while_a_turn_streams(ctx):
    """Looking up a key changes neither the conversation nor the request."""
    ctx.scenario("first_delay=6,text=done")
    s = ctx.spawn()
    s.submit("go on")
    s.wait_activity("thinking")

    s.submit("/keys")
    s.wait_text("Ctrl-R")
    s.wait_text("done")                 # the turn ran on under the page
    s.key("esc").sync()
    s.wait_gone("Ctrl-R")
    s.wait_turn_done()
    assert len(ctx.mock.requests) == 1, "the turn was not restarted"


# ---- shadow pairs --------------------------------------------------------

def test_ctrl_n_and_ctrl_p_move_the_popup_when_it_is_open(ctx):
    """With the command popup up, Ctrl-N/Ctrl-P walk it, not the history."""
    s = ctx.spawn()
    s.type("/").sync()
    first = s.popup_selected()
    assert first, s.text()
    s.key("ctrl-n").sync()
    second = s.popup_selected()
    assert second and second != first, (first, second)
    s.key("ctrl-p").sync()
    assert s.popup_selected() == first, (first, s.popup_selected())


def test_arrows_and_ctrl_n_move_the_popup_alike_on_an_exact_match(ctx):
    """One key, one binding: Down walks the popup wherever Ctrl-N does.

    A fully typed name is the command asked for, so the popup opens on it.
    Down used to be a second, later path that rebuilt the list after moving,
    and the rebuild put the highlight straight back on the exact match.
    """
    s = ctx.spawn()
    s.type("/mode").sync()
    exact = s.popup_selected()
    assert "/mode " in exact, exact

    s.key("down").sync()
    assert "/model" in s.popup_selected(), \
        f"Down left the highlight on {exact!r}"

    s.key("up").sync()
    assert s.popup_selected() == exact, s.popup_selected()

    s.key("ctrl-n").sync()
    assert "/model" in s.popup_selected(), s.popup_selected()


def test_ctrl_p_walks_history_when_no_popup_is_open(ctx):
    """The same key, the other side of its guard: the draft is recalled."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.submit("first message")
    s.wait_turn_done()

    assert s.composer_text() == "", s.composer_lines()
    s.key("ctrl-p").sync()
    assert s.composer_text() == "first message", s.composer_lines()
    s.key("ctrl-n").sync()
    assert s.composer_text() == "", s.composer_lines()


def test_ctrl_d_deletes_forward_but_only_exits_when_empty(ctx):
    """Ctrl-D is EOF on an empty composer and forward-delete otherwise."""
    s = ctx.spawn()
    s.type("abc").sync()
    s.key("home").sync()
    s.key("ctrl-d").sync()
    assert s.composer_text() == "bc", s.composer_lines()

    s.key("ctrl-d", "ctrl-d").sync()
    assert s.composer_text() == "", s.composer_lines()
    s.key("ctrl-d")
    s.wait_exit()


def test_enter_picks_from_the_popup_and_submits_otherwise(ctx):
    """Enter is one binding: the popup takes it only while it is open."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.type("plain message").sync()
    assert not s.popup_selected() or "/" not in s.popup_selected()
    s.key("enter")
    s.wait_for(lambda t: s.composer_text() == "", "the composer to clear")

    s.type("/ab").sync()
    assert "/about" in s.popup_selected(), s.popup_selected()
    s.key("enter")
    s.wait_text("Alikhan Bissakov")
    s.key("esc").sync()


def test_ctrl_e_is_end_of_line_in_the_composer_and_expand_in_find(ctx):
    """One byte, two contexts: each binds it once, and neither leaks."""
    s = ctx.spawn()
    s.type("hello").sync()
    s.key("home").sync()
    s.key("ctrl-e").sync()
    s.type("!").sync()
    assert s.composer_text() == "hello!", s.composer_lines()

    s.key("ctrl-r").sync()
    s.type("hello").sync()
    s.key("ctrl-e").sync()
    assert s.composer_text() == "hello!", "find keys never edit the draft"
    s.key("ctrl-g").sync()


def test_escape_in_find_closes_the_box_without_arming_a_rewind(ctx):
    """Esc belongs to whichever context owns the keyboard, once each."""
    ctx.scenario("text=alpha+needle")
    s = ctx.spawn()
    s.submit("question")
    s.wait_turn_done()

    s.key("ctrl-r").sync()
    s.type("needle").sync()
    assert "find:" in s.text(), s.text()
    s.key("esc").sync()
    assert "find:" not in s.text(), s.text()
    assert "Press Escape again" not in s.text(), s.text()
