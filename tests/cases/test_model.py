"""The /model picker: choosing a model from the provider's /models endpoint."""


def open_picker(ctx, s):
    s.submit("/model")
    return s.wait_status("pick a model")


def test_picker_lists_the_provider_models(ctx):
    """A short list is the plain popup, with the live model marked."""
    ctx.scenario("models=alpha|beta|gamma")
    s = ctx.spawn(YOKE_MODEL="beta")
    open_picker(ctx, s)
    text = s.text()
    assert "alpha" in text and "beta" in text and "gamma" in text, text
    assert "current" in text, "the live model is labelled"
    assert "+ enter a model manually" in text, text
    assert "search:" not in text, "a short list needs no search box"
    ctx.check_screen(s)


def test_choosing_switches_the_model(ctx):
    """Enter on an entry retitles the status line and answers in the popup slot."""
    ctx.scenario("models=alpha|beta|gamma")
    s = ctx.spawn(YOKE_MODEL="alpha")
    open_picker(ctx, s)
    s.key("down").sync()
    s.key("enter")
    s.wait_text("model: beta")
    assert s.status_field(1) == "beta", s.status_line()
    assert s.composer_text() == "", "picking must not type into the composer"


def test_the_chosen_model_is_used_for_the_next_turn(ctx):
    """The request body carries the picked model, not the configured one."""
    ctx.scenario("models=alpha|beta|gamma,text=done")
    s = ctx.spawn(YOKE_MODEL="alpha")
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
    s = ctx.spawn(YOKE_MODEL="alpha")
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
    s = ctx.spawn(YOKE_MODEL=None)
    open_picker(ctx, s)
    s.key("down").sync()
    s.key("enter")
    s.wait_for(lambda t: s.status_field(1) == "beta", "the model to change")
    s.submit("/exit")
    s.wait_exit()

    assert ctx.state().get("model") == "beta", ctx.state()

    again = ctx.spawn(YOKE_MODEL=None)
    assert again.status_field(1) == "beta", again.status_line()


def test_the_environment_still_wins_over_the_remembered_model(ctx):
    """YOKE_MODEL is a per-invocation override, so it outranks the state file."""
    ctx.scenario("models=alpha|beta|gamma")
    s = ctx.spawn(YOKE_MODEL=None)
    open_picker(ctx, s)
    s.key("down").sync()
    s.key("enter")
    s.wait_for(lambda t: s.status_field(1) == "beta", "the model to change")
    s.submit("/exit")
    s.wait_exit()

    again = ctx.spawn(YOKE_MODEL="gamma")
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
    ctx.scenario("models=alpha|beta")
    s = ctx.spawn(YOKE_MODEL="alpha")
    open_picker(ctx, s)
    s.key("down", "down", "enter")
    s.wait_text("model id (not verified)")
    s.type("private-model").sync()
    s.key("enter")
    s.wait_text("entered manually; not verified")
    assert "private-model" in s.status_line(), s.status_line()


def test_cancelling_manual_fallback_keeps_the_model(ctx):
    ctx.scenario("models_status=500")
    s = ctx.spawn(YOKE_MODEL="alpha")
    s.submit("/model")
    s.wait_text("enter a model manually")
    s.key("esc")
    s.wait_status("ready")
    assert "alpha" in s.status_line(), s.status_line()


def test_an_empty_models_list_opens_manual_entry(ctx):
    ctx.scenario("models_empty=1")
    s = ctx.spawn(YOKE_MODEL="alpha")
    s.submit("/model")
    s.wait_text("the provider listed no models; enter a model manually")
    s.type("manual-after-empty").sync()
    s.key("enter")
    s.wait_text("entered manually; not verified")
    assert "manual-after-empty" in s.status_line(), s.status_line()
