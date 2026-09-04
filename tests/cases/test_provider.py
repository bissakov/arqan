"""The /provider command: connections, in either API.

A provider is a connection and nothing else: what it serves is chosen in
/model, so these cases are about storing, editing and removing one, and about
what a session does not do when the set of connections changes.
"""

import stat


def credentials_file(ctx):
    return ctx.home / ".local" / "state" / "arqan" / "credentials.toml"


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
    """Seed a stored provider, plus the `model` default of a hand-written one."""
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


def select_provider(ctx, name, model=None):
    """The state pair: which provider serves the chosen model, and which."""
    p = ctx.state_file()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(f"provider = {name}\n"
                 + (f"model = {model}\n" if model else ""))


def first_run(ctx, **env):
    """A session with nothing to talk to: no endpoint from the environment."""
    return ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None,
                     **env)


def add_provider(s, ctx, name, url=None, key="sk-secret", api="openai",
                 submit_key=True):
    """Drive the creation form to its last answer.

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
    assert "+ add a provider" not in s.text(), s.text()
    ctx.check_screen(s)


def test_creating_the_first_provider_ends_on_the_model_picker(ctx):
    """A run with nothing to talk to is set up in one pass: connection, model.

    The connection lands in the config file and its key in the credentials
    file; the model is the state pair, since an id belongs to the endpoint
    that listed it rather than to the section that connects to it.
    """
    ctx.scenario("models=alpha|beta")
    s = first_run(ctx)
    s.submit("/provider")
    add_provider(s, ctx, "work")
    s.wait_status("pick a model")
    s.key("enter")
    s.wait_text("model: alpha @ work")
    assert s.status_field(1) == "alpha", s.status_line()
    assert s.status_field(3) == "work", s.status_line()

    assert store(ctx) == [
        {"name": "work", "base_url": ctx.mock.base_url, "api": "openai"}
    ], store(ctx)
    assert creds(ctx) == {"work": "sk-secret"}, creds(ctx)
    assert active(ctx) == "work", ctx.state()
    assert ctx.state().get("model") == "alpha", ctx.state()


def test_adding_a_provider_leaves_the_session_where_it_is(ctx):
    """A connection is not a choice: the turn still goes where it went."""
    write_provider(ctx, "work", ctx.mock.base_url, model="alpha", key="sk-work")
    select_provider(ctx, "work", model="alpha")
    ctx.scenario("models=alpha|beta,text=hi")
    s = first_run(ctx)
    s.submit("/provider")
    s.key("down").sync()
    assert "add a provider" in s.popup_selected(), s.popup_selected()
    s.key("enter")
    add_provider(s, ctx, "home", key="sk-home")
    s.wait_text("provider: home (2 models)")
    assert "pick a model" not in s.status_line(), s.status_line()
    assert s.status_field(1) == "alpha", s.status_line()
    assert s.status_field(3) == "work", s.status_line()
    assert active(ctx) == "work", ctx.state()

    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.auth[-1] == "Bearer sk-work", ctx.mock.auth


def test_provider_creation_does_not_ask_for_model_capabilities(ctx):
    """Provider setup owns transport; model capabilities are configured later."""
    ctx.scenario("models=alpha")
    s = first_run(ctx, cols=160)
    s.submit("/provider")
    add_provider(s, ctx, "work")
    s.wait_status("pick a model")
    assert "reasoning efforts" not in s.text(), s.text()
    s.key("enter")
    s.wait_text("model: alpha @ work")
    assert store(ctx) == [{
        "name": "work", "base_url": ctx.mock.base_url, "api": "openai",
    }], store(ctx)


def test_a_provider_can_be_deleted_from_the_tui(ctx):
    """Deletion removes its connection, model profiles, and credential."""
    write_provider(ctx, "work", ctx.mock.base_url, key="sk-work")
    write_provider(ctx, "home", ctx.mock.base_url, key="sk-home")
    with ctx.config_file().open("a") as f:
        f.write('[providers.home.models."mock-model"]\n'
                'context_window = 12345\n')
    select_provider(ctx, "work", model="mock-model")
    s = first_run(ctx)
    s.submit("/provider")
    s.wait_status("providers")
    s.key(*(["down"] * 3), "enter")
    s.wait_status("delete a provider")
    s.key("down", "enter")
    s.wait_status("delete provider home?")
    s.key("down", "enter")
    s.wait_text("deleted provider: home")

    assert [p["name"] for p in store(ctx)] == ["work"], store(ctx)
    assert 'providers.home.models."mock-model"' not in \
        ctx.settings(ctx.config_file())
    assert creds(ctx) == {"work": "sk-work"}, creds(ctx)
    assert active(ctx) == "work", ctx.state()
    assert ctx.state().get("model") == "mock-model", ctx.state()


def test_deleting_a_provider_forgets_its_pins(ctx):
    """A pin names an endpoint, so one that is gone leaves no row behind."""
    write_provider(ctx, "work", ctx.mock.base_url, key="sk-work")
    p = ctx.state_file()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("[favorites.work]\nmodels = alpha, beta\n")
    s = first_run(ctx)
    s.submit("/provider")
    s.wait_status("providers")
    s.key("down", "down", "enter")
    s.wait_status("delete a provider")
    s.key("enter")
    s.wait_status("delete provider work?")
    s.key("down", "enter")
    s.wait_text("deleted provider: work")
    assert "favorites.work" not in ctx.settings(ctx.state_file()), \
        ctx.settings(ctx.state_file())


def test_deleting_the_active_provider_leaves_no_stale_selection(ctx):
    """The removed current provider is forgotten and cannot receive a turn."""
    write_provider(ctx, "work", ctx.mock.base_url, key="sk-work")
    select_provider(ctx, "work", model="mock-model")
    s = first_run(ctx)
    s.submit("/provider")
    s.wait_status("providers")
    s.key("down", "down", "enter")
    s.wait_status("delete a provider")
    s.key("enter")
    s.wait_status("delete provider work?")
    s.key("down", "enter")
    s.wait_text("deleted provider: work")

    assert store(ctx) == [], store(ctx)
    assert creds(ctx) == {}, creds(ctx)
    assert active(ctx) is None, ctx.state()
    assert "model" not in ctx.state(), "the pair goes with its endpoint"
    assert s.status_kind() == "setup", s.status_line()
    assert "mock-model" not in s.status_line(), s.status_line()
    assert "local" not in s.status_line(), s.status_line()
    s.submit("hello")
    s.wait_text("no provider yet")
    assert ctx.mock.requests == [], ctx.mock.requests


def test_the_key_never_lands_in_the_config_directory(ctx):
    """The settings are shareable; the secret is not, so they live apart."""
    ctx.scenario("models=alpha")
    s = first_run(ctx)
    s.submit("/provider")
    add_provider(s, ctx, "work", key="sk-topsecret")
    s.wait_status("pick a model")
    s.key("enter")
    s.wait_text("model: alpha @ work")

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


def test_pasting_in_the_provider_form_does_not_trap_input(ctx):
    """The limited editor consumes paste markers and remains cancellable."""
    s = ctx.spawn()
    s.submit("/provider")
    s.wait_text("a name for this provider")
    s.paste("work").sync()
    assert s.composer_text() == "work", s.composer_lines()
    s.key("esc")
    s.wait_gone("a name for this provider")
    assert store(ctx) == [], store(ctx)


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
    s = first_run(ctx)
    s.submit("/provider")
    add_provider(s, ctx, "work")
    s.wait_text("could not be listed")
    s.key("down", "enter")               # store it anyway
    s.wait_text("models: HTTP 401: mock provider error; enter a model "
                "manually")
    s.type("manual-model").sync()
    s.key("enter")
    s.wait_text("entered manually; not verified")
    assert "model" not in store(ctx)[0], store(ctx)
    assert active(ctx) == "work", ctx.state()
    assert ctx.state().get("model") == "manual-model", ctx.state()
    assert creds(ctx) == {"work": "sk-secret"}, creds(ctx)


def test_a_provider_stored_without_a_model_says_which_step_is_left(ctx):
    """Two steps, so the hint names the one that has not been taken."""
    ctx.scenario("models=alpha|beta")
    s = first_run(ctx)
    s.submit("/provider")
    add_provider(s, ctx, "work")
    s.wait_status("pick a model")
    s.key("esc")
    s.wait_text("no model chosen yet")
    s.wait_status("setup")
    assert store(ctx)[0]["name"] == "work", store(ctx)
    assert active(ctx) is None, ctx.state()
    s.wait_text("no model yet")
    assert "no provider yet" not in s.text(), s.text()


def test_a_stored_provider_configures_the_next_run(ctx):
    """The state pair names the endpoint, which brings its URL and key."""
    write_provider(ctx, "work", ctx.mock.base_url, model="alpha", key="sk-work")
    select_provider(ctx, "work", model="beta")
    ctx.scenario("text=hi")
    s = first_run(ctx)
    assert s.status_field(1) == "beta", s.status_line()
    assert s.status_field(3) == "work", s.status_line()
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["model"] == "beta"
    assert ctx.mock.auth[-1] == "Bearer sk-work", ctx.mock.auth[-1]


def test_a_providers_model_line_is_only_a_default(ctx):
    """A hand-written `model` speaks for a run that chose none of its own."""
    write_provider(ctx, "work", ctx.mock.base_url, model="alpha", key="sk-work")
    select_provider(ctx, "work")
    ctx.scenario("text=hi")
    s = first_run(ctx)
    assert s.status_field(1) == "alpha", s.status_line()


def test_choosing_a_model_elsewhere_moves_the_whole_connection(ctx):
    """The pick names an endpoint, so its URL, API and key come with it."""
    write_provider(ctx, "work", ctx.mock.base_url, model="alpha", key="sk-work")
    write_provider(ctx, "home", ctx.mock.base_url, model="beta", key="sk-home")
    select_provider(ctx, "work", model="alpha")
    ctx.scenario("models=alpha|beta,text=hi")
    s = first_run(ctx)
    s.submit("/model")
    s.wait_status("pick a model")
    assert "alpha @ work" in s.popup_selected(), s.popup_selected()
    s.key("down", "down", "down").sync()
    assert "beta @ home" in s.popup_selected(), s.popup_selected()
    s.key("enter")
    s.wait_text("model: beta @ home")
    assert s.status_field(1) == "beta", s.status_line()
    assert s.status_field(3) == "home", s.status_line()
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.auth[-1] == "Bearer sk-home", ctx.mock.auth[-1]
    assert active(ctx) == "home", ctx.state()
    assert ctx.state().get("model") == "beta", ctx.state()


def test_the_chosen_connection_survives_clearing_the_conversation(ctx):
    """Clearing message storage must not reclaim the active connection."""
    write_provider(ctx, "work", ctx.mock.base_url, model="alpha", key="sk-work")
    write_provider(ctx, "home", ctx.mock.base_url, model="beta", key="sk-home")
    select_provider(ctx, "work", model="alpha")
    ctx.scenario("models=alpha|beta,text=configuration+survived")
    s = first_run(ctx)

    s.submit("/model")
    s.wait_status("pick a model")
    s.key("down", "down", "down", "enter")
    s.wait_text("model: beta @ home")
    s.submit("/clear")
    s.submit("overwrite reclaimed persistent storage with this prompt")
    s.wait_text("configuration survived")
    s.wait_turn_done()

    assert ctx.mock.requests[-1]["model"] == "beta", ctx.mock.requests[-1]
    assert ctx.mock.auth[-1] == "Bearer sk-home", ctx.mock.auth[-1]
    assert s.status_field(1) == "beta", s.status_line()
    assert s.status_field(3) == "home", s.status_line()


def test_the_model_picker_writes_the_pair_and_not_the_config(ctx):
    """The choice is a pair in state; the config file keeps what was written."""
    write_provider(ctx, "work", ctx.mock.base_url, model="alpha", key="sk-work")
    select_provider(ctx, "work", model="alpha")
    ctx.scenario("models=alpha|beta")
    s = first_run(ctx)
    s.submit("/model")
    s.wait_status("pick a model")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("model: beta")
    assert store(ctx)[0]["model"] == "alpha", "the default is the user's line"
    assert ctx.state().get("model") == "beta", ctx.state()
    assert active(ctx) == "work", ctx.state()


def test_creating_an_anthropic_provider_records_the_api(ctx):
    """The form asks which API a URL speaks, and the answer is stored with it."""
    ctx.scenario("models=claude-a|claude-b,text=hello+from+anthropic")
    s = first_run(ctx)
    s.submit("/provider")
    add_provider(s, ctx, "anth", key="sk-ant", api="anthropic")
    s.wait_status("pick a model")
    s.key("enter")
    s.wait_text("model: claude-a @ anth")

    assert store(ctx) == [
        {"name": "anth", "base_url": ctx.mock.base_url, "api": "anthropic"}
    ], store(ctx)
    assert ctx.state().get("model") == "claude-a", ctx.state()

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
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None)
    s.submit("hello")
    s.wait_text("answered")
    s.wait_turn_done()
    assert ctx.mock.keys[-1] == "sk-ant", ctx.mock.keys
    # The system prompt is a parameter there rather than the first message.
    body = ctx.mock.requests[-1]
    assert "system" in body, body
    assert [m["role"] for m in body["messages"]] == ["user"], body["messages"]


def test_reasoning_controls_belong_to_the_exact_model(ctx):
    """A provider-wide value is ignored; an exact model profile is applied."""
    write_provider(ctx, "open", ctx.mock.base_url, key="sk", api="openai")
    with ctx.config_file().open("a") as f:
        f.write("reasoning_efforts = wrong\nreasoning_effort = wrong\n"
                '[providers.open.models."mock-model"]\n'
                "reasoning_efforts = tiny,careful\n"
                "reasoning_effort = careful\n")
    select_provider(ctx, "open")
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None)
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["reasoning_effort"] == "careful"


def test_an_unconfigured_model_does_not_inherit_reasoning(ctx):
    """Switching models clears request controls from the model that left."""
    write_provider(ctx, "open", ctx.mock.base_url, model="alpha", key="sk",
                   api="openai")
    with ctx.config_file().open("a") as f:
        f.write('[providers.open.models."alpha"]\n'
                "reasoning_efforts = low,high\nreasoning_effort = high\n")
    select_provider(ctx, "open", model="alpha")
    ctx.scenario("models=alpha|beta,text=ok")
    s = first_run(ctx)
    s.submit("/model")
    s.wait_status("pick a model")
    s.key("down", "enter")
    s.wait_text("model: beta")
    s.submit("hello")
    s.wait_turn_done()
    assert "reasoning_effort" not in ctx.mock.requests[-1]


def test_anthropic_effort_enables_visible_adaptive_thinking(ctx):
    """Anthropic efforts use output_config and request summarized thinking."""
    write_provider(ctx, "anth", ctx.mock.base_url, key="sk", api="anthropic")
    with ctx.config_file().open("a") as f:
        f.write('[providers.anth.models."mock-model"]\n'
                "reasoning_efforts = low,medium,high\nreasoning_effort = medium\n"
                "thinking_budgets = 1024\nthinking_budget = 1024\n")
    select_provider(ctx, "anth")
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None)
    s.submit("hello")
    s.wait_turn_done()
    body = ctx.mock.requests[-1]
    assert body["thinking"] == {"type": "adaptive", "display": "summarized"}
    assert body["output_config"] == {"effort": "medium"}


def test_provider_answers_take_the_composer_editing_keys(ctx):
    """A question borrows the composer, so it answers the same editing keys.

    The keys are exercised on the base URL and then undone, so what the editor
    stores is the URL that was already there.
    """
    write_provider(ctx, "work", ctx.mock.base_url, key="sk-kept", api="openai")
    select_provider(ctx, "work", model="mock-model")
    s = first_run(ctx)
    s.submit("/provider")
    s.wait_status("providers")
    s.key("enter")                             # Enter edits the connection
    s.wait_text("base URL  (Esc cancels)")
    assert s.composer_text() == ctx.mock.base_url, s.composer_text()
    s.key("home").sync()
    s.type("x").sync()
    assert s.composer_text() == "x" + ctx.mock.base_url, s.composer_text()
    s.key("delete").sync()                     # the glyph at the cursor goes
    assert s.composer_text() == "x" + ctx.mock.base_url[1:], s.composer_text()
    s.key("ctrl-k").sync()                     # kill the tail
    assert s.composer_text() == "x", s.composer_text()
    s.key("ctrl-y").sync()                     # and put it back
    assert s.composer_text() == "x" + ctx.mock.base_url[1:], s.composer_text()
    s.key("home").sync()
    s.key("delete").sync()                     # drop the x that was typed
    s.type(ctx.mock.base_url[0]).sync()
    assert s.composer_text() == ctx.mock.base_url, s.composer_text()
    s.key("enter").sync()

    s.wait_status("which API does it speak")
    s.key("enter")
    s.wait_status("API key")
    s.key("enter")                             # keep the credential stored
    s.wait_text("provider: work")

    assert store(ctx) == [{
        "name": "work", "base_url": ctx.mock.base_url, "model": "mock-model",
        "api": "openai",
    }], store(ctx)
    assert creds(ctx) == {"work": "sk-kept"}, creds(ctx)


def test_provider_editor_can_clear_the_stored_key(ctx):
    """Clearing a credential is explicit and does not overload an empty answer."""
    write_provider(ctx, "work", ctx.mock.base_url, key="sk-remove", api="openai")
    select_provider(ctx, "work", model="mock-model")
    s = first_run(ctx)
    s.submit("/provider")
    s.wait_status("providers")
    s.key("enter")
    s.wait_text("base URL  (Esc cancels)")
    s.key("enter")
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
        f.write('[providers.custom.models."mock-model"]\n'
                "thinking_budgets = 17\nthinking_budget = 17\n"
                "reasoning_template = {\"vendor_budget\":\"$thinking_budget\",\"static\":true}\n")
    select_provider(ctx, "custom")
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None)
    s.submit("hello")
    s.wait_turn_done()
    body = ctx.mock.requests[-1]
    assert body["vendor_budget"] == 17 and body["static"] is True, body


def test_invalid_reasoning_template_never_reaches_the_provider(ctx):
    """Bad config is answered locally before an ambiguous request is sent."""
    write_provider(ctx, "bad", ctx.mock.base_url, key="sk")
    with ctx.config_file().open("a") as f:
        f.write('[providers.bad.models."mock-model"]\n'
                "reasoning_template = [not an object]\n")
    select_provider(ctx, "bad")
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None)
    s.submit("hello")
    s.wait_text("reasoning template must be a JSON object")
    assert ctx.mock.requests == [], ctx.mock.requests


def test_reasoning_template_rejects_trailing_garbage(ctx):
    """A valid JSON prefix does not make the whole template valid JSON."""
    write_provider(ctx, "bad", ctx.mock.base_url, key="sk")
    with ctx.config_file().open("a") as f:
        f.write('[providers.bad.models."mock-model"]\n'
                'reasoning_template = {"vendor":1} trailing\n')
    select_provider(ctx, "bad")
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None)
    s.submit("hello")
    s.wait_text("reasoning template must be a JSON object")
    assert ctx.mock.requests == [], ctx.mock.requests


def test_credentials_readable_by_others_are_refused(ctx):
    """A key anyone can read is one to rotate, so it is not loaded silently.

    The picker lists every provider, so the refusal is what that provider
    contributes to the list instead of models.
    """
    write_provider(ctx, "work", ctx.mock.base_url, model="alpha", key="sk-work")
    credentials_file(ctx).chmod(0o644)
    s = ctx.spawn()
    s.submit("/model")
    s.wait_text("readable by others")
    assert "could not list work" in s.text(), s.text()
    assert active(ctx) is None, ctx.state()


def test_the_first_run_without_a_key_says_how_to_add_one(ctx):
    """Nothing to talk to: the welcome screen names the command, and waits."""
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None)
    s.wait_text("+ add a provider")
    assert s.status_kind() == "setup", s.status_line()
    assert "gpt-4o-mini" not in s.status_line(), s.status_line()
    assert "api.openai.com" not in s.status_line(), s.status_line()
    assert "a name for this provider" not in s.text(), s.text()
    ctx.check_screen(s)


def test_a_message_without_a_provider_is_refused(ctx):
    """A turn with no endpoint is a 401 nobody can act on, so it never runs."""
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None)
    s.wait_text("+ add a provider")
    s.submit("hello")
    s.wait_text("no provider yet")
    assert "provider error" not in s.text(), s.text()
    assert ctx.mock.requests == [], ctx.mock.requests


def test_the_welcome_hint_goes_away_once_a_provider_exists(ctx):
    """The line is a missing endpoint and model, so setting both retires it."""
    ctx.scenario("models=alpha")
    s = first_run(ctx)
    s.wait_text("+ add a provider")
    s.submit("/provider")
    add_provider(s, ctx, "work")
    s.wait_status("pick a model")
    s.key("enter")
    s.wait_text("model: alpha @ work")
    assert "no provider yet" not in s.text(), s.text()
    assert "no model yet" not in s.text(), s.text()


def test_a_configured_endpoint_starts_a_conversation(ctx):
    """A named endpoint that needs no key is a run, not a question."""
    ctx.scenario("text=no+key+needed")
    s = ctx.spawn(ARQAN_API_KEY=None)
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
        s.wait_status("providers")
        s.key("down", "down", "enter")
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

    s = first_run(ctx)
    s.submit("/provider")
    add_provider(s, ctx, "work", key="")     # this endpoint needs no key
    s.wait_status("pick a model")
    s.key("enter")
    s.wait_text("model: alpha @ work")
    assert creds(ctx) == {}, creds(ctx)


def test_the_new_provider_form_lists_only_its_own_models(ctx):
    """Setting up an endpoint is not the place to reach another one's pins."""
    write_provider(ctx, "home", ctx.mock.base_url, model="beta", key="sk-home")
    p = ctx.state_file()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("[favorites.home]\nmodels = beta\n")
    ctx.scenario("models=alpha|beta")
    s = first_run(ctx)
    s.submit("/provider")
    s.wait_status("providers")
    s.key("down").sync()
    assert "add a provider" in s.popup_selected(), s.popup_selected()
    s.key("enter")
    add_provider(s, ctx, "work")
    s.wait_status("pick a model")
    assert "@" not in s.text(), "one provider serves the list, so no row names one"
    assert "* beta" not in s.text(), s.text()
