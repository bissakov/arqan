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


def test_an_older_per_key_state_file_is_folded_into_the_state_file(ctx):
    """A word in a file of its own becomes a key, and the file is removed."""
    d = state_dir(ctx)
    d.mkdir(parents=True, exist_ok=True)
    (d / "model").write_text("gamma\n")
    (d / "telemetry").write_text("on\n")

    s = ctx.spawn(YOKE_MODEL=None)
    assert s.status_field(1) == "gamma", s.status_line()
    s.submit("/exit")
    s.wait_exit()

    assert ctx.state()["model"] == "gamma", ctx.state()
    assert ctx.state()["telemetry"] == "on", ctx.state()
    left = {p.name for p in d.iterdir()}
    assert "model" not in left, left
    # The name is free again: the record is the directory that took it.
    assert (d / "telemetry").is_dir(), left


def test_the_state_file_wins_over_the_older_one(ctx):
    """The file the UI has been writing since is the answer; the other goes."""
    d = state_dir(ctx)
    d.mkdir(parents=True, exist_ok=True)
    ctx.state_file().write_text("model = current\n")
    (d / "model").write_text("stale\n")

    s = ctx.spawn(YOKE_MODEL=None)
    assert s.status_field(1) == "current", s.status_line()
    s.submit("/exit")
    s.wait_exit()

    assert ctx.state()["model"] == "current", ctx.state()
    assert not (d / "model").exists()


# ---- legacy credentials ----------------------------------------------------

def legacy_credentials(ctx, text):
    c = ctx.home / ".local" / "state" / "yoke" / "credentials"
    c.parent.mkdir(parents=True, exist_ok=True)
    c.write_text(text)
    c.chmod(0o600)
    return c


def test_json_lines_credentials_are_migrated_to_sections(ctx):
    """Keys written before the settings formats were unified stay reachable.

    The old file was JSON Lines. The ini parser reads no keys from one, so
    without a migration its secrets are invisible: the app cannot use them
    and deleting the provider cannot remove them.
    """
    c = legacy_credentials(ctx, '{"name":"work","key":"sk-legacy"}\n')
    ctx.write_config(f"[provider work]\nbase_url = {ctx.mock.base_url}\n"
                     f"model = mock-model\n")
    (ctx.home / ".local" / "state" / "yoke" / "state").write_text("provider = work\n")
    ctx.scenario("text=ok")

    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.auth[-1] == "Bearer sk-legacy", ctx.mock.auth
    assert ctx.settings(c).get("provider work", {}).get("key") == "sk-legacy"
    assert "{" not in c.read_text(), c.read_text()


def test_a_key_left_by_a_deleted_provider_becomes_visible(ctx):
    """An orphaned legacy key stops hiding as an unparseable line.

    Every rewrite copied it forward verbatim, so it survived the deletion of
    the provider it belonged to and could never be removed through the app.
    Converting it makes it a section like any other, which is what both a
    person reading the file and a later delete need.
    """
    c = legacy_credentials(
        ctx,
        '{"name":"gone","key":"sk-orphan"}\n'
        '{"name":"work","key":"sk-work"}\n')
    ctx.write_config(f"[provider work]\nbase_url = {ctx.mock.base_url}\n"
                     f"model = mock-model\n")
    (ctx.home / ".local" / "state" / "yoke" / "state").write_text(
        "provider = work\n")

    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.settle()
    sections = ctx.settings(c)
    assert sections.get("provider gone", {}).get("key") == "sk-orphan", sections
    assert sections.get("provider work", {}).get("key") == "sk-work", sections
    assert "{" not in c.read_text(), c.read_text()
