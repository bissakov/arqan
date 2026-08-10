"""The /provider command: user-defined endpoints, in either API."""

import stat


def credentials_file(ctx):
    return ctx.home / ".local" / "state" / "yoke" / "credentials.toml"


def store(ctx):
    """The [provider ...] sections of the config file, in file order."""
    out = []
    for section, keys in ctx.settings(ctx.config_file()).items():
        if not section.startswith("providers."):
            continue
        out.append({"name": section[len("providers."):], **keys})
    return out


def creds(ctx):
    """The keys the credentials file holds, by provider.

    A section with no `key` line holds no key: that is what clearing one
    leaves behind, and it is not the same as a key whose value is empty.
    """
    return {
        section[len("providers."):]: keys["key"]
        for section, keys in ctx.settings(credentials_file(ctx)).items()
        if section.startswith("providers.") and keys.get("key")
    }


def active(ctx):
    return ctx.state().get("provider")


def write_provider(ctx, name, base_url, model="mock-model", key="stored-key",
                   api=None):
    """Seed a stored provider, the way an earlier session would have left it."""
    p = ctx.config_file()
    p.parent.mkdir(parents=True, exist_ok=True)
    with p.open("a") as f:
        f.write(f"[providers.{name}]\nbase_url = {base_url}\nmodel = {model}\n")
        if api:
            f.write(f"api = {api}\n")
    if key is not None:
        c = credentials_file(ctx)
        c.parent.mkdir(parents=True, exist_ok=True)
        with c.open("a") as f:
            f.write(f"[providers.{name}]\nkey = {key}\n")
        c.chmod(0o600)


def select_provider(ctx, name):
    p = ctx.state_file()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(f"provider = {name}\n")


def add_provider(s, ctx, name, url=None, key="sk-secret", api="openai",
                 reasoning_efforts="", thinking_budgets="",
                 reasoning_effort="", thinking_budget="",
                 reasoning_template="", submit_key=True):
    """Drive the creation form to the model picker.

    A key is followed by the store to keep it in; the default answer is the
    credentials file. `submit_key=False` stops with the key typed but not
    entered, which is where the case about the echo looks.
    """
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
    for question, value in (
        ("reasoning efforts", reasoning_efforts),
        ("thinking budgets", thinking_budgets),
        ("active reasoning effort", reasoning_effort),
        ("active thinking budget", thinking_budget),
        ("reasoning JSON template", reasoning_template),
    ):
        s.wait_text(question)
        if value:
            s.type(value).sync()
        s.key("enter")
    s.wait_text("its API key")
    if key:
        s.type(key).sync()
    if not submit_key:
        return s
    s.key("enter")
    if key:
        s.wait_status("where should the key be kept")
        s.key("enter")
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


def test_creating_a_provider_includes_reasoning_options(ctx):
    """Add exposes and stores every optional control that edit exposes."""
    ctx.scenario("models=alpha")
    s = ctx.spawn(cols=160)
    s.submit("/provider")
    add_provider(
        s, ctx, "work", reasoning_efforts="low,xhigh",
        thinking_budgets="1024,2048", reasoning_effort="xhigh",
        thinking_budget="1024",
        reasoning_template='{"vendor":"$reasoning_effort"}',
    )
    s.wait_status("pick a model")
    s.key("enter")
    s.wait_text("provider: work")

    assert store(ctx) == [{
        "name": "work", "base_url": ctx.mock.base_url, "model": "alpha",
        "api": "openai", "reasoning_efforts": "low,xhigh",
        "thinking_budgets": "1024,2048", "reasoning_effort": "xhigh",
        "thinking_budget": "1024",
        "reasoning_template": '{"vendor":"$reasoning_effort"}',
    }], store(ctx)
    assert s.status_field(2) == "xhigh", s.status_line()
    assert s.status_field(3) == "thinking 1024", s.status_line()
    assert s.status_field(4) == "build", s.status_line()


def test_a_provider_can_be_deleted_from_the_tui(ctx):
    """Deletion removes its public settings and its stored credential."""
    write_provider(ctx, "work", ctx.mock.base_url, key="sk-work")
    write_provider(ctx, "home", ctx.mock.base_url, key="sk-home")
    select_provider(ctx, "work")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.submit("/provider")
    s.wait_status("pick a provider")
    s.key(*(["down"] * 4), "enter")
    s.wait_status("delete a provider")
    s.key("down", "enter")
    s.wait_status("delete provider home?")
    s.key("down", "enter")
    s.wait_text("deleted provider: home")

    assert [p["name"] for p in store(ctx)] == ["work"], store(ctx)
    assert creds(ctx) == {"work": "sk-work"}, creds(ctx)
    assert active(ctx) == "work", ctx.state()


def test_deleting_the_active_provider_leaves_no_stale_selection(ctx):
    """The removed current provider is forgotten and cannot receive a turn."""
    write_provider(ctx, "work", ctx.mock.base_url, key="sk-work")
    select_provider(ctx, "work")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.submit("/provider")
    s.wait_status("pick a provider")
    s.key("down", "down", "down", "enter")
    s.wait_status("delete a provider")
    s.key("enter")
    s.wait_status("delete provider work?")
    s.key("down", "enter")
    s.wait_text("deleted provider: work")

    assert store(ctx) == [], store(ctx)
    assert creds(ctx) == {}, creds(ctx)
    assert active(ctx) is None, ctx.state()
    assert s.status_kind() == "setup", s.status_line()
    assert "mock-model" not in s.status_line(), s.status_line()
    assert "local" not in s.status_line(), s.status_line()
    s.submit("hello")
    s.wait_text("no provider yet")
    assert ctx.mock.requests == [], ctx.mock.requests


def test_the_key_never_lands_in_the_config_directory(ctx):
    """The settings are shareable; the secret is not, so they live apart."""
    ctx.scenario("models=alpha")
    s = ctx.spawn()
    s.submit("/provider")
    add_provider(s, ctx, "work", key="sk-topsecret")
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
    add_provider(s, ctx, "work", key="sk-visible", submit_key=False)
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


def test_provider_creation_allows_an_unverified_manual_model(ctx):
    """A provider without /models support can still be configured explicitly."""
    ctx.scenario("models_status=401")
    s = ctx.spawn()
    s.submit("/provider")
    add_provider(s, ctx, "work")
    s.wait_text("models: HTTP 401; enter a model manually")
    s.type("manual-model").sync()
    s.key("enter")
    s.wait_text("model entered manually; not verified")
    assert store(ctx)[0]["model"] == "manual-model", store(ctx)
    assert creds(ctx) == {"work": "sk-secret"}, creds(ctx)


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


def test_switched_provider_survives_clearing_the_conversation(ctx):
    """Clearing message storage must not reclaim the active connection."""
    write_provider(ctx, "work", ctx.mock.base_url, model="alpha", key="sk-work")
    write_provider(ctx, "home", ctx.mock.base_url, model="beta", key="sk-home")
    select_provider(ctx, "work")
    ctx.scenario("text=configuration+survived")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)

    s.submit("/provider")
    s.wait_status("pick a provider")
    s.key("down", "enter")
    s.wait_text("provider: home")
    s.submit("/clear")
    s.submit("overwrite reclaimed persistent storage with this prompt")
    s.wait_text("configuration survived")
    s.wait_turn_done()

    assert ctx.mock.requests[-1]["model"] == "beta", ctx.mock.requests[-1]
    assert ctx.mock.auth[-1] == "Bearer sk-home", ctx.mock.auth[-1]
    assert s.status_field(1) == "beta", s.status_line()
    assert s.status_field(3) == "home", s.status_line()


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


def test_provider_reasoning_controls_shape_requests(ctx):
    """Selections follow their provider and use each native API's field."""
    write_provider(ctx, "open", ctx.mock.base_url, key="sk", api="openai")
    with ctx.config_file().open("a") as f:
        f.write("reasoning_efforts = tiny,careful\nreasoning_effort = careful\n")
    select_provider(ctx, "open")
    ctx.scenario("text=ok")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["reasoning_effort"] == "careful"


def test_provider_reasoning_controls_are_editable_in_the_tui(ctx):
    """Editing keeps defaults and the key while optional controls may stay Off."""
    write_provider(ctx, "work", ctx.mock.base_url, key="sk-kept", api="openai")
    select_provider(ctx, "work")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.submit("/provider")
    s.wait_status("pick a provider")
    s.key("down", "down", "enter")
    s.wait_status("edit a provider")
    s.key("enter")

    s.wait_text("base URL  (Esc cancels)")
    assert s.composer_text() == ctx.mock.base_url, s.composer_text()
    s.key("enter").sync()
    assert s.composer_text() == "mock-model", s.composer_text()
    s.key("enter").sync()
    s.type("low,high").key("enter").sync()
    s.key("enter").sync()          # thinking budgets stay Off
    s.type("high").key("enter").sync()
    s.key("enter").sync()          # thinking budget stays Off
    s.key("enter").sync()          # template stays Off
    s.wait_status("which API does it speak")
    s.key("enter")
    s.wait_status("API key")
    s.key("enter")                  # keep the credential already stored
    s.wait_text("provider: work")

    assert store(ctx) == [{
        "name": "work", "base_url": ctx.mock.base_url,
        "model": "mock-model", "api": "openai",
        "reasoning_efforts": "low,high", "reasoning_effort": "high",
    }], store(ctx)
    assert creds(ctx) == {"work": "sk-kept"}, creds(ctx)


def test_provider_editor_clears_values_and_keeps_a_long_template(ctx):
    """Empty is distinct from cancel, and templates are not clipped to 1 KiB."""
    template = '{"padding":"' + "x" * 1200 + '"}'
    write_provider(ctx, "work", ctx.mock.base_url, key="sk", api="openai")
    with ctx.config_file().open("a") as f:
        f.write("reasoning_efforts = low,high\nreasoning_effort = high\n"
                f"reasoning_template = {template}\n")
    select_provider(ctx, "work")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.submit("/provider")
    s.wait_status("pick a provider")
    s.key("down", "down", "enter")
    s.wait_status("edit a provider")
    s.key("enter")
    s.wait_text("base URL  (Esc cancels)")

    s.key("enter", "enter", "enter", "enter", "ctrl-u", "enter",
          "enter", "enter")
    s.wait_status("which API does it speak")
    s.key("enter")
    s.wait_status("API key")
    s.key("enter")
    s.wait_text("provider: work")

    saved = store(ctx)[0]
    assert "reasoning_effort" not in saved, saved
    assert saved["reasoning_template"] == template, len(saved["reasoning_template"])


def test_provider_editor_can_clear_the_stored_key(ctx):
    """Clearing a credential is explicit and does not overload an empty answer."""
    write_provider(ctx, "work", ctx.mock.base_url, key="sk-remove", api="openai")
    select_provider(ctx, "work")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.submit("/provider")
    s.wait_status("pick a provider")
    s.key("down", "down", "enter")
    s.wait_status("edit a provider")
    s.key("enter")
    s.wait_text("base URL  (Esc cancels)")
    s.key(*(["enter"] * 7))
    s.wait_status("which API does it speak")
    s.key("enter")
    s.wait_status("API key")
    s.key("down", "down", "down", "enter")   # Keep, Replace, Move, Clear
    s.wait_text("provider: work")

    section = ctx.settings(credentials_file(ctx)).get("providers.work", {})
    assert "key" not in section, section


def test_provider_reasoning_template_substitutes_a_number(ctx):
    """Templates are structured JSON, so a budget placeholder is a number."""
    write_provider(ctx, "custom", ctx.mock.base_url, key="sk", api="openai")
    with ctx.config_file().open("a") as f:
        f.write("thinking_budgets = 17\nthinking_budget = 17\n"
                "reasoning_template = {\"vendor_budget\":\"$thinking_budget\",\"static\":true}\n")
    select_provider(ctx, "custom")
    ctx.scenario("text=ok")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.submit("hello")
    s.wait_turn_done()
    body = ctx.mock.requests[-1]
    assert body["vendor_budget"] == 17 and body["static"] is True, body


def test_invalid_reasoning_template_never_reaches_the_provider(ctx):
    """Bad config is answered locally before an ambiguous request is sent."""
    write_provider(ctx, "bad", ctx.mock.base_url, key="sk")
    with ctx.config_file().open("a") as f:
        f.write("reasoning_template = [not an object]\n")
    select_provider(ctx, "bad")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.submit("hello")
    s.wait_text("reasoning template must be a JSON object")
    assert ctx.mock.requests == [], ctx.mock.requests


def test_reasoning_template_rejects_trailing_garbage(ctx):
    """A valid JSON prefix does not make the whole template valid JSON."""
    write_provider(ctx, "bad", ctx.mock.base_url, key="sk")
    with ctx.config_file().open("a") as f:
        f.write('reasoning_template = {"vendor":1} trailing\n')
    select_provider(ctx, "bad")
    s = ctx.spawn(YOKE_BASE_URL=None, YOKE_API_KEY=None, YOKE_MODEL=None)
    s.submit("hello")
    s.wait_text("reasoning template must be a JSON object")
    assert ctx.mock.requests == [], ctx.mock.requests


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
    assert s.status_kind() == "setup", s.status_line()
    assert "gpt-4o-mini" not in s.status_line(), s.status_line()
    assert "api.openai.com" not in s.status_line(), s.status_line()
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


def test_deleting_removes_the_key_even_when_the_config_cannot_be_written(ctx):
    """The secret goes first, and no failure elsewhere may strand it.

    Removing an entry touches two files. The config one holds nothing
    sensitive, so its failure must not leave the key on disk: a provider the
    user deleted is a key they expect gone.
    """
    write_provider(ctx, "work", ctx.mock.base_url, model="alpha", key="sk-work")
    config_dir = ctx.config_file().parent
    mode = config_dir.stat().st_mode
    config_dir.chmod(0o555)          # no new file, so the rewrite cannot land
    try:
        s = ctx.spawn()
        s.submit("/provider")
        s.wait_status("pick a provider")
        s.key("down", "down", "down", "enter")
        s.wait_status("delete a provider")
        s.key("enter")
        s.wait_status("delete provider work?")
        s.key("down", "enter")
        s.wait_text("could not")
        assert creds(ctx) == {}, creds(ctx)
    finally:
        config_dir.chmod(mode)


def test_a_new_provider_never_inherits_a_leftover_key(ctx):
    """A name reused after a failed delete starts with no key of its own.

    The credential is keyed by name, so one left behind would be picked up
    silently by the next provider to take that name, and the run would
    authenticate with a secret its user never entered.
    """
    ctx.scenario("models=alpha")
    c = credentials_file(ctx)
    c.parent.mkdir(parents=True, exist_ok=True)
    c.write_text("[providers.work]\nkey = sk-leftover\n")
    c.chmod(0o600)

    s = ctx.spawn()
    s.submit("/provider")
    add_provider(s, ctx, "work", key="")     # this endpoint needs no key
    s.wait_status("pick a model")
    s.key("enter")
    s.wait_text("provider: work")
    assert creds(ctx) == {}, creds(ctx)
