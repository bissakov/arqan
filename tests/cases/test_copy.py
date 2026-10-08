"""'/copy': the last reply on the clipboard, as the Markdown the model wrote."""

from tests.context import wait_until

MARKDOWN = "# Title\\n\\n- one\\n- two\\n\\n`code`"
MARKDOWN_TEXT = "# Title\n\n- one\n- two\n\n`code`"


def test_copy_puts_the_last_reply_on_the_clipboard(ctx):
    """The clipboard holds the reply's source, newlines and markup included."""
    ctx.scenario(f"text={MARKDOWN}")
    s = ctx.spawn()
    s.submit("write some markdown")
    s.wait_text("two")
    s.wait_turn_done()
    s.submit("/copy")
    s.wait_text("copied the last response")
    assert s.screen.clipboard == MARKDOWN_TEXT, repr(s.screen.clipboard)


def test_copy_takes_the_source_not_the_rendered_rows(ctx):
    """A reply wider than the screen is copied unwrapped."""
    long_line = "+".join(f"word{i:02d}" for i in range(40))
    ctx.scenario(f"text={long_line}")
    s = ctx.spawn()
    s.submit("say a long line")
    s.wait_text("word39")
    s.wait_turn_done()
    s.submit("/copy")
    s.wait_text("copied the last response")
    assert s.screen.clipboard == long_line.replace("+", " "), repr(
        s.screen.clipboard
    )


def test_copy_skips_tool_calls(ctx):
    """Tool-call slots carry JSON arguments, so the answer is what is copied."""
    ctx.write_file("notes.txt", "hello from the file\n")
    ctx.scenario('tool=read:{"path":"notes.txt"},final_text=I+read+it')
    s = ctx.spawn()
    s.submit("read the notes")
    s.wait_text("I read it")
    s.wait_turn_done()
    s.submit("/copy")
    s.wait_text("copied the last response")
    assert s.screen.clipboard == "I read it", repr(s.screen.clipboard)


def test_copy_without_a_reply_answers_in_the_popup_slot(ctx):
    """Nothing said yet leaves the view as it was and says so above the composer."""
    s = ctx.spawn()
    assert "| (_| | | | (_| |" in s.text(), "the welcome art starts on screen"
    s.submit("/copy")
    s.wait_text("no response to copy")
    rows = s.screen.lines()
    assert "no response to copy" in rows[s.screen.rows - 6], rows[s.screen.rows - 8 :]
    assert "| (_| | | | (_| |" in s.text(), s.text()
    assert s.screen.clipboard is None or s.screen.clipboard == "", (
        repr(s.screen.clipboard)
    )


def test_copy_acknowledges_on_the_status_line(ctx):
    """The status line reports the copy, as it does for a drag-select."""
    ctx.scenario("text=alpha+beta")
    s = ctx.spawn()
    s.submit("say something")
    s.wait_text("alpha beta")
    s.wait_turn_done()
    s.submit("/copy")
    s.wait_text("copied the last response")
    assert "copied" in s.status_line(), s.status_line()


def test_copy_is_not_part_of_the_conversation(ctx):
    """The command never reaches the provider or the transcript."""
    ctx.scenario("text=alpha+beta")
    s = ctx.spawn()
    s.submit("say something")
    s.wait_text("alpha beta")
    s.wait_turn_done()
    s.submit("/copy")
    s.wait_text("copied the last response")
    rows = s.screen.lines()
    transcript = "\n".join(rows[: s.screen.rows - 6])
    assert "/copy" not in transcript, s.text()
    sent = [m["content"] for r in ctx.mock.requests for m in r.get("messages", [])]
    assert not any("/copy" in str(c) for c in sent), sent


def test_copy_after_clear_has_nothing_to_copy(ctx):
    """'/clear' drops the conversation, so there is no last reply left."""
    ctx.scenario("text=alpha+beta")
    s = ctx.spawn()
    s.submit("say something")
    s.wait_text("alpha beta")
    s.wait_turn_done()
    s.submit("/clear")
    s.wait_gone("alpha beta")
    s.submit("/copy")
    s.wait_text("no response to copy")


TMUX = "/tmp/tmux-1000/default,4242,0"


def fake_tmux(ctx, exit_code: int = 0) -> dict:
    """Env that puts a stub `tmux` first on PATH and points TMUX at a server.

    The stub records its arguments in `tmux.args` and its stdin in
    `tmux.stdin` under the work directory, then exits with `exit_code`. A
    nonzero code stands for a tmux too old for `load-buffer -w`. The stub
    keeps a case from reaching the developer's own tmux server.
    """
    bindir = ctx.work / "tmuxbin"
    bindir.mkdir(exist_ok=True)
    script = bindir / "tmux"
    script.write_text(
        "#!/bin/sh\n"
        "PATH=/usr/bin:/bin\n"
        f'printf "%s\\n" "$@" > "{ctx.work}/tmux.args"\n'
        f'cat > "{ctx.work}/tmux.stdin"\n'
        f"exit {exit_code}\n"
    )
    script.chmod(0o755)
    return {"TMUX": TMUX, "PATH": f"{bindir}:/usr/bin:/bin"}


def tmux_received(ctx) -> tuple[str, str]:
    """What the stub tmux was called with: its arguments and its stdin."""
    args = ctx.work / "tmux.args"
    stdin = ctx.work / "tmux.stdin"
    wait_until(lambda: args.exists() and stdin.exists(), "the stub tmux ran")
    return args.read_text(), stdin.read_text()


def test_copy_under_tmux_hands_the_text_to_tmux(ctx):
    """tmux 3.7c drops an OSC 52 that follows the end of a synchronized
    frame, so under tmux the copy goes through `tmux load-buffer -w -`,
    which sets the clipboard whatever `set-clipboard` says."""
    ctx.scenario("text=alpha+beta")
    s = ctx.spawn(**fake_tmux(ctx))
    s.submit("say something")
    s.wait_text("alpha beta")
    s.wait_turn_done()
    s.submit("/copy")
    s.wait_text("copied the last response")
    assert tmux_received(ctx) == ("load-buffer\n-w\n-\n", "alpha beta")
    assert "set-clipboard" not in s.text(), s.text()
    assert s.screen.clipboard is None, repr(s.screen.clipboard)


def test_copy_under_an_older_tmux_names_the_option_that_carries_it(ctx):
    """When tmux refuses `load-buffer -w`, the copy falls back to OSC 52,
    which tmux drops by default, so the notice says what would make it
    land."""
    ctx.scenario("text=alpha+beta")
    s = ctx.spawn(**fake_tmux(ctx, exit_code=1))
    s.submit("say something")
    s.wait_text("alpha beta")
    s.wait_turn_done()
    s.submit("/copy")
    s.wait_text("set-clipboard on")
    assert "copied the last response" not in s.text(), s.text()
    assert s.screen.clipboard == "alpha beta", repr(s.screen.clipboard)


def test_copy_outside_tmux_keeps_its_plain_acknowledgement(ctx):
    """The caveat belongs to tmux alone; a bare terminal answers as before."""
    ctx.scenario("text=alpha+beta")
    s = ctx.spawn(TMUX=None)
    s.submit("say something")
    s.wait_text("alpha beta")
    s.wait_turn_done()
    s.submit("/copy")
    s.wait_text("copied the last response")
    assert "set-clipboard" not in s.text(), s.text()
    assert s.screen.clipboard == "alpha beta", repr(s.screen.clipboard)
