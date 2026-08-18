"""Startup, chrome and layout at a range of terminal sizes."""

from tests.context import VERSION

# A fragment of the ASCII-art logo the welcome screen paints; nothing else
# on any frame contains it.
WELCOME_ART = "| (_| | | | (_| | (_| | | | |"
WELCOME_HINT = "type a message and press Enter to begin"


def test_first_frame(ctx):
    """Opening frame: empty composer, placeholder, status line."""
    s = ctx.spawn()
    ctx.check_screen(s)


def test_status_line_fields(ctx):
    """Status line carries model, provider, cwd and an estimated token count."""
    s = ctx.spawn()
    status = s.status_line()
    assert "mock-model" in status, status
    # base_url is http://127.0.0.1:PORT/v1, so the loopback host reads as "local"
    assert "local" in status, status
    assert "~/work" in status, status
    # No request has been measured, but the prompt and the schemas one would
    # carry are already known, so the field estimates rather than shrugs.
    assert s.status_field(5).startswith("~"), status
    assert str(ctx.mock.port) not in status, "port must not leak into the UI"


def test_placeholder_and_prompt(ctx):
    """The composer shows a prompt marker and a dimmed placeholder."""
    s = ctx.spawn()
    composer = s.composer_rows()
    assert len(composer) == 1, composer
    assert composer[0].strip().startswith("\u203a"), composer
    assert "Message arqan" in composer[0], composer


def test_alt_screen_and_modes(ctx):
    """Startup claims the alternate screen and SGR mouse reporting."""
    s = ctx.spawn()
    t = s.screen
    assert t.alt_active, "should be on the alternate screen"
    assert t.modes.get(1049) is True
    assert t.modes.get(1003) is True, "motion reporting is needed for hover"
    assert t.modes.get(1006) is True, "SGR coordinates are needed past col 223"
    assert t.modes.get(7) is False, "autowrap off: the UI wraps text itself"


def test_narrow_terminal(ctx):
    """A 48-column terminal keeps every row inside the screen."""
    s = ctx.spawn(cols=48, rows=16)
    ctx.check_screen(s)
    for line in s.screen.lines():
        assert len(line) <= 48, line


def test_tiny_terminal(ctx):
    """A 20x8 terminal shows only the centered minimum-size warning."""
    s = ctx.spawn(cols=20, rows=8, wait=False)
    s.wait_text("Terminal too small")
    ctx.check_screen(s)
    assert "20x8; need 40x12" in s.text()
    assert "mock-model" not in s.text()


def test_small_terminal_warning_tracks_resize(ctx):
    """Either undersized dimension warns, and restoring both repaints the UI."""
    s = ctx.spawn(cols=80, rows=24)

    s.resize(cols=30, rows=24).sync()
    assert "Terminal size too small:" in s.text()
    assert "Width = 30 Height = 24" in s.text()
    assert "Width = 40 Height = 12" in s.text()

    s.resize(cols=80, rows=10).sync()
    assert "Terminal size too small:" in s.text()
    assert "Width = 80 Height = 10" in s.text()
    assert "Width = 40 Height = 12" in s.text()

    s.resize(cols=80, rows=24)
    s.wait_text(WELCOME_ART).sync()
    assert "Terminal size too small:" not in s.text()
    assert "mock-model" in s.status_line()


def test_welcome_screen_is_centered(ctx):
    """An empty transcript shows the welcome art in the middle of the view."""
    s = ctx.spawn()
    assert WELCOME_ART in s.text()
    assert WELCOME_HINT in s.text()
    art_row = s.screen.find_row(WELCOME_ART)          # 0-based
    assert 3 <= art_row <= s.rows - 7, f"art row {art_row} is not vertically centered"
    line = s.row(s.screen.find_row(WELCOME_HINT))
    left = len(line) - len(line.lstrip())
    right = s.cols - len(line.rstrip())
    assert abs(left - right) <= 4, f"hint not horizontally centered: {line!r}"


def test_welcome_screen_goes_away_on_submit(ctx):
    """The first submitted message replaces the welcome screen for good."""
    ctx.scenario("text=hello")
    s = ctx.spawn()
    assert WELCOME_ART in s.text()
    s.submit("hi")
    s.wait_turn_done()
    assert WELCOME_ART not in s.text()
    assert WELCOME_HINT not in s.text()


def test_welcome_screen_hidden_when_narrow(ctx):
    """A terminal too narrow for the welcome text shows none of it."""
    s = ctx.spawn(cols=42, rows=24)
    assert WELCOME_ART not in s.text()
    assert "type a message" not in s.text()


def test_welcome_screen_hidden_when_short(ctx):
    """A terminal too short for the art keeps its transcript rows blank."""
    s = ctx.spawn(cols=80, rows=15)
    assert WELCOME_ART not in s.text()
    assert "type a message" not in s.text()


def test_resize_repaints(ctx):
    """SIGWINCH re-lays-out the frame at the new size."""
    s = ctx.spawn(cols=80, rows=24)
    s.type("resize me")
    s.wait_text("resize me")
    s.resize(60, 18)
    s.wait_for(lambda t: t.rows == 18 and "mock-model" in t.row_text(17), "reflow")
    ctx.check_screen(s)
    assert "resize me" in "\n".join(s.composer_rows())


def test_no_api_key_warns_without_tty(ctx):
    """The non-tty banner reports a missing key instead of silently failing."""
    out = ctx.run_piped("/exit\n", ARQAN_API_KEY=None)
    assert "no API key" in out.stdout, out.stdout


def test_piped_banner(ctx):
    """Without a tty, arqan stays line-oriented and prints a banner."""
    out = ctx.run_piped("/exit\n")
    assert out.returncode == 0, out
    assert f"arqan {VERSION}" in out.stdout, out.stdout
    assert "model=mock-model" in out.stdout, out.stdout
    assert "tools=" in out.stdout, out.stdout
    assert "\x1b[?1049h" not in out.stdout, "must not touch the alternate screen"


def test_unconfigured_piped_banner_says_setup_without_fake_endpoint(ctx):
    out = ctx.run_piped(
        "/exit\n", ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None
    )
    assert "setup" in out.stdout, out.stdout
    assert "model=" not in out.stdout and "base=" not in out.stdout, out.stdout


def test_config_file_is_read(ctx):
    """Values in $XDG_CONFIG_HOME/arqan/config are picked up when env is unset."""
    ctx.write_config(f"model=from-config\nbase_url={ctx.mock.base_url}\n")
    s = ctx.spawn(ARQAN_MODEL=None)
    assert "from-config" in s.status_line(), s.status_line()


def test_env_beats_config_file(ctx):
    """ARQAN_MODEL wins over the config file."""
    ctx.write_config("model=from-config\n")
    s = ctx.spawn(ARQAN_MODEL="from-env")
    assert "from-env" in s.status_line(), s.status_line()
