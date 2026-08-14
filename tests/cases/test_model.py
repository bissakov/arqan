"""The /model picker: choosing a model from the provider's /models endpoint."""


def open_picker(ctx, s):
    s.submit("/model")
    return s.wait_status("pick a model")


def test_picker_lists_the_provider_models(ctx):
    """A short list is the plain popup, with the live model marked."""
    ctx.scenario("models=alpha|beta|gamma")
    s = ctx.spawn(ARQAN_MODEL="beta")
    open_picker(ctx, s)
    text = s.text()
    assert "alpha" in text and "beta" in text and "gamma" in text, text
    assert "current" in text, "the live model is labelled"
    assert "+ enter a model manually" not in text, text
    assert "Ctrl-O manual entry" in text, text
    assert "Ctrl-S small model" in text, text
    assert "search:" not in text, "a short list needs no search box"
    ctx.check_screen(s)


def test_choosing_switches_the_model(ctx):
    """Enter on an entry retitles the status line and answers in the popup slot."""
    ctx.scenario("models=alpha|beta|gamma")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    open_picker(ctx, s)
    s.key("down").sync()
    s.key("enter")
    s.wait_text("model: beta")
    assert s.status_field(1) == "beta", s.status_line()
    assert s.composer_text() == "", "picking must not type into the composer"


def test_the_chosen_model_is_used_for_the_next_turn(ctx):
    """The request body carries the picked model, not the configured one."""
    ctx.scenario("models=alpha|beta|gamma,text=done")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    open_picker(ctx, s)
    s.key("down").sync()
    s.key("enter")
    s.wait_for(lambda t: s.status_field(1) == "beta", "the model to change")
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["model"] == "beta", ctx.mock.requests[-1]["model"]


def test_esc_cancels_the_picker(ctx):
    """Cancelling leaves the model alone."""
    ctx.scenario("models=alpha|beta|gamma")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    open_picker(ctx, s)
    s.key("esc")
    s.wait_status("ready")
    assert s.status_field(1) == "alpha", s.status_line()


def test_ten_models_still_use_the_plain_popup(ctx):
    """The searchable popup starts above ten entries, not at it."""
    ctx.scenario("model_count=10")
    s = ctx.spawn()
    open_picker(ctx, s)
    assert "search:" not in s.text(), s.text()


def test_a_long_list_is_searchable(ctx):
    """Past ten entries the picker takes the keyboard and filters."""
    ctx.scenario("model_count=20")
    s = ctx.spawn()
    open_picker(ctx, s)
    assert "search:" in s.text(), s.text()
    s.type("017").sync()
    text = s.text()
    assert "search: 017" in text, text
    assert "model-017" in text, text
    assert "model-016" not in text, text
    ctx.check_screen(s, "filtered")
    s.key("enter")
    s.wait_for(lambda t: s.status_field(1) == "model-017", "the picked model")


def test_a_provider_serving_hundreds_of_models_lists_all_of_them(ctx):
    """The last entry of a long list is searchable, not dropped by a cap."""
    ctx.scenario("model_count=900")
    s = ctx.spawn()
    open_picker(ctx, s)
    s.type("899").sync()
    assert "model-899" in s.text(), s.text()
    s.key("enter")
    s.wait_for(lambda t: s.status_field(1) == "model-899", "the picked model")


def test_search_is_a_literal_substring(ctx):
    """Nothing fuzzy: characters have to appear together, in order."""
    ctx.scenario("model_count=20")
    s = ctx.spawn()
    open_picker(ctx, s)
    s.type("m1").sync()
    assert "(no match)" in s.text(), s.text()
    s.key("backspace").sync()
    assert "(no match)" not in s.text(), s.text()


def test_closing_a_long_picker_drops_its_matches(ctx):
    """A choice past the command count leaves no stale match behind it."""
    ctx.scenario("model_count=900")
    s = ctx.spawn()
    open_picker(ctx, s)
    s.type("899").sync()
    s.key("enter")
    s.wait_for(lambda t: s.status_field(1) == "model-899", "the picked model")
    # The composer popup indexes the command table, which the picker's 900
    # entries outnumber: a match surviving the close reads past its end.
    s.type("/").sync()
    assert "/model" in s.text(), s.text()


def test_search_keystrokes_never_reach_the_composer(ctx):
    """The picker owns the keyboard while it is open."""
    ctx.scenario("model_count=20")
    s = ctx.spawn()
    open_picker(ctx, s)
    s.type("003").sync()
    s.key("enter")
    s.wait_for(lambda t: s.status_field(1) == "model-003", "the picked model")
    assert s.composer_text() == "", s.composer_lines()


def test_the_choice_is_remembered_across_runs(ctx):
    """The pick lands in the state dir and comes back on the next launch."""
    ctx.scenario("models=alpha|beta|gamma")
    s = ctx.spawn(ARQAN_MODEL=None)
    open_picker(ctx, s)
    s.key("down").sync()
    s.key("enter")
    s.wait_for(lambda t: s.status_field(1) == "beta", "the model to change")
    s.submit("/exit")
    s.wait_exit()

    assert ctx.state().get("model") == "beta", ctx.state()

    again = ctx.spawn(ARQAN_MODEL=None)
    assert again.status_field(1) == "beta", again.status_line()


def test_the_environment_still_wins_over_the_remembered_model(ctx):
    """ARQAN_MODEL is a per-invocation override, so it outranks the state file."""
    ctx.scenario("models=alpha|beta|gamma")
    s = ctx.spawn(ARQAN_MODEL=None)
    open_picker(ctx, s)
    s.key("down").sync()
    s.key("enter")
    s.wait_for(lambda t: s.status_field(1) == "beta", "the model to change")
    s.submit("/exit")
    s.wait_exit()

    again = ctx.spawn(ARQAN_MODEL="gamma")
    assert again.status_field(1) == "gamma", again.status_line()


def test_an_unreachable_models_endpoint_answers_in_the_popup_slot(ctx):
    """A refused list reports the error and falls back to manual entry."""
    ctx.scenario("models_status=500")
    s = ctx.spawn()
    s.submit("/model")
    s.wait_text("models: HTTP 500; enter a model manually")
    s.type("manual-model").sync()
    s.key("enter")
    s.wait_text("entered manually; not verified")
    assert "manual-model" in s.status_line(), s.status_line()


def test_manual_entry_is_offered_after_a_successful_list(ctx):
    """Ctrl-O stays visible and available with hundreds of model rows."""
    ctx.scenario("model_count=900")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    open_picker(ctx, s)
    s.key("ctrl-o")
    s.wait_text("model id (not verified)")
    s.type("private-model").sync()
    s.key("enter")
    s.wait_text("entered manually; not verified")
    assert "private-model" in s.status_line(), s.status_line()


def test_the_current_model_can_be_configured_from_the_picker(ctx):
    """Ctrl-E works above a long list and configures the row it is on.

    The cursor opens on the model the session is on, so the row is that model
    and the settings reach the live request as well as the file.
    """
    ctx.scenario("model_count=900,text=ok,usage=5000/100")
    write_provider = ctx.config_file()
    write_provider.parent.mkdir(parents=True, exist_ok=True)
    write_provider.write_text(
        f"[providers.work]\nbase_url = {ctx.mock.base_url}\n")
    state = ctx.state_file()
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text("provider = work\nmodel = model-500\n")
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None)
    open_picker(ctx, s)
    assert "model-500" in s.popup_selected(), s.popup_selected()
    s.key("ctrl-e")
    s.wait_text("context window")
    s.type("200000").key("enter")
    s.wait_text("Named efforts")
    s.key("down", "enter")
    s.wait_text("reasoning efforts")
    s.type("low,high").key("enter")
    s.wait_text("active effort")
    s.type("high").key("enter")
    s.wait_text("model settings saved")
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["reasoning_effort"] == "high"
    assert s.status_field(-2).endswith("/200k"), s.status_line()
    profile = ctx.settings(ctx.config_file())[
        'providers.work.models."model-500"']
    assert profile == {
        "context_window": "200000",
        "reasoning_efforts": "low,high",
        "reasoning_effort": "high",
    }, profile


def test_cancelling_manual_fallback_keeps_the_model(ctx):
    ctx.scenario("models_status=500")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    s.submit("/model")
    s.wait_text("enter a model manually")
    s.key("esc")
    s.wait_status("ready")
    assert "alpha" in s.status_line(), s.status_line()


def test_an_empty_models_list_opens_manual_entry(ctx):
    ctx.scenario("models_empty=1")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    s.submit("/model")
    s.wait_text("the provider listed no models; enter a model manually")
    s.type("manual-after-empty").sync()
    s.key("enter")
    s.wait_text("entered manually; not verified")
    assert "manual-after-empty" in s.status_line(), s.status_line()


# ---- favorites -----------------------------------------------------------
def picker_rows(s, names):
    """The picker's model rows, in the order the popup lists them."""
    out = []
    for line in s.text().splitlines():
        row = line.strip()
        if row.startswith("\u203a "):
            row = row[2:].strip()
        if row.startswith("* "):
            row = row[2:].strip()
        first = row.split()[0] if row.split() else ""
        if first in names:
            out.append(first)
    return out


def test_ctrl_f_pins_a_model_to_the_top(ctx):
    """The favorite is starred, first, and listed once."""
    ctx.scenario("models=alpha|beta|gamma")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    open_picker(ctx, s)
    s.key("down").sync()
    assert "beta" in s.popup_selected(), s.popup_selected()
    s.key("ctrl-f").sync()
    text = s.text()
    assert "* beta" in text, text
    assert text.count("beta") == 1, "a favorite is not listed twice"
    assert "* beta" in s.popup_selected(), "the selection follows the row"
    assert picker_rows(s, ["alpha", "beta", "gamma"]) == \
        ["beta", "alpha", "gamma"], s.text()
    ctx.check_screen(s, "favorite")


def test_ctrl_f_again_unpins_it(ctx):
    ctx.scenario("models=alpha|beta|gamma")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    open_picker(ctx, s)
    s.key("down").sync()
    s.key("ctrl-f").sync()
    s.key("ctrl-f").sync()
    assert "* beta" not in s.text(), s.text()
    assert picker_rows(s, ["alpha", "beta", "gamma"]) == \
        ["alpha", "beta", "gamma"], s.text()
    assert "beta" in s.popup_selected(), s.popup_selected()
    # The last favorite leaves no key behind, rather than an empty list.
    assert "models" not in ctx.settings(ctx.state_file()).get("favorites", {}), \
        ctx.settings(ctx.state_file())


def test_a_favorite_is_remembered_across_runs(ctx):
    """Pinning is its own action: it survives a picker the user cancels."""
    ctx.scenario("models=alpha|beta|gamma")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    open_picker(ctx, s)
    s.key("down").sync()
    s.key("ctrl-f").sync()
    s.key("esc")
    s.wait_status("ready")
    assert s.status_field(1) == "alpha", "pinning does not choose"
    favorites = ctx.settings(ctx.state_file()).get("favorites", {})
    assert favorites.get("models") == "beta", favorites
    s.submit("/exit")
    s.wait_exit()

    again = ctx.spawn(ARQAN_MODEL="alpha")
    open_picker(ctx, again)
    assert "* beta" in again.text(), again.text()
    assert picker_rows(again, ["alpha", "beta", "gamma"]) == \
        ["beta", "alpha", "gamma"], again.text()


def test_favorites_keep_the_order_they_were_pinned_in(ctx):
    ctx.scenario("models=alpha|beta|gamma")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    open_picker(ctx, s)
    s.key("down", "down").sync()
    s.key("ctrl-f").sync()          # gamma
    s.key("down").sync()            # back onto alpha, now last
    s.key("ctrl-f").sync()
    assert ctx.settings(ctx.state_file())["favorites"]["models"] == \
        "gamma, alpha", ctx.settings(ctx.state_file())
    assert picker_rows(s, ["alpha", "beta", "gamma"]) == \
        ["gamma", "alpha", "beta"], s.text()


def test_choosing_a_favorite_switches_to_it(ctx):
    """The reordered row still answers with the model it names."""
    ctx.scenario("models=alpha|beta|gamma")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    open_picker(ctx, s)
    s.key("down", "down").sync()
    s.key("ctrl-f").sync()
    s.key("enter")
    s.wait_text("model: gamma")
    assert s.status_field(1) == "gamma", s.status_line()


def test_favorites_are_kept_per_provider(ctx):
    """A model id only means something against the endpoint that served it."""
    ctx.scenario("models=alpha|beta|gamma")
    ctx.write_config(
        f'provider = "mock"\n\n[providers.mock]\n'
        f'base_url = "{ctx.mock.base_url}"\napi = "openai"\n'
        f'model = "alpha"\n'
    )
    s = ctx.spawn(ARQAN_MODEL=None, ARQAN_BASE_URL=None)
    open_picker(ctx, s)
    s.key("down").sync()
    s.key("ctrl-f").sync()
    s.wait_for(lambda t: "* beta" in s.text(), "the favorite")
    state = ctx.settings(ctx.state_file())
    assert state.get("favorites.mock", {}).get("models") == "beta", state
    assert "favorites" not in state, state


def test_ctrl_o_is_available_without_a_manual_row(ctx):
    ctx.scenario("models=alpha|beta")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    open_picker(ctx, s)
    assert "enter a model manually" not in s.text(), s.text()
    s.key("ctrl-o")
    s.wait_text("model id (not verified)")


def test_a_favorite_survives_the_search_box(ctx):
    """A long list filters and still pins the row the query left selected."""
    ctx.scenario("model_count=20")
    s = ctx.spawn()
    open_picker(ctx, s)
    s.type("017").sync()
    s.key("ctrl-f").sync()
    assert "* model-017" in s.text(), s.text()
    s.key("ctrl-u").sync()
    assert picker_rows(s, ["model-017", "model-000"])[0] == "model-017", s.text()


# ---- models of every provider --------------------------------------------
def two_providers(ctx, pins="", keys=False):
    """Two stored endpoints, `mock` serving the chosen model.

    Both are served by the same mock: what is under test is which provider a
    row belongs to, not what an endpoint answers. `pins` is written under
    `spare`, since a pin belongs to the endpoint that serves the model.
    """
    ctx.write_config(
        f'[providers.mock]\nbase_url = "{ctx.mock.base_url}"\n'
        f'api = "openai"\n\n'
        f'[providers.spare]\nbase_url = "{ctx.mock.base_url}"\n'
        f'api = "openai"\n'
    )
    if keys:
        c = ctx.home / ".local" / "state" / "arqan" / "credentials.toml"
        c.parent.mkdir(parents=True, exist_ok=True)
        c.write_text("[providers.mock]\nkey = sk-mock\n\n"
                     "[providers.spare]\nkey = sk-spare\n")
        c.chmod(0o600)
    p = ctx.state_file()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("provider = mock\nmodel = alpha\n"
                 + (f"\n[favorites.spare]\nmodels = {pins}\n" if pins else ""))


def open_two_provider_picker(ctx, pins="", keys=False):
    ctx.scenario("models=alpha|beta|gamma")
    two_providers(ctx, pins=pins, keys=keys)
    s = ctx.spawn(ARQAN_MODEL=None, ARQAN_BASE_URL=None, ARQAN_API_KEY=None)
    open_picker(ctx, s)
    return s


def named_rows(s):
    """The picker's rows as they read, "<id> @ <provider>", in order."""
    out = []
    for line in s.text().splitlines():
        row = line.strip()
        if row.startswith("\u203a "):
            row = row[2:].strip()
        if " @ " not in row:
            continue
        out.append(row.split("   ")[0].strip())
    return out


def pinned_row(s, name):
    """The picker row for pinned `name`, without the selection marker."""
    for line in s.text().splitlines():
        row = line.strip().lstrip("\u203a ").strip()
        if row.startswith(f"* {name}"):
            return row
    return ""


def test_every_configured_provider_is_listed(ctx):
    """One list for the whole run: a model is a pair, so each row names one."""
    s = open_two_provider_picker(ctx)
    assert named_rows(s) == [
        "alpha @ mock", "beta @ mock", "gamma @ mock",
        "alpha @ spare", "beta @ spare", "gamma @ spare",
    ], s.text()
    assert "alpha @ mock" in s.popup_selected(), \
        "the cursor opens on the model the session is on"
    ctx.check_screen(s)


def test_the_picker_lists_every_provider_on_each_open(ctx):
    """No cache: what a provider serves is asked again every time it is read."""
    s = open_two_provider_picker(ctx, keys=True)
    assert ctx.mock.listings == ["Bearer sk-mock", "Bearer sk-spare"], \
        ctx.mock.listings
    s.key("esc")
    s.wait_status("ready")
    open_picker(ctx, s)
    assert ctx.mock.listings == ["Bearer sk-mock", "Bearer sk-spare"] * 2, \
        ctx.mock.listings


def test_the_same_id_at_another_provider_is_not_current(ctx):
    """Matching ids across endpoints are different models."""
    s = open_two_provider_picker(ctx)
    assert s.text().count("current") == 1, "only the live pair is current"
    for line in s.text().splitlines():
        if "alpha @ spare" in line:
            assert "current" not in line, line


def test_choosing_a_row_of_another_provider_moves_the_session(ctx):
    """The row carries its endpoint, so the pair that is stored names it."""
    s = open_two_provider_picker(ctx, keys=True)
    s.key("down", "down", "down", "down").sync()
    assert "beta @ spare" in s.popup_selected(), s.popup_selected()
    s.key("enter")
    s.wait_text("model: beta @ spare")
    assert s.status_field(1) == "beta", s.status_line()
    assert s.status_field(3) == "spare", s.status_line()
    assert ctx.state().get("provider") == "spare", ctx.state()
    assert ctx.state().get("model") == "beta", ctx.state()
    stored = ctx.settings(ctx.config_file())
    assert "model" not in stored["providers.spare"], stored
    assert "model" not in stored["providers.mock"], stored


def test_a_pinned_pair_is_listed_first_and_named(ctx):
    """Pins order one list, wherever their models are served."""
    s = open_two_provider_picker(ctx, pins="beta")
    assert named_rows(s)[0] == "* beta @ spare", s.text()
    assert named_rows(s).count("beta @ spare") == 0, "a pin is listed once"
    assert "alpha @ mock" in s.popup_selected(), \
        "the cursor still opens on the live pair"


def test_unpinning_a_listed_model_keeps_its_row(ctx):
    """The listing says the model is there, so only the pin goes."""
    s = open_two_provider_picker(ctx, pins="beta")
    s.key("up").sync()
    assert "* beta @ spare" in s.popup_selected(), s.popup_selected()
    s.key("ctrl-f").sync()
    assert "* beta" not in s.text(), s.text()
    assert "beta @ spare" in s.popup_selected(), "the selection follows the row"
    assert named_rows(s) == [
        "alpha @ mock", "beta @ mock", "gamma @ mock",
        "alpha @ spare", "beta @ spare", "gamma @ spare",
    ], s.text()
    state = ctx.settings(ctx.state_file())
    assert state.get("favorites.spare", {}).get("models") is None, state
    assert ctx.state().get("provider") == "mock", "unpinning chooses nothing"


def test_a_pin_its_provider_no_longer_lists_is_not_offered(ctx):
    """The listing is what a reachable provider serves, pins included."""
    s = open_two_provider_picker(ctx, pins="delta")
    assert "delta" not in s.text(), s.text()
    assert len(named_rows(s)) == 6, s.text()


def unreachable_provider(ctx, pins="delta"):
    """`mock` live beside an endpoint nothing answers on, holding the pins."""
    ctx.scenario("models=alpha|beta")
    ctx.write_config(
        f'[providers.mock]\nbase_url = "{ctx.mock.base_url}"\n'
        f'api = "openai"\n\n'
        f'[providers.dead]\nbase_url = "http://127.0.0.1:1/v1"\n'
        f'api = "openai"\n'
    )
    p = ctx.state_file()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("provider = mock\nmodel = alpha\n"
                 f"\n[favorites.dead]\nmodels = {pins}\n")
    s = ctx.spawn(ARQAN_MODEL=None, ARQAN_BASE_URL=None, ARQAN_API_KEY=None)
    open_picker(ctx, s)
    return s


def test_a_provider_that_cannot_be_listed_is_reported_with_its_pins(ctx):
    """A listing that did not arrive is a gap local state can still fill."""
    s = unreachable_provider(ctx)
    assert "could not list dead" in s.text(), s.text()
    assert named_rows(s) == [
        "* delta @ dead", "alpha @ mock", "beta @ mock",
    ], s.text()
    s.key("up").sync()
    assert "* delta @ dead" in s.popup_selected(), s.popup_selected()
    s.key("enter")
    s.wait_text("model: delta @ dead")
    assert s.status_field(1) == "delta", s.status_line()
    assert s.status_field(3) == "dead", s.status_line()


def test_unpinning_a_pin_of_an_unlisted_provider_takes_the_row(ctx):
    """The pin was the only reason the row existed, so it goes with it."""
    s = unreachable_provider(ctx)
    s.key("up").sync()
    s.key("ctrl-f").sync()
    assert "delta" not in s.text(), s.text()
    state = ctx.settings(ctx.state_file())
    assert state.get("favorites.dead", {}).get("models") is None, state


def test_ctrl_s_takes_a_model_of_another_provider_as_the_small_model(ctx):
    """Errands may run at another endpoint, so the pair names the provider."""
    s = open_two_provider_picker(ctx)
    s.key("down", "down", "down", "down").sync()
    assert "beta @ spare" in s.popup_selected(), s.popup_selected()
    s.key("ctrl-s").sync()
    s.wait_for(lambda t: "small" in s.popup_selected(), "the row to mark")
    assert ctx.state().get("small_model") == "beta", ctx.state()
    assert ctx.state().get("small_provider") == "spare", ctx.state()
    assert ctx.state().get("provider") == "mock", "the session stays here"
    stored = ctx.settings(ctx.config_file())
    assert "small_model" not in stored["providers.spare"], stored
    assert "small_model" not in stored["providers.mock"], stored

    s.key("ctrl-s").sync()
    s.wait_for(lambda t: "small" not in s.popup_selected(), "it to clear")
    assert "small_model" not in ctx.state(), ctx.state()


def test_one_id_at_two_providers_marks_only_the_small_ones_row(ctx):
    """The small model is a model at an endpoint, not an id."""
    s = open_two_provider_picker(ctx)
    s.key("down", "down", "down", "down").sync()
    s.key("ctrl-s").sync()
    s.wait_for(lambda t: "small" in s.popup_selected(), "the row to mark")
    rows = [line for line in s.text().splitlines()
            if "beta" in line and "small" in line]
    assert len(rows) == 1 and "spare" in rows[0], s.text()
    assert ctx.state().get("small_provider") == "spare", ctx.state()


def test_a_search_finds_a_provider_by_name(ctx):
    """The picker searches names, so the provider is part of one."""
    ctx.scenario("model_count=20")
    two_providers(ctx)
    s = ctx.spawn(ARQAN_MODEL=None, ARQAN_BASE_URL=None, ARQAN_API_KEY=None)
    open_picker(ctx, s)
    assert "search:" in s.text(), "forty rows are a searchable list"
    s.type("spare").sync()
    text = s.text()
    assert "model-000 @ spare" in text, text
    assert "@ mock" not in text, "only that provider's models are left"
    # The id alone still finds it at both providers.
    s.key("ctrl-u").sync()
    s.type("model-017").sync()
    assert named_rows(s) == ["model-017 @ mock", "model-017 @ spare"], s.text()


def three_providers(ctx):
    """`mock` serving the chosen model, with two more endpoints beside it."""
    ctx.write_config("".join(
        f'[providers.{name}]\nbase_url = "{ctx.mock.base_url}"\n'
        f'api = "openai"\n\n' for name in ("mock", "spare", "other")))
    p = ctx.state_file()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("provider = mock\nmodel = alpha\n")


def test_one_id_at_three_providers_is_three_named_rows(ctx):
    """An id names a model only against an endpoint, so each row names one."""
    ctx.scenario("models=alpha")
    three_providers(ctx)
    s = ctx.spawn(ARQAN_MODEL=None, ARQAN_BASE_URL=None, ARQAN_API_KEY=None)
    open_picker(ctx, s)
    assert named_rows(s) == [
        "alpha @ mock", "alpha @ spare", "alpha @ other",
    ], s.text()
    s.key("down", "down").sync()
    assert "alpha @ other" in s.popup_selected(), s.popup_selected()
    s.key("enter")
    s.wait_text("model: alpha @ other")
    assert ctx.state().get("provider") == "other", ctx.state()
    assert ctx.state().get("model") == "alpha", ctx.state()


def test_ctrl_e_configures_the_row_rather_than_the_session(ctx):
    """Capabilities belong to a pair, so they are edited where they are read."""
    ctx.scenario("models=alpha|beta")
    two_providers(ctx)
    s = ctx.spawn(ARQAN_MODEL=None, ARQAN_BASE_URL=None, ARQAN_API_KEY=None)
    open_picker(ctx, s)
    s.key("down", "down", "down").sync()
    assert "beta @ spare" in s.popup_selected(), s.popup_selected()
    s.key("ctrl-e")
    s.wait_text("context window")
    s.type("123456").key("enter")
    s.wait_text("Named efforts")
    s.key("enter")                       # Off: nothing but the window
    s.wait_text("model settings saved")
    profile = ctx.settings(ctx.config_file())[
        'providers.spare.models."beta"']
    assert profile == {"context_window": "123456"}, profile
    assert s.status_field(1) == "alpha", "the session is untouched"
    assert not s.status_field(-2).endswith("/123k"), s.status_line()
