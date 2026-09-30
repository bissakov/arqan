"""Colour themes: the built-in set, theme files, and /theme."""


def nearest_256(r, g, b):
    """The xterm-256 index a #rrggbb falls back to without truecolor: the
    nearer of the 6x6x6 cube and the grey ramp."""
    levels = [0, 95, 135, 175, 215, 255]

    def near(v):
        return min(range(6), key=lambda i: abs(levels[i] - v))

    ci = [near(v) for v in (r, g, b)]
    cube = [levels[i] for i in ci]
    cube_d = sum((a - c) ** 2 for a, c in zip((r, g, b), cube))
    grey_i = min(range(24),
                 key=lambda i: sum((v - (8 + 10 * i)) ** 2 for v in (r, g, b)))
    grey = 8 + 10 * grey_i
    grey_d = sum((v - grey) ** 2 for v in (r, g, b))
    if grey_d < cube_d:
        return 232 + grey_i
    return 16 + 36 * ci[0] + 6 * ci[1] + ci[2]


def converse(ctx, **env):
    ctx.scenario("text=answered")
    s = ctx.spawn(**env)
    s.submit("my question")
    s.wait_text("answered")
    s.wait_turn_done()
    return s


def user_bg(s):
    row = s.screen.find_row("my question")
    assert row >= 0, s.text()
    return s.screen.attr_at(row, 2).bg


def reply_fg(s):
    row = s.screen.find_row("answered")
    assert row >= 0, s.text()
    return s.screen.attr_at(row, 2).fg


def write_theme(ctx, name, body):
    p = ctx.xdg / "arqan" / "themes" / f"{name}.toml"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(body)
    return p


def test_the_default_theme_keeps_the_dark_colours(ctx):
    ctx.write_file("f.txt", "file body")
    ctx.scenario('tool=read:{"path":"f.txt"},final_text=answered')
    s = ctx.spawn()
    s.submit("my question")
    s.wait_text("answered")
    s.wait_turn_done()
    assert user_bg(s) == 238
    assert reply_fg(s) == 253
    row = s.screen.find_row("\u25c6  read f.txt")
    assert s.screen.attr_at(row, 2).fg == 221


def test_the_light_theme_recolours_the_screen(ctx):
    ctx.write_config('theme = "light"\n')
    s = converse(ctx)
    assert user_bg(s) == 253
    assert reply_fg(s) == 236


def test_a_hex_colour_is_truecolor_only_when_the_terminal_says_so(ctx):
    ctx.write_config('theme = "kanagawa-wave"\n')
    s = converse(ctx, COLORTERM="truecolor")
    assert user_bg(s) == (0x36, 0x36, 0x46)
    s.close()

    s = converse(ctx, COLORTERM=None)
    assert user_bg(s) == nearest_256(0x36, 0x36, 0x46)


def test_a_theme_file_overrides_its_base(ctx):
    write_theme(ctx, "mine", 'base = "light"\ntext = 33\n')
    ctx.write_config('theme = "mine"\n')
    s = converse(ctx)
    assert reply_fg(s) == 33
    assert user_bg(s) == 253


def test_a_bad_theme_value_is_reported_and_dropped(ctx):
    path = write_theme(ctx, "mine", 'base = "light"\ntext = "red"\nbogus = 1\n')
    ctx.write_config('theme = "mine"\n')
    ctx.scenario("text=ok")
    out = ctx.run_cli("-p", "hello")
    assert "text" in out.stderr and str(path) in out.stderr, out.stderr
    assert "bogus" in out.stderr, out.stderr

    s = converse(ctx)
    assert reply_fg(s) == 236
    assert user_bg(s) == 253


def test_a_bad_value_in_a_picked_theme_is_a_notice(ctx):
    write_theme(ctx, "mine", 'text = "red"\n')
    s = ctx.spawn()
    s.submit("/theme")
    s.wait_text("kanagawa-lotus")
    for _ in range(16):
        if "mine" in s.popup_selected():
            break
        s.key("down").sync()
    s.key("enter")
    s.wait_text("ignoring text in")


def test_an_unknown_theme_falls_back_to_dark(ctx):
    for name in ("nope", "../x"):
        ctx.write_config(f'theme = "{name}"\n')
        ctx.scenario("text=ok")
        out = ctx.run_cli("-p", "hello")
        assert f"no theme named {name}; using dark" in out.stderr, out.stderr
    s = converse(ctx)
    assert user_bg(s) == 238
    assert reply_fg(s) == 253


def test_the_theme_command_switches_and_remembers(ctx):
    s = converse(ctx)
    assert user_bg(s) == 238
    s.submit("/theme")
    s.wait_text("kanagawa-wave")
    assert "dark" in s.popup_selected(), s.text()
    for _ in range(8):
        if "light" in s.popup_selected():
            break
        s.key("down").sync()
    s.key("enter")
    s.wait_gone("kanagawa-wave")
    s.wait_for(lambda t: user_bg(s) == 253, "the light user box")
    assert reply_fg(s) == 236
    assert ctx.state().get("theme") == "light", ctx.state()
    s.close()

    again = converse(ctx)
    assert user_bg(again) == 253


def test_no_color_wins_over_a_theme(ctx):
    ctx.write_config('theme = "kanagawa-wave"\n')
    s = converse(ctx, NO_COLOR="1", COLORTERM="truecolor")
    assert reply_fg(s) is None
    assert user_bg(s) is None


def blank_bg(s):
    """The background of a cell nothing was written to: the page."""
    return s.screen.attr_at(0, 0).bg


def test_a_light_theme_paints_the_page(ctx):
    ctx.write_config('theme = "light"\n')
    s = converse(ctx)
    assert blank_bg(s) == 231
    row = s.screen.find_row("answered")
    assert s.screen.attr_at(row, 2).bg == 231
    assert s.screen.attr_at(row, s.screen.cols - 1).bg == 231, "row tail"
    status = s.screen.rows - 1
    assert s.screen.attr_at(status, 0).bg == 231, "status line"
    assert s.screen.attr_at(status, s.gutter()).bg == 231, "status bullet"


def test_a_kanagawa_theme_paints_the_page_in_truecolor(ctx):
    ctx.write_config('theme = "kanagawa-wave"\n')
    s = converse(ctx, COLORTERM="truecolor")
    assert blank_bg(s) == (0x1F, 0x1F, 0x28)
    assert reply_fg(s) == (0xDC, 0xD7, 0xBA)


def test_the_dark_theme_leaves_the_page_to_the_terminal(ctx):
    s = converse(ctx)
    assert blank_bg(s) is None
    row = s.screen.find_row("answered")
    assert s.screen.attr_at(row, s.screen.cols - 1).bg is None


def test_default_in_a_theme_file_means_the_page_colour(ctx):
    write_theme(ctx, "mine", 'base = "light"\nuser_bg = "default"\n')
    ctx.write_config('theme = "mine"\n')
    s = converse(ctx)
    assert user_bg(s) == 231


def test_switching_theme_repaints_the_page(ctx):
    s = converse(ctx)
    assert blank_bg(s) is None
    s.submit("/theme")
    s.wait_text("kanagawa-wave")
    for _ in range(8):
        if "light" in s.popup_selected():
            break
        s.key("down").sync()
    s.key("enter")
    s.wait_gone("kanagawa-wave")
    s.wait_for(lambda t: blank_bg(s) == 231, "the light page")
    row = s.screen.find_row("answered")
    assert s.screen.attr_at(row, s.screen.cols - 1).bg == 231


def test_no_color_paints_no_page(ctx):
    ctx.write_config('theme = "light"\n')
    s = converse(ctx, NO_COLOR="1")
    assert blank_bg(s) is None


def test_a_light_theme_leaves_no_cell_on_the_terminal_background(ctx):
    ctx.write_file("f.txt", "file body")
    ctx.scenario('tool=read:{"path":"f.txt"},final_text=# Title\n\nSome *text* '
                 'and `code`.\n\n```c\nint x = 1; // note\n```\n\n> quoted\n')
    ctx.write_config('theme = "light"\n')
    s = ctx.spawn()
    s.submit("my question")
    s.wait_text("quoted")
    s.wait_turn_done()
    s.type("/").sync()
    holes = [(r, c) for r in range(s.screen.rows) for c in range(s.screen.cols)
             if s.screen.attr_at(r, c).bg is None]
    assert not holes, f"{holes[:10]}\n{s.text()}"
    unset = [(r, c) for r in range(s.screen.rows) for c in range(s.screen.cols)
             if s.screen.attr_at(r, c).fg is None
             and s.screen.buf.chars[r][c].strip()]
    assert not unset, f"{unset[:10]}\n{s.text()}"


def test_a_painted_page_sets_the_cursor_colour(ctx):
    ctx.write_config('theme = "light"\n')
    s = ctx.spawn()
    assert s.term.cursor_colour == "#303030", s.term.cursor_colour
    s.close()

    ctx.write_config('theme = "kanagawa-lotus"\n')
    s = ctx.spawn()
    assert s.term.cursor_colour == "#545464", s.term.cursor_colour


def test_the_dark_theme_leaves_the_cursor_alone(ctx):
    s = ctx.spawn()
    assert s.term.cursor_colour is None


def test_no_color_leaves_the_cursor_alone(ctx):
    ctx.write_config('theme = "light"\n')
    s = ctx.spawn(NO_COLOR="1")
    assert s.term.cursor_colour is None


def test_the_cursor_colour_follows_a_theme_switch_and_exit(ctx):
    ctx.write_config('theme = "light"\n')
    s = ctx.spawn()
    assert s.term.cursor_colour == "#303030"
    s.submit("/theme")
    s.wait_text("kanagawa-wave")
    for _ in range(8):
        if "kanagawa-wave" in s.popup_selected():
            break
        s.key("down").sync()
    s.key("enter")
    s.wait_for(lambda t: s.term.cursor_colour == "#dcd7ba", "the wave cursor")
    s.submit("/theme")
    s.wait_text("kanagawa-lotus")
    for _ in range(8):
        if "dark" in s.popup_selected():
            break
        s.key("up").sync()
    s.key("enter")
    s.wait_for(lambda t: s.term.cursor_colour is None, "the terminal's cursor")
    s.submit("/theme")
    s.wait_text("kanagawa-lotus")
    for _ in range(8):
        if "light" in s.popup_selected():
            break
        s.key("down").sync()
    s.key("enter")
    s.wait_for(lambda t: s.term.cursor_colour == "#303030", "the light cursor")
    s.submit("/exit")
    assert s.wait_exit() == 0
    assert s.term.cursor_colour is None, "exit gives the cursor colour back"
