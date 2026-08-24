"""Starting in the newest session, and /restart.

`resume_last` decides what a start does with the sessions this directory
already holds: nothing, which is the welcome screen, or reopen the newest.
A directory the user cleared is greeted again whatever the setting says,
until a message gives the next start something to come back to.
/restart hands the terminal to a fresh process, which reads that setting
again.
"""

from tests.context import wait_until

from .test_resume import record, sessions_dir

WELCOME_ART = "| (_| | | | (_| |"


def test_a_start_greets_while_resuming_is_off(ctx):
    """The default leaves a saved session where it is."""
    record(ctx, "older question", "older answer")

    s = ctx.spawn()
    assert WELCOME_ART in s.text(), s.text()
    assert "older answer" not in s.text(), s.text()


def test_resume_last_reopens_the_newest_session(ctx):
    """With it on, the transcript is back before the first keystroke."""
    record(ctx, "older question", "older answer")

    s = ctx.spawn(ARQAN_RESUME_LAST="true")
    s.wait_text("older answer")
    assert "older question" in s.text(), s.text()
    assert WELCOME_ART not in s.text(), "a transcript replaces the welcome"
    assert s.status_kind() == "ready", s.status_line()


def test_resume_last_with_nothing_saved_still_greets(ctx):
    """A first run in a directory has no session to reopen."""
    s = ctx.spawn(ARQAN_RESUME_LAST="true")
    assert WELCOME_ART in s.text(), s.text()


def test_a_cleared_directory_greets_the_next_start(ctx):
    """/clear is putting the conversation away, so it stays away."""
    ctx.scenario("text=an+answer")
    s = ctx.spawn(ARQAN_RESUME_LAST="true")
    s.submit("a question")
    s.wait_text("an answer")
    s.wait_turn_done()
    s.submit("/clear")
    s.wait_gone("an answer")
    s.submit("/exit")
    s.wait_exit()

    again = ctx.spawn(ARQAN_RESUME_LAST="true")
    assert WELCOME_ART in again.text(), again.text()
    assert "a question" not in again.text(), again.text()


def test_a_message_after_clearing_is_resumed_again(ctx):
    """The new conversation is what the next start comes back to."""
    ctx.scenario("text=first+answer")
    s = ctx.spawn(ARQAN_RESUME_LAST="true")
    s.submit("first question")
    s.wait_text("first answer")
    s.wait_turn_done()
    s.submit("/clear")
    s.wait_gone("first answer")
    ctx.scenario("text=second+answer")
    s.submit("second question")
    s.wait_text("second answer")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    again = ctx.spawn(ARQAN_RESUME_LAST="true")
    again.wait_text("second answer")
    assert "first answer" not in again.text(), again.text()


def test_reopening_a_session_by_hand_undoes_a_clear(ctx):
    """Picking a session is choosing it, so the next start reopens it."""
    record(ctx, "older question", "older answer")

    s = ctx.spawn(ARQAN_RESUME_LAST="true")
    s.wait_text("older answer")
    s.submit("/clear")
    s.wait_gone("older answer")
    s.submit("/resume")
    s.wait_text("pick a session")
    s.key("enter")
    s.wait_text("older answer")
    s.submit("/exit")
    s.wait_exit()

    again = ctx.spawn(ARQAN_RESUME_LAST="true")
    again.wait_text("older answer")


def test_a_reopened_session_continues_its_file_and_its_history(ctx):
    """It is the same session, not a copy: one file, and the model sees it."""
    record(ctx, "one", "two")
    saved = sorted(p.name for p in sessions_dir(ctx).iterdir())

    s = ctx.spawn(ARQAN_RESUME_LAST="true")
    s.wait_text("two")
    ctx.scenario("text=four")
    s.submit("three")
    s.wait_text("four")
    s.wait_turn_done()

    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == [
        "system", "user", "assistant", "user"
    ], messages
    assert messages[1]["content"] == "one"
    assert messages[3]["content"] == "three"

    s.submit("/exit")
    s.wait_exit()
    assert sorted(p.name for p in sessions_dir(ctx).iterdir()) == saved
    contents = (sessions_dir(ctx) / saved[0]).read_text()
    assert contents.count('"role":"user"') == 2, contents


def test_the_setting_is_a_row_of_the_settings_screen(ctx):
    """The box is off by default and the choice is remembered."""
    s = ctx.spawn()
    s.open_settings().settings_select("Resume last session")
    assert "[ ] Resume last session" in s.popup_selected(), s.text()

    s.key("space").sync()
    s.wait_text("[x] Resume last session")
    wait_until(lambda: ctx.state().get("resume_last") == "true",
               "resume_last in the state file")


def test_a_remembered_choice_is_what_the_next_start_reads(ctx):
    """Turning it on in one session reopens it in the next."""
    ctx.scenario("text=an+answer")
    s = ctx.spawn()
    s.submit("a question")
    s.wait_text("an answer")
    s.wait_turn_done()
    s.settings_toggle("Resume last session")
    s.submit("/exit")
    s.wait_exit()

    again = ctx.spawn()
    again.wait_text("an answer")
    assert "a question" in again.text(), again.text()


def test_restart_greets_again_while_resuming_is_off(ctx):
    """The new process starts clean, and the next turn is a new session."""
    ctx.scenario("text=first+answer")
    s = ctx.spawn()
    s.submit("first question")
    s.wait_text("first answer")
    s.wait_turn_done()
    files = sorted(p.name for p in sessions_dir(ctx).iterdir())

    s.submit("/restart")
    s.wait_gone("first answer")
    s.wait_for(lambda t: t.contains(WELCOME_ART), "the welcome screen")

    ctx.scenario("text=second+answer")
    s.submit("second question")
    s.wait_text("second answer")
    s.wait_turn_done()
    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == ["system", "user"], messages

    s.submit("/exit")
    s.wait_exit()
    now = sorted(p.name for p in sessions_dir(ctx).iterdir())
    assert len(now) == len(files) + 1, now


def test_restart_reopens_the_session_while_resuming_is_on(ctx):
    """A restart mid-conversation lands back in it."""
    ctx.scenario("text=first+answer")
    s = ctx.spawn(ARQAN_RESUME_LAST="true")
    s.submit("first question")
    s.wait_text("first answer")
    s.wait_turn_done()

    # A notice the transcript does not hold marks the process it was printed
    # in, so the restart is observable while the conversation is unchanged.
    s.submit("/nosuchcommand")
    s.wait_text("unknown command")
    s.submit("/restart")
    s.wait_gone("unknown command")
    s.wait_text("first answer")
    assert "first question" in s.text(), s.text()
    assert s.status_kind() == "ready", s.status_line()


def test_restart_after_clearing_greets(ctx):
    """The fresh process reads the same mark a fresh launch does."""
    ctx.scenario("text=first+answer")
    s = ctx.spawn(ARQAN_RESUME_LAST="true")
    s.submit("first question")
    s.wait_text("first answer")
    s.wait_turn_done()

    s.submit("/clear")
    s.wait_gone("first answer")
    s.submit("/restart")
    s.wait_for(lambda t: t.contains(WELCOME_ART), "the welcome screen")
    assert "first answer" not in s.text(), s.text()


def test_restart_is_offered_as_a_command(ctx):
    """It is in the popup, so it is discoverable like the rest."""
    s = ctx.spawn()
    s.type("/rest").sync()
    s.wait_text("/restart")
    assert "Restart arqan" in s.text(), s.text()
    s.key("esc")
