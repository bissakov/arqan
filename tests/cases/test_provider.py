"""The /provider command: user-defined endpoints, in either API."""

import stat


def credentials_file(ctx):
    return ctx.home / ".local" / "state" / "yoke" / "credentials"


def store(ctx):
    """The [provider ...] sections of the config file, in file order."""
    out = []
    for section, keys in ctx.settings(ctx.config_file()).items():
        if not section.startswith("provider "):
            continue
        out.append({"name": section[len("provider "):], **keys})
    return out


def creds(ctx):
    return {
        section[len("provider "):]: keys["key"]
        for section, keys in ctx.settings(credentials_file(ctx)).items()
        if section.startswith("provider ")
    }


def active(ctx):
    return ctx.state().get("provider")


def write_provider(ctx, name, base_url, model="mock-model", key="stored-key",
                   api=None):
    """Seed a stored provider, the way an earlier session would have left it."""
    p = ctx.config_file()
    p.parent.mkdir(parents=True, exist_ok=True)
    with p.open("a") as f:
        f.write(f"[provider {name}]\nbase_url = {base_url}\nmodel = {model}\n")
        if api:
            f.write(f"api = {api}\n")
    if key is not None:
        c = credentials_file(ctx)
        c.parent.mkdir(parents=True, exist_ok=True)
        with c.open("a") as f:
            f.write(f"[provider {name}]\nkey = {key}\n")
        c.chmod(0o600)


def select_provider(ctx, name):
    p = ctx.state_file()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(f"provider = {name}\n")


def add_provider(s, ctx, name, url=None, key="sk-secret", api="openai"):
    """Drive the creation form to the model picker."""
    s.wait_text("a name for this provider")
    s.type(name).sync()
    s.key("enter")
    s.wait_status("which API does it speak")
    if api == "anthropic":
        s.key("down").sync()
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
    assert s.status_field(3) == "work", s.status_line()

    assert store(ctx) == [
        {"name": "work", "base_url": ctx.mock.base_url, "model": "alpha",
         "api": "openai"}
    ], store(ctx)
    assert creds(ctx) == {"work": "sk-secret"}, creds(ctx)
    assert active(ctx) == "work", ctx.state()


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
    assert store(ctx) == [], store(ctx)
    assert s.composer_text() == "", s.composer_lines()


def test_a_url_without_a_scheme_is_refused(ctx):
    """A base URL is a URL, and saying so beats a failed turn later."""
    s = ctx.spawn()
    s.submit("/provider")
    add_provider(s, ctx, "work", url="api.example.com/v1", key=None)
    s.wait_text("starts with http:// or https://")
    assert store(ctx) == [], store(ctx)


def test_an_endpoint_that_lists_nothing_is_not_stored(ctx):
    """Listing the models is the check that the URL and key work."""
    ctx.scenario("models_status=401")
    s = ctx.spawn()
    s.submit("/provider")
    add_provider(s, ctx, "work")
    s.key("enter")
    s.wait_text("models: HTTP 401")
    assert store(ctx) == [], store(ctx)
    assert not credentials_file(ctx).exists()


def test_a_stored_provider_configures_the_next_run(ctx):
    """The active entry outranks the config file: URL, model and key."""
    write_provider(ctx, "work", ctx.mock.base_url, model="beta", key="sk-work")
    select_provider(ctx, "work")
    ctx.scenario("text=hi")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    assert s.status_field(1) == "beta", s.status_line()
    assert s.status_field(3) == "work", s.status_line()
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
    assert active(ctx) == "home", ctx.state()


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
    assert "model" not in ctx.state(), ctx.state()


def test_creating_an_anthropic_provider_records_the_api(ctx):
    """The form asks which API a URL speaks, and the answer is stored with it."""
    ctx.scenario("models=claude-a|claude-b,text=hello+from+anthropic")
    s = ctx.spawn()
    s.submit("/provider")
    add_provider(s, ctx, "anth", key="sk-ant", api="anthropic")
    s.key("enter")
    s.wait_status("pick a model")
    s.key("enter")
    s.wait_text("provider: anth")

    assert store(ctx) == [
        {"name": "anth", "base_url": ctx.mock.base_url, "model": "claude-a",
         "api": "anthropic"}
    ], store(ctx)

    s.submit("hello")
    s.wait_text("hello from anthropic")
    s.wait_turn_done()
    # The key rides in Anthropic's own header, and the request names a version.
    assert ctx.mock.keys[-1] == "sk-ant", ctx.mock.keys
    assert ctx.mock.auth[-1] is None, ctx.mock.auth
    assert ctx.mock.versions[-1] == "2023-06-01", ctx.mock.versions


def test_an_anthropic_provider_configures_the_next_run(ctx):
    """A stored `api` key is what the next session speaks, without asking."""
    write_provider(ctx, "anth", ctx.mock.base_url, model="claude-a",
                   key="sk-ant", api="anthropic")
    select_provider(ctx, "anth")
    ctx.scenario("text=answered")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.submit("hello")
    s.wait_text("answered")
    s.wait_turn_done()
    assert ctx.mock.keys[-1] == "sk-ant", ctx.mock.keys
    # The system prompt is a parameter there rather than the first message.
    body = ctx.mock.requests[-1]
    assert "system" in body, body
    assert [m["role"] for m in body["messages"]] == ["user"], body["messages"]


def test_credentials_readable_by_others_are_refused(ctx):
    """A key anyone can read is one to rotate, so it is not loaded silently."""
    write_provider(ctx, "work", ctx.mock.base_url, model="alpha", key="sk-work")
    credentials_file(ctx).chmod(0o644)
    s = ctx.spawn()
    s.submit("/provider")
    s.wait_status("pick a provider")
    s.key("enter")
    s.wait_text("readable by others")
    assert active(ctx) is None, ctx.state()


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
