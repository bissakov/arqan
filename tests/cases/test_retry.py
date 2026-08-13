"""Retrying a request that reached nothing, and saying so in the transcript."""

import signal


RETRY = {"ARQAN_RETRIES": 3, "ARQAN_RETRY_DELAY_MS": 10}


def test_transient_failure_is_retried(ctx):
    """Two refused requests are retried and the reply arrives on the third."""
    ctx.scenario("fail_times=2,text=hello+there")
    s = ctx.spawn(**RETRY)
    s.submit("say hi")
    s.wait_text("hello there")
    s.wait_turn_done()
    text = s.text()
    assert "[HTTP 503; retrying in 10ms (attempt 2 of 4)]" in text, text
    assert "[HTTP 503; retrying in 20ms (attempt 3 of 4)]" in text, text
    assert len(ctx.mock.requests) == 3, ctx.mock.requests
    ctx.check_screen(s)


def test_retry_warning_is_red(ctx):
    """The warning reads as a failure, not as an ordinary notice."""
    ctx.scenario("fail_times=1,text=fine+now")
    s = ctx.spawn(**RETRY)
    s.submit("say hi")
    s.wait_text("fine now")
    s.wait_turn_done()
    row = s.screen.find_row("; retrying in ")
    assert s.screen.attr_at(row, 2).fg == 203, "S_RED failure row"


def test_refused_request_is_not_retried(ctx):
    """An authentication failure is an answer about the request, not weather."""
    ctx.scenario("fail_times=1,fail_status=401")
    s = ctx.spawn(**RETRY)
    s.submit("say hi")
    s.wait_text("[provider error: HTTP 401]")
    s.wait_turn_done()
    assert "retrying in" not in s.text(), s.text()
    assert len(ctx.mock.requests) == 1, ctx.mock.requests


def test_dropped_connection_is_retried(ctx):
    """A transport failure before any byte arrived is retried too."""
    ctx.scenario("fail_times=1,fail_mode=close,text=second+time+lucky")
    s = ctx.spawn(**RETRY)
    s.submit("say hi")
    s.wait_text("second time lucky")
    s.wait_turn_done()
    # The notice wraps, and where it breaks is the wrapper's business.
    text = s.text()
    assert "retrying in 10ms (attempt" in text, text
    assert "2 of 4)" in text, text
    assert len(ctx.mock.requests) == 2, ctx.mock.requests


def test_empty_completion_is_retried(ctx):
    """Metadata-only success is not accepted as the assistant's answer."""
    ctx.scenario("empty_times=1,text=second+time+lucky")
    s = ctx.spawn(**RETRY)
    s.submit("say hi")
    s.wait_text("second time lucky")
    s.wait_turn_done()
    text = s.text()
    assert "empty response; retrying in 10ms (attempt" in text, text
    assert len(ctx.mock.requests) == 2, ctx.mock.requests


def test_empty_completions_are_exhausted(ctx):
    """Repeated empty successes become an error, not a blank assistant turn."""
    ctx.scenario("empty_times=9")
    s = ctx.spawn(ARQAN_RETRIES=1, ARQAN_RETRY_DELAY_MS=10)
    s.submit("say hi")
    s.wait_text("[provider error: the provider returned an empty response]")
    s.wait_turn_done()
    assert len(ctx.mock.requests) == 2, ctx.mock.requests


def test_stream_error_before_output_is_retried(ctx):
    """An SSE error object is a failure even though the HTTP request was 200."""
    ctx.scenario("stream_error_times=1,text=recovered")
    s = ctx.spawn(**RETRY)
    s.submit("say hi")
    s.wait_text("recovered")
    s.wait_turn_done()
    text = s.text()
    assert "provider reported a stream error; retrying" in text, text
    assert len(ctx.mock.requests) == 2, ctx.mock.requests


def test_retries_are_exhausted(ctx):
    """Past the last attempt the turn ends on the error and the UI stays usable."""
    ctx.scenario("fail_times=9")
    s = ctx.spawn(ARQAN_RETRIES=2, ARQAN_RETRY_DELAY_MS=10)
    s.submit("say hi")
    s.wait_text("[provider error: HTTP 503]")
    s.wait_turn_done()
    text = s.text()
    assert "(attempt 2 of 3)" in text, text
    assert "(attempt 3 of 3)" in text, text
    assert len(ctx.mock.requests) == 3, ctx.mock.requests
    s.type("still alive").sync()
    assert s.composer_text() == "still alive", s.composer_lines()


def test_partial_stream_is_not_retried(ctx):
    """Bytes already on screen cannot be taken back, so the turn ends instead."""
    ctx.scenario("text=one+two+three+four+five,chunk=1,abort_after=2,delay=0.01")
    s = ctx.spawn(**RETRY)
    s.submit("say hi")
    s.wait_text("[provider error:")
    s.wait_turn_done()
    text = s.text()
    assert "one two" in text, text
    assert "retrying in" not in text, text
    assert len(ctx.mock.requests) == 1, ctx.mock.requests


def test_retry_wait_is_interruptible(ctx):
    """Ctrl-C during the backoff ends the turn instead of waiting it out."""
    ctx.scenario("fail_times=9")
    s = ctx.spawn(ARQAN_RETRIES=3, ARQAN_RETRY_DELAY_MS=5000)
    s.submit("say hi")
    s.wait_text("; retrying in 5.0s")
    s.signal(signal.SIGINT)
    s.wait_text("[interrupted]")
    s.wait_turn_done()
    assert len(ctx.mock.requests) == 1, ctx.mock.requests
