"""The status line's context field: what it measures and what it estimates.

The provider states the tokens one request carried. That is the only exact
figure there is, it describes the conversation as it stood when the request
went out, and it belongs to the model that answered. Everything else the
field says is an estimate, and says so.
"""

import json

from tests.context import wait_until


def field(s) -> str:
    """The context field, immediately before the appended permissions field."""
    return s.status_field(-2)


def tokens(s) -> int:
    value = field(s).lstrip("~").split("/")[0]
    scales = {"k": 1000, "M": 1000000}
    if value[-1:] in scales:
        return int(value[:-1]) * scales[value[-1]]
    return int(value)


def test_the_field_is_exact_only_while_the_measurement_covers_it(ctx):
    """Measured for the conversation that was sent, estimated once the reply
    lands on top of it."""
    ctx.scenario("words=400,usage_first=1,usage=5000/300")
    s = ctx.spawn()
    s.submit("go on")
    s.wait_for(lambda t: field(s) == "5k", "the measured request")
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


def test_a_configured_model_window_is_shown_beside_the_count(ctx):
    """The user-owned exact model profile supplies the context window."""
    config = ctx.config_file()
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text('[providers.work]\n'
                      f'base_url = {ctx.mock.base_url}\nmodel = alpha\n'
                      '[providers.work.models."alpha"]\n'
                      'context_window = 200000\n')
    state = ctx.state_file()
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text("provider = work\n")
    ctx.scenario("text=ok,usage=5000/100")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    s.submit("hello")
    s.wait_turn_done()
    s.wait_for(lambda t: field(s).endswith("/200k"), "the configured window")


def test_a_large_context_count_uses_the_same_units_as_the_window(ctx):
    """Both sides of the context gauge stay compact and easy to compare."""
    config = ctx.config_file()
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text('[providers.work]\n'
                      f'base_url = {ctx.mock.base_url}\nmodel = alpha\n'
                      '[providers.work.models."alpha"]\n'
                      'context_window = 258000\n')
    state = ctx.state_file()
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text("provider = work\n")
    ctx.scenario("text=ok,usage=123405/100")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    s.submit("hello")
    s.wait_turn_done()
    assert field(s) == "~123k/258k", field(s)


def test_a_models_listing_does_not_define_the_window(ctx):
    """Non-standard listing metadata is not a user-owned model profile."""
    ctx.scenario("models=alpha|beta,text=ok,model_window=128000,"
                 "model_window_key=context_window,usage=5000/100")
    s = ctx.spawn(ARQAN_MODEL="alpha")
    s.submit("/model")
    s.wait_status("pick a model")
    s.key("down", "enter")
    s.wait_text("model: beta")
    s.submit("hello")
    s.wait_turn_done()
    assert "/" not in field(s), field(s)


def test_a_window_no_model_could_have_is_refused(ctx):
    """An invalid configured window is dropped rather than clamped."""
    config = ctx.config_file()
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text('[providers.work]\n'
                      f'base_url = {ctx.mock.base_url}\nmodel = alpha\n'
                      '[providers.work.models."alpha"]\n'
                      'context_window = 9000000000\n')
    state = ctx.state_file()
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text("provider = work\n")
    ctx.scenario("text=ok,usage=5000/100")
    s = ctx.spawn(ARQAN_MODEL="alpha", ARQAN_BASE_URL=None,
                  ARQAN_API_KEY=None)
    s.submit("hello")
    s.wait_turn_done()
    assert "/" not in field(s), field(s)


def wire_tokens(request) -> int:
    """What the mock charged for a request: its messages at four bytes a
    token, which is the only tokenizer in the test suite."""
    return max(1, len(json.dumps(request.get("messages", []))) // 4)


def test_the_field_follows_the_wire_when_old_results_are_elided(ctx):
    """A tool result older than two user turns goes out as a one-line note,
    so the conversation costs less than the bytes it holds. The field has to
    say so when the turn that elides them is typed, not a request later: it
    reports what a request sent now would carry, and that is what decides
    whether there is room for one.
    """
    ctx.write_config(
        "compact = off\n"
        "[providers.work]\n"
        f"base_url = {ctx.mock.base_url}\n"
        "model = alpha\n"
        '[providers.work.models."alpha"]\n'
        "context_window = 1000\n"
    )
    state = ctx.state_file()
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text("provider = work\n")
    args = json.dumps({"command": "seq 1 800"})
    ctx.scenario(f"tool=bash:{args},tool_rounds=1,text=ok,final_text=ok")
    # A window the run fills: the boundary only advances under pressure.
    s = ctx.spawn(ARQAN_PERMISSIONS="free", ARQAN_MODEL="alpha",
                  ARQAN_BASE_URL=None, ARQAN_API_KEY=None)
    s.submit("run it")
    s.wait_turn_done()
    ctx.scenario("text=ok")
    s.submit("and again")
    s.wait_turn_done()

    before = tokens(s)
    # Held open, so the field is read from the estimate alone: no usage for
    # this request has been heard yet.
    ctx.scenario("hold,text=ok")
    sent = len(ctx.mock.requests)
    # A third user turn puts the result two turns back, which is where the
    # writer stops sending it.
    s.submit("carry on")
    request = wait_until(lambda: ctx.mock.requests[sent:], "the request")[0]
    predicted = tokens(s.settle())
    ctx.mock.release()
    s.wait_turn_done()

    carried = wire_tokens(request)
    assert "result elided" in json.dumps(request.get("messages", [])), \
        "the run never got old enough to elide a result"
    assert predicted < before, (
        f"the turn that elides the result has to cost less: "
        f"{before} -> {predicted}")
    # Counting the bytes the conversation holds rather than the ones it sends
    # is what `before` is; the field must be the closer of the two.
    assert abs(predicted - carried) < abs(before - carried), (
        predicted, before, carried)


def test_a_resumed_session_reports_its_context_before_it_speaks(ctx):
    """A resumed conversation costs what it costs whether or not this run has
    heard a reply yet, so the field estimates it on the frame it is replayed
    and gives way to the measurement when one arrives."""
    ctx.scenario("text=a+long+enough+first+answer+to+be+worth+counting")
    s = ctx.spawn()
    s.submit("a question with some words in it to give the fit something")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    s = ctx.spawn()
    fresh = tokens(s)
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter").sync()
    s.wait_text("a long enough first answer")
    resumed = tokens(s)
    assert field(s).startswith("~"), field(s)
    assert resumed > fresh, (
        f"the replayed conversation has to show in the field: "
        f"{fresh} -> {resumed}")

    ctx.scenario("text=ok,usage=800/10")
    s.submit("carry on")
    s.wait_turn_done()
    # The measurement wins: an estimate is only what stands in for one.
    assert 800 <= tokens(s) < 900, field(s)
