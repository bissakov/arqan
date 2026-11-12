"""The /statusline visibility menu."""

import re


ITEMS = (
    "State",
    "Model",
    "Reasoning effort",
    "Thinking budget",
    "Mode",
    "Provider",
    "Working directory",
    "Context tokens",
    "Copy confirmation",
    "Permissions",
    "Step list",
    "Cache window",
)

# A long working directory would push the cache field off the row.
CACHE_FIELDS = str(1 | 2 | 2048)


def open_statusline(s):
    s.submit("/statusline")
    return s.wait_status("status line")


def select(s, label):
    for _ in range(len(ITEMS)):
        if label in s.popup_selected():
            return s
        s.key("down").sync()
    raise AssertionError(f"no status-line row for {label!r}\n{s.text()}")


def test_statusline_lists_every_item_as_visible(ctx):
    """Every independently rendered field has a checkbox."""
    s = ctx.spawn(ARQAN_STATUS_FIELDS="4095")
    open_statusline(s)
    for label in ITEMS:
        assert f"[x] {label}" in s.popup_selected(), s.text()
        s.key("down").sync()


def test_statusline_can_hide_and_restore_the_model(ctx):
    """A checkbox changes the live row and the menu remains open."""
    s = ctx.spawn()
    open_statusline(s)
    select(s, "Model").key("space").sync()
    assert "[ ] Model" in s.popup_selected(), s.text()
    assert "mock-model" not in s.status_line(), s.status_line()

    s.key("space").sync()
    assert "[x] Model" in s.popup_selected(), s.text()
    assert "mock-model" in s.status_line(), s.status_line()


def test_statusline_can_hide_the_leading_state(ctx):
    """The next visible item starts the row without a stray separator."""
    s = ctx.spawn()
    open_statusline(s)
    s.key("space").sync()
    assert "[ ] State" in s.popup_selected(), s.text()
    s.key("esc")
    s.wait_for(lambda t: not s.status_line().startswith("●"),
               "state field to disappear")
    assert s.status_line().startswith("mock-model · "), s.status_line()


def test_statusline_visibility_survives_a_restart(ctx):
    s = ctx.spawn()
    open_statusline(s)
    select(s, "Model").key("space").sync()
    s.key("esc").sync()
    s.wait_status("ready")
    s.submit("/exit")
    s.wait_exit()

    again = ctx.spawn()
    assert "mock-model" not in again.status_line(), again.status_line()
    open_statusline(again)
    select(again, "Model")
    assert "[ ] Model" in again.popup_selected(), again.text()


def test_the_cache_window_counts_down_then_expires(ctx):
    """A cached request starts a countdown that ends in a word, not 0:00."""
    ctx.scenario("text=hi,usage=1000/10,cache_creation=1000")
    s = ctx.spawn(ARQAN_STATUS_FIELDS=CACHE_FIELDS,
                  ARQAN_TEST_CACHE_TTL_MS="3000")
    assert "cache" not in s.status_line(), s.status_line()
    s.submit("one")
    s.wait_turn_done()
    s.wait_for(lambda t: re.search(r"cache 0:0[1-3]$", s.status_line()),
               "the countdown")
    s.wait_for(lambda t: s.status_line().endswith("cache expired"),
               "the window to expire")
    assert "0:00" not in s.status_line(), s.status_line()

    ctx.scenario("text=hi,usage=1000/10,cache_read=1000")
    s.submit("two")
    s.wait_turn_done()
    s.wait_for(lambda t: re.search(r"cache 0:0[1-3]$", s.status_line()),
               "the next request to reopen the window")


def test_the_cache_window_waits_for_a_cache(ctx):
    """A provider that reports no cache tokens shows no countdown."""
    ctx.scenario("text=hi,usage=1000/10")
    s = ctx.spawn(ARQAN_STATUS_FIELDS=CACHE_FIELDS)
    s.submit("one")
    s.wait_turn_done()
    s.sync()
    assert "cache" not in s.status_line(), s.status_line()


def test_a_new_conversation_closes_the_cache_window(ctx):
    """/clear starts a conversation with nothing cached yet."""
    ctx.scenario("text=hi,usage=1000/10,cache_creation=1000")
    s = ctx.spawn(ARQAN_STATUS_FIELDS=CACHE_FIELDS)
    s.submit("one")
    s.wait_turn_done()
    s.wait_for(lambda t: re.search(r"cache [45]:\d\d$", s.status_line()),
               "the countdown")
    s.submit("/clear")
    s.wait_for(lambda t: "cache" not in s.status_line(), "the field to go")
