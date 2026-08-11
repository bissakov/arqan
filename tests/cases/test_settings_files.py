"""The settings files: a global config, a project config, and arqan's state.

Settings are one table read from several files. The global config is the
user's document, a project's `.arqan/config.toml` overrides it for the tree it
sits in, and the state file is what the UI remembers. Providers are sections
of the config files rather than a store of their own, so a write by the UI has
to leave the rest of the document exactly as its owner wrote it.
"""

CONFIG = """\
# my endpoints
max_tokens = 1234

[providers.work]
base_url = "{url}"
model = "alpha"
"""


def state_dir(ctx):
    return ctx.home / ".local" / "state" / "arqan"


def select_provider(ctx, name):
    p = ctx.state_file()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(f'provider = "{name}"\n')


def test_a_hand_written_provider_section_configures_the_run(ctx):
    """A provider is a section of the config file, editable like any key."""
    ctx.write_config(CONFIG.format(url=ctx.mock.base_url))
    select_provider(ctx, "work")
    ctx.scenario("text=hi")
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None)
    assert s.status_field(1) == "alpha", s.status_line()
    assert s.status_field(3) == "work", s.status_line()
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["model"] == "alpha"


def test_writing_a_provider_keeps_the_rest_of_the_config(ctx):
    """/model rewrites one key, not the file: comments and order survive."""
    ctx.write_config(
        CONFIG.format(url=ctx.mock.base_url).replace(
            "max_tokens = 1234", "max_tokens = 1234\nunknown_key = 1"))
    select_provider(ctx, "work")
    ctx.scenario("models=alpha|beta")
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None)
    s.submit("/model")
    s.wait_status("pick a model")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("model: beta")

    text = ctx.config_file().read_text()
    assert "# my endpoints" in text, text
    settings = ctx.settings(ctx.config_file())
    assert settings[""]["max_tokens"] == "1234", settings
    assert settings[""]["unknown_key"] == "1", settings
    assert settings["providers.work"]["model"] == "beta", settings
    assert settings["providers.work"]["base_url"] == ctx.mock.base_url, settings


def test_what_arqan_writes_is_toml(ctx):
    """The format is a TOML subset, so what arqan writes a TOML reader parses.

    A string is quoted and a number is bare; anything else would be a file
    arqan could read back and an editor could not.
    """
    ctx.write_config(CONFIG.format(url=ctx.mock.base_url))
    select_provider(ctx, "work")
    ctx.scenario("models=alpha|beta")
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None)
    s.submit("/model")
    s.wait_status("pick a model")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("model: beta")
    s.settings_toggle("Verbose tool output")
    s.submit("/exit")
    s.wait_exit()

    assert 'model = "beta"' in ctx.config_file().read_text()
    state = ctx.state_file().read_text()
    assert 'provider = "work"' in state, state
    assert "verbose_tools = true" in state, state


def test_a_quoted_value_keeps_what_is_inside_it(ctx):
    """Quotes delimit the value; a trailing comment is not part of it."""
    ctx.write_config(
        f'model = "spaced model"   # the one this project uses\n'
        f'base_url = "{ctx.mock.base_url}"\n'
    )
    s = ctx.spawn(ARQAN_MODEL=None, ARQAN_BASE_URL=None)
    assert s.status_field(1) == "spaced model", s.status_line()


def test_a_provider_in_the_config_dirs_is_offered(ctx):
    """The system config is searched for providers as it is for keys."""
    etc = ctx.tmp / "etc"
    (etc / "arqan").mkdir(parents=True)
    (etc / "arqan" / "config.toml").write_text(
        f'[providers.sitewide]\nbase_url = "{ctx.mock.base_url}"\n'
    )
    s = ctx.spawn(XDG_CONFIG_DIRS=str(etc))
    s.submit("/provider")
    s.wait_status("pick a provider")
    assert "sitewide" in s.text(), s.text()
    s.key("esc")


def test_every_remembered_choice_lands_in_one_state_file(ctx):
    """Model, provider and telemetry share the state file and own no others."""
    ctx.scenario("models=alpha|beta")
    s = ctx.spawn(ARQAN_MODEL=None)
    s.settings_toggle("Telemetry")
    s.submit("/model")
    s.wait_status("pick a model")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("model: beta")
    s.submit("/exit")
    s.wait_exit()

    assert ctx.state() == {"model": "beta", "telemetry": "true"}, ctx.state()
    left = {p.name for p in state_dir(ctx).iterdir()}
    assert left == {"state.toml", "history", "telemetry"}, left


# ---- project settings ------------------------------------------------------

def test_a_project_config_overrides_the_global_one(ctx):
    """`.arqan/config.toml` is the more local statement, so it is the answer."""
    ctx.write_config("max_tokens = 1000\nmodel = \"global-model\"\n")
    ctx.write_project_config("max_tokens = 2000\n")
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_MODEL=None)
    assert s.status_field(1) == "global-model", s.status_line()
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["max_tokens"] == 2000, ctx.mock.requests[-1]


def test_the_nearest_project_config_wins(ctx):
    """The chain is walked to the root, and the nearest file has the say."""
    ctx.write_project_config("max_tokens = 2000\n")
    inner = ctx.work / "sub"
    inner.mkdir()
    ctx.write_project_config("max_tokens = 3000\n", at=inner)
    ctx.scenario("text=ok")
    s = ctx.spawn(cwd=str(inner))
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["max_tokens"] == 3000, ctx.mock.requests[-1]


def test_a_project_config_may_not_carry_an_api_key(ctx):
    """It arrives with a clone, so it does not get to authenticate anyone."""
    ctx.write_project_config('api_key = "sk-from-the-repo"\n')
    ctx.scenario("text=ok")
    out = ctx.run_cli("-p", "hello", ARQAN_API_KEY=None)
    assert "api_key" in out.stderr, out.stderr
    assert "may not set it" in out.stderr, out.stderr
    assert ctx.mock.auth[-1] != "Bearer sk-from-the-repo", ctx.mock.auth


def test_a_project_config_may_define_a_provider(ctx):
    """An endpoint is a URL, not a secret, so a repository may name one."""
    ctx.write_project_config(
        f'provider = "repo"\n'
        f'[providers.repo]\nbase_url = "{ctx.mock.base_url}"\n'
        f'model = "repo-model"\n'
    )
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None)
    assert s.status_field(1) == "repo-model", s.status_line()
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["model"] == "repo-model"


# ---- bad input -------------------------------------------------------------

def test_an_unknown_key_is_reported(ctx):
    """A typo in a document is worth saying out loud rather than ignoring."""
    ctx.write_config("moddel = gpt-4o-mini\n")
    ctx.scenario("text=ok")
    out = ctx.run_cli("-p", "hello")
    assert "unknown setting moddel" in out.stderr, out.stderr


def test_a_value_outside_its_bounds_falls_through(ctx):
    """A refused value must not shadow the good one below it."""
    ctx.write_config("max_tokens = 4096\n")
    ctx.write_project_config("max_tokens = 99999999\n")
    ctx.scenario("text=ok")
    out = ctx.run_cli("-p", "hello")
    assert "max_tokens" in out.stderr, out.stderr
    assert ctx.mock.requests[-1]["max_tokens"] == 4096, ctx.mock.requests[-1]


def test_a_provider_naming_nothing_is_not_a_selection(ctx):
    """A name with no section behind it leaves the run asking for one."""
    ctx.write_config('provider = "ghost"\n')
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None)
    s.wait_text("no provider yet")
