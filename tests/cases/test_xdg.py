"""XDG Base Directory compliance: where settings and state land.

Settings come from the config dirs and prompt history lives in the state dir,
so nothing yoke owns is written straight into $HOME.
"""


def test_history_lands_in_the_state_dir(ctx):
    """A submitted prompt is mirrored to $HOME/.local/state/yoke/history."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.submit("remember me")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    hist = ctx.home / ".local" / "state" / "yoke" / "history"
    assert hist.exists(), sorted(p.name for p in ctx.home.rglob("*"))
    assert hist.read_text().splitlines() == ["remember me", "/exit"]


def test_nothing_is_written_directly_into_home(ctx):
    """$HOME gains no dotfile of our own, only XDG directories."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.submit("write something down")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    # "work" is the cwd and "xdg" is XDG_CONFIG_HOME, both from the fixture.
    entries = {p.name for p in ctx.home.iterdir()}
    assert entries == {"work", "xdg", ".local"}, entries
    assert (ctx.home / ".local").is_dir()


def test_state_home_env_is_honoured(ctx):
    """XDG_STATE_HOME moves the history file wholesale."""
    state = ctx.tmp / "state"
    ctx.scenario("text=ok")
    s = ctx.spawn(XDG_STATE_HOME=str(state))
    s.submit("into the state dir")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    assert (state / "yoke" / "history").read_text() == "into the state dir\n/exit\n"
    assert not (ctx.home / ".local").exists(), "the default must stay unused"


def test_relative_state_home_is_ignored(ctx):
    """A relative XDG_STATE_HOME is invalid, so the default takes over."""
    ctx.scenario("text=ok")
    s = ctx.spawn(XDG_STATE_HOME="relative/path")
    s.submit("still recorded")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    assert (ctx.home / ".local/state/yoke/history").exists()
    assert not (ctx.work / "relative").exists(), "must not write a relative dir"


def test_created_dirs_are_private(ctx):
    """Directories yoke creates are 0700, as the spec requires."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.submit("private")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    d = ctx.home / ".local" / "state" / "yoke"
    assert d.stat().st_mode & 0o777 == 0o700, oct(d.stat().st_mode)


def test_config_dirs_are_searched(ctx):
    """A config under XDG_CONFIG_DIRS is picked up when the user has none."""
    etc = ctx.tmp / "etc"
    (etc / "yoke").mkdir(parents=True)
    (etc / "yoke" / "config").write_text("model=system-model\n")

    s = ctx.spawn(XDG_CONFIG_DIRS=str(etc), YOKE_MODEL=None)
    assert "system-model" in s.status_line(), s.status_line()


def test_config_home_beats_config_dirs(ctx):
    """XDG_CONFIG_HOME has the higher precedence of the two."""
    etc = ctx.tmp / "etc"
    (etc / "yoke").mkdir(parents=True)
    (etc / "yoke" / "config").write_text("model=system-model\n")
    ctx.write_config("model=user-model\n")

    s = ctx.spawn(XDG_CONFIG_DIRS=str(etc), YOKE_MODEL=None)
    status = s.status_line()
    assert "user-model" in status and "system-model" not in status, status


def test_env_beats_every_config_file(ctx):
    """The environment still wins over both config layers."""
    etc = ctx.tmp / "etc"
    (etc / "yoke").mkdir(parents=True)
    (etc / "yoke" / "config").write_text("model=system-model\n")
    ctx.write_config("model=user-model\n")

    s = ctx.spawn(XDG_CONFIG_DIRS=str(etc))
    assert "mock-model" in s.status_line(), s.status_line()
