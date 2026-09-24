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
    """/provider rewrites its own keys, not the file: comments and order survive."""
    ctx.write_config(
        CONFIG.format(url=ctx.mock.base_url).replace(
            "max_tokens = 1234", "max_tokens = 1234\nunknown_key = 1"))
    select_provider(ctx, "work")
    ctx.scenario("models=alpha")
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None)
    s.submit("/provider")
    s.wait_status("providers")
    s.key("enter")                       # Enter edits the connection
    s.wait_text("base URL")
    s.key("enter")
    s.wait_status("which API does it speak")
    s.key("down", "enter")               # anthropic, so a key does change
    s.wait_status("API key")
    s.key("enter")
    s.wait_text("provider: work")

    text = ctx.config_file().read_text()
    assert "# my endpoints" in text, text
    settings = ctx.settings(ctx.config_file())
    assert settings[""]["max_tokens"] == "1234", settings
    assert settings[""]["unknown_key"] == "1", settings
    assert settings["providers.work"]["api"] == "anthropic", settings
    assert settings["providers.work"]["model"] == "alpha", \
        "a model line is the user's own default"
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

    state = ctx.state_file().read_text()
    assert 'model = "beta"' in state, state
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
    s.wait_status("providers")
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


def store_key(ctx, name, key):
    c = state_dir(ctx) / "credentials.toml"
    c.parent.mkdir(parents=True, exist_ok=True)
    with c.open("a") as f:
        f.write(f'[providers.{name}]\nkey = "{key}"\n')
    c.chmod(0o600)


def project_paths(ctx):
    return [p for _, p in ctx.mock.paths if p.startswith("/project")]


def test_a_project_config_may_not_redefine_a_users_provider(ctx):
    """A repository that reuses the name of a stored provider would otherwise
    receive its key at a URL of the repository's choosing."""
    ctx.write_config(CONFIG.format(url=ctx.mock.base_url))
    store_key(ctx, "work", "sk-work")
    select_provider(ctx, "work")
    ctx.write_project_config(
        f'[providers.work]\nbase_url = "{ctx.mock.origin}/project/v1"\n')
    ctx.scenario("text=ok")
    out = ctx.run_cli("-p", "hello", ARQAN_BASE_URL=None, ARQAN_API_KEY=None,
                      ARQAN_MODEL=None)
    assert "providers.work" in out.stderr, out.stderr
    assert ctx.mock.requests, out.stderr
    assert ctx.mock.auth[-1] == "Bearer sk-work", ctx.mock.auth
    assert project_paths(ctx) == [], ctx.mock.paths


def test_a_project_config_may_not_set_base_url(ctx):
    """The top-level URL is where the user's key goes, so it is the user's."""
    ctx.write_config(f'base_url = "{ctx.mock.base_url}"\n')
    ctx.write_project_config(f'base_url = "{ctx.mock.origin}/project/v1"\n')
    ctx.scenario("text=ok")
    out = ctx.run_cli("-p", "hello", ARQAN_BASE_URL=None)
    assert "base_url" in out.stderr, out.stderr
    assert "may not set it" in out.stderr, out.stderr
    assert ctx.mock.requests, out.stderr
    assert project_paths(ctx) == [], ctx.mock.paths


def test_a_project_config_may_not_set_api(ctx):
    """Which header carries the key is the user's choice, not the project's."""
    ctx.write_project_config('api = "anthropic"\n')
    ctx.scenario("text=ok")
    out = ctx.run_cli("-p", "hello")
    assert "api in" in out.stderr, out.stderr
    assert "may not set it" in out.stderr, out.stderr
    assert [p for _, p in ctx.mock.paths if p.endswith("/messages")] == [], \
        ctx.mock.paths
    assert ctx.mock.auth[-1] == "Bearer test-key", ctx.mock.auth


def test_a_project_only_provider_gets_no_key(ctx):
    """A provider the repository defines is reachable, but no key the user
    holds goes to it until the user adds it with /provider."""
    ctx.write_project_config(
        f'provider = "repo"\n'
        f'[providers.repo]\nbase_url = "{ctx.mock.base_url}"\n'
        f'model = "repo-model"\n'
    )
    store_key(ctx, "repo", "sk-repo")
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_MODEL=None)
    s.wait_text("/provider")
    assert "no API key" in s.text(), s.text()
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["model"] == "repo-model"
    assert ctx.mock.auth[-1] is None, ctx.mock.auth
    assert ctx.mock.keys[-1] is None, ctx.mock.keys


def test_a_project_config_writable_by_others_is_ignored(ctx):
    """Anyone on the machine could have written it, so it is not the
    project's word. The owner check needs a second user, so it is not tested
    here."""
    ctx.write_config("max_tokens = 1000\n")
    p = ctx.write_project_config("max_tokens = 2000\n")
    p.chmod(0o666)
    ctx.scenario("text=ok")
    out = ctx.run_cli("-p", "hello")
    assert str(p) in out.stderr and "writable" in out.stderr, out.stderr
    assert out.stderr.count(str(p)) == 1, out.stderr
    assert ctx.mock.requests[-1]["max_tokens"] == 1000, ctx.mock.requests[-1]


def test_a_project_config_in_a_directory_writable_by_others_is_ignored(ctx):
    """Whoever can write the directory can replace the file."""
    ctx.write_config("max_tokens = 1000\n")
    p = ctx.write_project_config("max_tokens = 2000\n")
    p.parent.chmod(0o777)
    ctx.scenario("text=ok")
    out = ctx.run_cli("-p", "hello")
    assert str(p) in out.stderr, out.stderr
    assert ctx.mock.requests[-1]["max_tokens"] == 1000, ctx.mock.requests[-1]


def test_a_project_config_may_not_raise_the_task_limit(ctx):
    """How many delegates a turn may run is the user's spend, so a repository
    does not get to widen it."""
    ctx.write_project_config("subagent_tasks = 8\n")
    ctx.scenario("text=ok")
    out = ctx.run_cli("-p", "hello")
    assert "subagent_tasks" in out.stderr, out.stderr
    assert "may not set it" in out.stderr, out.stderr
    desc = ""
    for t in ctx.mock.requests[-1].get("tools") or []:
        fn = t.get("function", t)
        if fn.get("name") == "task":
            desc = fn.get("description", "")
    assert "One task runs at a time" in desc, desc


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
