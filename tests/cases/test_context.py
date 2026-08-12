"""The status line's context field: what it measures and what it estimates.

The provider states the tokens one request carried. That is the only exact
figure there is, it describes the conversation as it stood when the request
went out, and it belongs to the model that answered. Everything else the
field says is an estimate, and says so.
"""


def field(s) -> str:
    """The context field, which is the last one on the status line."""
    return s.status_field(-1)


def tokens(s) -> int:
    value = field(s).lstrip("~").split("/")[0]
    return int(value)


def test_the_field_is_exact_only_while_the_measurement_covers_it(ctx):
    """Measured for the conversation that was sent, estimated once the reply
    lands on top of it."""
    ctx.scenario("words=200,usage_first=1,usage=5000/300")
    s = ctx.spawn()
    s.submit("go on")
    s.wait_for(lambda t: field(s) == "5000", "the measured request")
    s.wait_turn_done()
    assert field(s).startswith("~"), field(s)
    assert tokens(s) > 5000, "the reply is context the next request carries"


def test_local_output_between_requests_reaches_the_field(ctx):
    """A shell run is replayed to the provider, so it is context the moment
    it is captured, not the next time a request is measured."""
    ctx.scenario("text=ok,usage=5000/100")
    s = ctx.spawn()
    s.submit("start")
    s.wait_turn_done()
    before = tokens(s)
    s.submit("!printf 'x%.0s' $(seq 1 4000)")
    s.wait_text("\u2514\u2500 exit 0")
    s.wait_for(lambda t: tokens(s) > before + 500, "the captured output")


def test_switching_models_does_not_restate_the_old_count(ctx):
    """A count belongs to the tokenizer that produced it. Switching keeps the
    size of the conversation and stops calling it measured."""
    ctx.scenario("models=alpha|beta,text=ok,usage=5000/100")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    s.submit("hello")
    s.wait_turn_done()
    before = tokens(s)
    s.submit("/model")
    s.wait_status("pick a model")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("model: beta")
    s.wait_for(lambda t: s.status_field(1) == "beta", "the new model")
    after = tokens(s)
    assert field(s).startswith("~"), f"beta has measured nothing: {field(s)}"
    assert abs(after - before) <= 200, (before, after)


def test_clearing_the_conversation_drops_what_it_carried(ctx):
    """/clear keeps the system prompt and the tools, so the field falls back
    to what a request costs carrying nothing else."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.submit("start")
    s.wait_turn_done()
    s.submit("!printf 'x%.0s' $(seq 1 4000)")
    s.wait_text("\u2514\u2500 exit 0")
    s.wait_for(lambda t: tokens(s) > 900, "the captured output")
    before = tokens(s)
    s.submit("/clear")
    s.wait_for(lambda t: tokens(s) < before - 500, "the emptied conversation")


def test_a_published_window_is_shown_beside_the_count(ctx):
    """The /models listing is the free source of a context window."""
    ctx.scenario("models=alpha|beta,model_window=200000,text=ok,"
                 "usage=5000/100")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    s.submit("hello")
    s.wait_turn_done()
    assert "/" not in field(s), "nothing has listed the models yet"
    s.submit("/model")
    s.wait_status("pick a model")
    s.key("down").sync()
    s.key("enter")
    s.wait_for(lambda t: field(s).endswith("/200k"), "the published window")


def test_each_endpoint_publishes_the_window_under_its_own_name(ctx):
    """One endpoint's field name is another's; the listing is read for all of
    the names endpoints actually use."""
    for key in ("max_input_tokens", "context_window", "max_model_len",
                "loaded_context_length", "top_provider"):
        ctx.scenario(f"models=alpha|beta,text=ok,model_window=128000,"
                     f"model_window_key={key}")
        s = ctx.spawn(ARQAN_MODEL="alpha")
        s.submit("hello")
        s.wait_turn_done()
        s.submit("/model")
        s.wait_status("pick a model")
        s.key("down").sync()
        s.key("enter")
        s.wait_text("model: beta")
        s.wait_for(lambda t: field(s).endswith("/128k"), f"window from {key}")
        s.submit("/exit")
        s.wait_exit()


def test_a_window_no_model_could_have_is_refused(ctx):
    """A listing states the window; a number that cannot be one is dropped
    rather than believed or clamped."""
    ctx.scenario("models=alpha|beta,model_window=9000000000,text=ok,"
                 "usage=5000/100")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    s.submit("/model")
    s.wait_status("pick a model")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("model: beta")
    s.submit("hello")
    s.wait_turn_done()
    assert "/" not in field(s), field(s)
