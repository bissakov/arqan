"""The /provider command: user-defined OpenAI-compatible endpoints."""

import json
import stat


def providers_file(ctx):
    return ctx.xdg / "yoke" / "providers"


def credentials_file(ctx):
    return ctx.home / ".local" / "state" / "yoke" / "credentials"


def active_file(ctx):
    return ctx.home / ".local" / "state" / "yoke" / "provider"


def store(ctx):
    path = providers_file(ctx)
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line]


def creds(ctx):
    path = credentials_file(ctx)
    if not path.exists():
        return {}
    return {
        e["name"]: e["key"]
        for e in (json.loads(l) for l in path.read_text().splitlines() if l)
    }


def write_provider(ctx, name, base_url, model="mock-model", key="stored-key"):
    """Seed a stored provider, the way an earlier session would have left it."""
    p = providers_file(ctx)
    p.parent.mkdir(parents=True, exist_ok=True)
    line = json.dumps({"name": name, "base_url": base_url, "model": model})
    with p.open("a") as f:
        f.write(line + "\n")
    if key is not None:
        c = credentials_file(ctx)
        c.parent.mkdir(parents=True, exist_ok=True)
        with c.open("a") as f:
            f.write(json.dumps({"name": name, "key": key}) + "\n")
        c.chmod(0o600)


def select_provider(ctx, name):
    active_file(ctx).parent.mkdir(parents=True, exist_ok=True)
    active_file(ctx).write_text(name + "\n")


def add_provider(s, ctx, name, url=None, key="sk-secret"):
    """Drive the creation form to the model picker."""
    s.wait_text("a name for this provider")
    s.type(name).sync()
    s.key("enter")
    s.wait_text("its base URL")
    s.type(url or ctx.mock.base_url).sync()
    s.key("enter")
    if key is None:      # the URL was meant to be refused
        return s
    s.wait_text("its API key")
    s.type(key).sync()
    return s


def test_the_form_opens_when_nothing_is_stored(ctx):
    """With no provider yet there is nothing to pick from, so /provider asks."""
    s = ctx.spawn()
    s.submit("/provider")
    s.wait_text("a name for this provider")
    assert "pick a provider" not in s.text(), s.text()
    ctx.check_screen(s)


def test_creating_a_provider_stores_it_and_switches_to_it(ctx):
    """The form ends on the model picker and the entry lands in both files."""
    ctx.scenario("models=alpha|beta")
    s = ctx.spawn()
    s.submit("/provider")
    add_provider(s, ctx, "work")
    s.key("enter")
    s.wait_status("pick a model")
    s.key("enter")
    s.wait_text("provider: work")
    assert s.status_field(1) == "alpha", s.status_line()
    assert s.status_field(2) == "work", s.status_line()

    assert store(ctx) == [
        {"name": "work", "base_url": ctx.mock.base_url, "model": "alpha"}
    ], store(ctx)
    assert creds(ctx) == {"work": "sk-secret"}, creds(ctx)
    assert active_file(ctx).read_text().strip() == "work"


def test_the_key_never_lands_in_the_config_directory(ctx):
    """The settings are shareable; the secret is not, so they live apart."""
    ctx.scenario("models=alpha")
    s = ctx.spawn()
    s.submit("/provider")
    add_provider(s, ctx, "work", key="sk-topsecret")
    s.key("enter")
    s.wait_status("pick a model")
    s.key("enter")
    s.wait_text("provider: work")

    for path in ctx.xdg.rglob("*"):
        if path.is_file():
            assert "sk-topsecret" not in path.read_text(), path
    mode = stat.S_IMODE(credentials_file(ctx).stat().st_mode)
    assert mode == 0o600, oct(mode)


def test_the_key_is_not_echoed(ctx):
    """A key is typed in front of whoever is looking at the screen."""
    ctx.scenario("models=alpha")
    s = ctx.spawn()
    s.submit("/provider")
    add_provider(s, ctx, "work", key="sk-visible")
    text = s.text()
    assert "sk-visible" not in text, text
    assert "**********" in text, text
    ctx.check_screen(s)


def test_escape_cancels_the_form_and_writes_nothing(ctx):
    """A cancelled form leaves the configuration exactly as it was."""
    s = ctx.spawn()
    s.submit("/provider")
    s.wait_text("a name for this provider")
    s.key("esc")
    s.wait_gone("a name for this provider")
    assert not providers_file(ctx).exists(), store(ctx)
    assert s.composer_text() == "", s.composer_lines()


def test_a_url_without_a_scheme_is_refused(ctx):
    """A base URL is a URL, and saying so beats a failed turn later."""
    s = ctx.spawn()
    s.submit("/provider")
    add_provider(s, ctx, "work", url="api.example.com/v1", key=None)
    s.wait_text("starts with http:// or https://")
    assert not providers_file(ctx).exists(), store(ctx)


def test_an_endpoint_that_lists_nothing_is_not_stored(ctx):
    """Listing the models is the check that the URL and key work."""
    ctx.scenario("models_status=401")
    s = ctx.spawn()
    s.submit("/provider")
    add_provider(s, ctx, "work")
    s.key("enter")
    s.wait_text("models: HTTP 401")
    assert not providers_file(ctx).exists(), store(ctx)
    assert not credentials_file(ctx).exists()


def test_a_stored_provider_configures_the_next_run(ctx):
    """The active entry outranks the config file: URL, model and key."""
    write_provider(ctx, "work", ctx.mock.base_url, model="beta", key="sk-work")
    select_provider(ctx, "work")
    ctx.scenario("text=hi")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    assert s.status_field(1) == "beta", s.status_line()
    assert s.status_field(2) == "work", s.status_line()
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["model"] == "beta"
    assert ctx.mock.auth[-1] == "Bearer sk-work", ctx.mock.auth[-1]


def test_switching_provider_picks_up_its_model_and_key(ctx):
    """Two entries, and the picker moves the whole configuration at once."""
    write_provider(ctx, "work", ctx.mock.base_url, model="alpha", key="sk-work")
    write_provider(ctx, "home", ctx.mock.base_url, model="beta", key="sk-home")
    select_provider(ctx, "work")
    ctx.scenario("text=hi")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.submit("/provider")
    s.wait_status("pick a provider")
    assert "current" in s.text(), s.text()
    s.key("down").sync()
    s.key("enter")
    s.wait_text("provider: home")
    assert s.status_field(1) == "beta", s.status_line()
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.auth[-1] == "Bearer sk-home", ctx.mock.auth[-1]
    assert active_file(ctx).read_text().strip() == "home"


def test_the_model_picker_writes_to_the_active_provider(ctx):
    """A model id belongs to the endpoint that served it, not to the state."""
    write_provider(ctx, "work", ctx.mock.base_url, model="alpha", key="sk-work")
    select_provider(ctx, "work")
    ctx.scenario("models=alpha|beta")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.submit("/model")
    s.wait_status("pick a model")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("model: beta")
    assert store(ctx)[0]["model"] == "beta", store(ctx)
    assert not (ctx.home / ".local" / "state" / "yoke" / "model").exists()


def test_credentials_readable_by_others_are_refused(ctx):
    """A key anyone can read is one to rotate, so it is not loaded silently."""
    write_provider(ctx, "work", ctx.mock.base_url, model="alpha", key="sk-work")
    credentials_file(ctx).chmod(0o644)
    s = ctx.spawn()
    s.submit("/provider")
    s.wait_status("pick a provider")
    s.key("enter")
    s.wait_text("readable by others")
    assert active_file(ctx).exists() is False


def test_the_first_run_without_a_key_says_how_to_add_one(ctx):
    """Nothing to talk to: the welcome screen names the command, and waits."""
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.wait_text("+ add a provider")
    assert "a name for this provider" not in s.text(), s.text()
    ctx.check_screen(s)


def test_a_message_without_a_provider_is_refused(ctx):
    """A turn with no endpoint is a 401 nobody can act on, so it never runs."""
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.wait_text("+ add a provider")
    s.submit("hello")
    s.wait_text("no provider yet")
    assert "provider error" not in s.text(), s.text()
    assert ctx.mock.requests == [], ctx.mock.requests


def test_the_welcome_hint_goes_away_once_a_provider_exists(ctx):
    """The line is a missing endpoint, so naming one retires it."""
    ctx.scenario("models=alpha")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.wait_text("+ add a provider")
    s.submit("/provider")
    add_provider(s, ctx, "work")
    s.key("enter")
    s.wait_status("pick a model")
    s.key("enter")
    s.wait_text("provider: work")
    assert "no provider yet" not in s.text(), s.text()


def test_a_configured_endpoint_starts_a_conversation(ctx):
    """A named endpoint that needs no key is a run, not a question."""
    ctx.scenario("text=no+key+needed")
    s = ctx.spawn(YOKE_API_KEY=None)
    assert "a name for this provider" not in s.text(), s.text()
    s.submit("hello")
    s.wait_text("no key needed")
