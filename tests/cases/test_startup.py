"""Startup, chrome and layout at a range of terminal sizes."""


def test_first_frame(ctx):
    """Opening frame: empty composer, placeholder, status line."""
    s = ctx.spawn()
    ctx.check_screen(s)


def test_status_line_fields(ctx):
    """Status line carries model, provider, cwd and an unknown token count."""
    s = ctx.spawn()
    status = s.status_line()
    assert "mock-model" in status, status
    # base_url is http://127.0.0.1:PORT/v1 → the loopback host reads as "local"
    assert "local" in status, status
    assert "~/work" in status, status
    assert "\u2014" in status, status  # em dash: no usage reported yet
    assert str(ctx.mock.port) not in status, "port must not leak into the UI"


def test_placeholder_and_prompt(ctx):
    """The composer shows a prompt marker and a dimmed placeholder."""
    s = ctx.spawn()
    composer = s.composer_rows()
    assert len(composer) == 1, composer
    assert composer[0].strip().startswith("\u203a"), composer
    assert "Message ah" in composer[0], composer


def test_alt_screen_and_modes(ctx):
    """Startup claims the alternate screen and SGR mouse reporting."""
    s = ctx.spawn()
    t = s.screen
    assert t.alt_active, "should be on the alternate screen"
    assert t.modes.get(1049) is True
    assert t.modes.get(1002) is True, "drag reporting is needed for selection"
    assert t.modes.get(1006) is True, "SGR coordinates are needed past col 223"
    assert t.modes.get(7) is False, "autowrap off: the UI wraps text itself"


def test_narrow_terminal(ctx):
    """A 48-column terminal keeps every row inside the screen."""
    s = ctx.spawn(cols=48, rows=16)
    ctx.check_screen(s)
    for line in s.screen.lines():
        assert len(line) <= 48, line


def test_tiny_terminal(ctx):
    """A 20x8 terminal still renders a transcript row and a status line."""
    s = ctx.spawn(cols=20, rows=8)
    ctx.check_screen(s)
    assert "mock-model" in s.status_line() or "mock" in s.status_line()


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
    out = ctx.run_piped("/exit\n", AH_API_KEY=None)
    assert "no API key" in out.stdout, out.stdout


def test_piped_banner(ctx):
    """Without a tty, ah stays line-oriented and prints a banner."""
    out = ctx.run_piped("/exit\n")
    assert out.returncode == 0, out
    assert "ah 0.1.0" in out.stdout, out.stdout
    assert "model=mock-model" in out.stdout, out.stdout
    assert "tools=" in out.stdout, out.stdout
    assert "\x1b[?1049h" not in out.stdout, "must not touch the alternate screen"


def test_config_file_is_read(ctx):
    """Values in $XDG_CONFIG_HOME/ah/config are picked up when env is unset."""
    ctx.write_config(f"model=from-config\nbase_url={ctx.mock.base_url}\n")
    s = ctx.spawn(AH_MODEL=None)
    assert "from-config" in s.status_line(), s.status_line()


def test_env_beats_config_file(ctx):
    """AH_MODEL wins over the config file."""
    ctx.write_config("model=from-config\n")
    s = ctx.spawn(AH_MODEL="from-env")
    assert "from-env" in s.status_line(), s.status_line()
