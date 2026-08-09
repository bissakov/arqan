"""The /statusline visibility menu."""


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
)


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
    s = ctx.spawn()
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

