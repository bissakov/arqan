"""Commands submitted while the assistant is working.

A turn holds the composer but not the session: the commands that touch
neither the conversation nor the request in flight run where they stand, and
the ones that would change what the agent is doing wait for the prompt.
"""

import json

PLAN = "## Steps\n\n1. Read the file\n2. Change the line"


def running_turn(ctx, spec="first_delay=6,text=done"):
    """A session with a turn slow enough to type a command into."""
    ctx.scenario(spec)
    s = ctx.spawn()
    s.submit("go on")
    s.wait_activity("thinking")
    return s


def test_settings_opens_while_a_turn_streams(ctx):
    """The screen comes up mid-turn and the reply arrives behind it."""
    s = running_turn(ctx)
    s.submit("/settings")
    s.wait_text("Verbose tool output")
    assert "Stream replies" in s.text(), s.text()
    s.wait_text("done")                 # the turn ran on under the screen
    s.key("esc").sync()
    s.wait_gone("Verbose tool output")
    s.wait_turn_done()
    assert len(ctx.mock.requests) == 1, "the turn was not restarted"


def test_a_look_setting_applies_without_disturbing_the_reply(ctx):
    """Verbose tool output is a way of reading the transcript, not a request
    setting, so it changes mid-turn and the streamed reply survives it."""
    ctx.scenario('tool=bash:{"command":"echo hi"},first_delay=4,'
                 'final_text=finished')
    s = ctx.spawn()
    s.submit("run it")
    s.wait_text("echo hi")
    s.submit("/settings")
    s.wait_text("Verbose tool output")
    s.settings_select("Verbose tool output").key("enter").sync()
    s.wait_text("[x] Verbose tool output")
    s.key("esc").sync()
    s.wait_turn_done()
    assert "finished" in s.text(), s.text()
    assert "echo hi" in s.text(), "the tool call outlived the re-render"


def test_a_request_setting_is_refused_until_the_turn_ends(ctx):
    """Tools, mode, streaming and the token limits are what the running turn
    reads, so the screen says no rather than moving the agent's ground."""
    s = running_turn(ctx)
    s.submit("/settings")
    s.wait_text("Stream replies")
    s.settings_select("Stream replies").key("enter").sync()
    s.wait_text("shapes the request")
    assert "[x] Stream replies" in s.text(), "the setting changed anyway"
    s.key("esc").sync()
    s.wait_turn_done()
    # At the prompt the same row answers.
    s.open_settings()
    s.settings_select("Stream replies").key("enter").sync()
    s.wait_text("[ ] Stream replies")


def test_copy_takes_the_reply_the_last_turn_left(ctx):
    """Reading the conversation is not writing to it, so /copy answers while
    the next turn is still running."""
    ctx.scenario("text=first+answer")
    s = ctx.spawn()
    s.submit("say something")
    s.wait_text("first answer")
    s.wait_turn_done()
    ctx.scenario("first_delay=6,text=second")
    s.submit("say more")
    s.wait_activity("thinking")
    s.submit("/copy")
    s.wait_text("copied the last response")
    assert s.screen.clipboard == "first answer", repr(s.screen.clipboard)
    s.wait_turn_done()


def test_a_command_that_changes_the_turn_waits_for_the_prompt(ctx):
    """/clear would drop the conversation the turn is still writing to, so it
    is refused and handed back to the composer, ready for the next Enter."""
    s = running_turn(ctx)
    s.type("/clear")
    s.key("enter")
    s.wait_text("waits until the turn ends")
    s.wait_for(lambda t: s.composer_text() == "/clear", "the command to stay")
    s.wait_turn_done()
    s.key("enter")
    s.wait_gone("go on")


def test_an_unknown_command_says_so_mid_turn(ctx):
    """A line the agent never offers is answered the way the prompt answers
    it, rather than sitting there as if it had run."""
    s = running_turn(ctx)
    s.type("/nope")
    s.key("enter")
    s.wait_text("unknown command: /nope")
    s.wait_turn_done()


def test_a_message_typed_mid_turn_still_waits_in_the_composer(ctx):
    """Only commands are submitted early: a message belongs to the next turn
    and Enter leaves it where it was typed."""
    s = running_turn(ctx)
    s.type("and then this")
    s.key("enter")
    s.wait_for(lambda t: s.composer_text() == "and then this",
               "the message to stay")
    s.wait_turn_done()
    assert len(ctx.mock.requests) == 1, "the message was sent mid-turn"


def test_a_question_from_the_turn_takes_the_screen_back(ctx):
    """The agent loop is waiting on the answer, so a plan handover closes the
    screen the user left open rather than being refused by it."""
    ctx.scenario("first_delay=4,tool=submit_plan:"
                 + json.dumps({"plan": PLAN}))
    s = ctx.spawn()
    s.key("shift-tab")
    s.wait_for(lambda t: s.status_field(2) == "plan", "plan mode")
    s.submit("plan the change")
    s.wait_activity("thinking")
    s.submit("/settings")
    s.wait_text("Verbose tool output")
    s.wait_status("continue?")
    assert "Verbose tool output" not in s.text(), s.text()
    assert "Yes, but from a new session" in s.text(), s.text()


def test_a_screen_left_open_survives_the_end_of_the_turn(ctx):
    """The prompt waits behind it rather than reading the keys meant for it,
    so the row under the cursor is still the row being acted on."""
    s = running_turn(ctx)
    s.submit("/settings")
    s.wait_text("Verbose tool output")
    s.wait_text("done")                 # the turn ended under the screen
    s.settings_select("Display raw").key("enter").sync()
    s.wait_text("[x] Display raw")
    s.key("esc").sync()
    s.wait_gone("Verbose tool output")
    s.wait_turn_done()
    s.open_settings()
    assert "[x] Display raw" in s.text(), s.text()
