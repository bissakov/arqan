"""The settings files: one config to edit, one state file yoke remembers in.

Providers are sections of the config file rather than a store of their own, so
a write by the UI has to leave the rest of the document exactly as its owner
wrote it.
"""

CONFIG = """\
# my endpoints
max_tokens = 1234
unknown_key = kept

[provider work]
base_url = {url}
model = alpha
"""


def state_dir(ctx):
    return ctx.home / ".local" / "state" / "yoke"


def select_provider(ctx, name):
    p = ctx.state_file()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(f"provider = {name}\n")


def test_a_hand_written_provider_section_configures_the_run(ctx):
    """A provider is a section of the config file, editable like any key."""
    ctx.write_config(CONFIG.format(url=ctx.mock.base_url))
    select_provider(ctx, "work")
    ctx.scenario("text=hi")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    assert s.status_field(1) == "alpha", s.status_line()
    assert s.status_field(3) == "work", s.status_line()
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["model"] == "alpha"


def test_writing_a_provider_keeps_the_rest_of_the_config(ctx):
    """/model rewrites one key, not the file: comments and order survive."""
    ctx.write_config(CONFIG.format(url=ctx.mock.base_url))
    select_provider(ctx, "work")
    ctx.scenario("models=alpha|beta")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.submit("/model")
    s.wait_status("pick a model")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("model: beta")

    text = ctx.config_file().read_text()
    assert "# my endpoints" in text, text
    settings = ctx.settings(ctx.config_file())
    assert settings[""]["max_tokens"] == "1234", settings
    assert settings[""]["unknown_key"] == "kept", settings
    assert settings["provider work"]["model"] == "beta", settings
    assert settings["provider work"]["base_url"] == ctx.mock.base_url, settings


def test_a_write_keeps_the_config_files_mode(ctx):
    """The file is the user's, so yoke does not decide who may read it."""
    ctx.write_config(CONFIG.format(url=ctx.mock.base_url))
    ctx.config_file().chmod(0o644)
    select_provider(ctx, "work")
    ctx.scenario("models=alpha|beta")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.submit("/model")
    s.wait_status("pick a model")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("model: beta")

    mode = ctx.config_file().stat().st_mode & 0o777
    assert mode == 0o644, oct(mode)


def test_a_provider_in_the_config_dirs_is_offered(ctx):
    """The system config is searched for providers as it is for keys."""
    etc = ctx.tmp / "etc"
    (etc / "yoke").mkdir(parents=True)
    (etc / "yoke" / "config").write_text(
        f"[provider sitewide]\nbase_url = {ctx.mock.base_url}\n"
    )
    s = ctx.spawn(XDG_CONFIG_DIRS=str(etc))
    s.submit("/provider")
    s.wait_status("pick a provider")
    assert "sitewide" in s.text(), s.text()
    s.key("esc")


def test_every_remembered_choice_lands_in_one_state_file(ctx):
    """Model, provider and telemetry share the state file and own no others."""
    ctx.scenario("models=alpha|beta")
    s = ctx.spawn(YOKE_MODEL=None)
    s.settings_toggle("Telemetry")
    s.submit("/model")
    s.wait_status("pick a model")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("model: beta")
    s.submit("/exit")
    s.wait_exit()

    assert ctx.state() == {"model": "beta", "telemetry": "on"}, ctx.state()
    left = {p.name for p in state_dir(ctx).iterdir()}
    assert left == {"state", "history", "telemetry"}, left
