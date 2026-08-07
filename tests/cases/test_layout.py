"""Transcript spacing: every block is one blank row from the next."""


def transcript(s):
    """The transcript rows on screen, trimmed of the air around them."""
    rows = [s.screen.row_text(r).rstrip() for r in range(s.screen.rows)]
    end = next(i for i, r in enumerate(rows) if r.lstrip().startswith(("\u203a", "!")))
    rows = rows[: end - 1]
    while rows and not rows[0]:
        rows.pop(0)
    while rows and not rows[-1]:
        rows.pop()
    return rows


def check_gaps(s, boxed=()):
    """One blank row between blocks; two only under a user message, whose
    padding row sits between its text and that gap."""
    rows = transcript(s)
    run = 0
    for i, row in enumerate(rows):
        if not row:
            run += 1
            continue
        if run:
            above = rows[i - run - 1].strip()
            allowed = 2 if above in boxed or row.strip() in boxed else 1
            assert run == allowed, (
                f"{run} blank rows between {above!r} and {row.strip()!r}, "
                f"expected {allowed}\n" + "\n".join(rows)
            )
        run = 0
    return rows


def test_a_turn_keeps_one_blank_row_between_blocks(ctx):
    """Reasoning, two tool calls and the reply are spaced alike."""
    ctx.write_file("notes.txt", "written down\n")
    ctx.scenario(
        "reasoning=let+me+look,"
        'tool=read:{"path":"notes.txt"},'
        'tool=find:{"name":"notes"},'
        "final_text=that+is+all"
    )
    s = ctx.spawn()
    s.submit("what is in notes?")
    s.wait_text("that is all")
    s.wait_turn_done()
    check_gaps(s, boxed=["what is in notes?"])
    ctx.check_screen(s)


def test_a_shell_run_is_spaced_like_a_tool_call(ctx):
    """A '!' run and the turn after it keep the same rhythm."""
    ctx.scenario("text=after+the+shell")
    s = ctx.spawn()
    s.submit("!echo hello-from-shell")
    s.wait_text("hello-from-shell")
    s.wait_turn_done()
    s.submit("and now?")
    s.wait_text("after the shell")
    s.wait_turn_done()
    check_gaps(s, boxed=["and now?"])


def test_a_replay_is_spaced_like_the_live_turn(ctx):
    """A setting re-renders the conversation; the air comes out the same."""
    ctx.write_file("notes.txt", "written down\n")
    ctx.scenario('tool=read:{"path":"notes.txt"},final_text=I+read+it')
    s = ctx.spawn()
    s.submit("read the notes")
    s.wait_text("I read it")
    s.wait_turn_done()
    live = transcript(s)

    s.settings_toggle("Verbose tool output")
    s.settings_toggle("Verbose tool output")
    assert transcript(s) == live, "\n".join(transcript(s))


def test_a_reply_ending_in_blank_lines_does_not_stack_air(ctx):
    """Trailing newlines in a reply are the model's, not the layout's."""
    ctx.scenario("text=done.\\n\\n\\n\\n")
    s = ctx.spawn()
    s.submit("say it")
    s.wait_text("done.")
    s.wait_turn_done()
    s.submit("again")
    s.wait_text("again")
    s.sync()
    check_gaps(s, boxed=["say it", "again"])


def test_one_shot_output_is_a_reply_not_a_view(ctx):
    """Plain stdout opens on the reply and ends on one newline."""
    ctx.write_file("notes.txt", "written down\n")
    ctx.scenario('tool=read:{"path":"notes.txt"},final_text=I+read+it')
    out = ctx.run_cli("-p", "read the notes")
    assert out.returncode == 0, out
    assert not out.stdout.startswith("\n"), repr(out.stdout[:40])
    assert out.stdout.endswith("it\n"), repr(out.stdout[-40:])
    assert "\n\n\n" not in out.stdout, repr(out.stdout)


def test_a_full_transcript_keeps_off_the_composer(ctx):
    """A conversation taller than the view still ends on a blank row."""
    ctx.scenario("words=400,paragraphs=4,chunk=16")
    s = ctx.spawn()
    s.submit("write a lot")
    s.wait_turn_done()
    rows = [s.screen.row_text(r) for r in range(s.screen.rows)]
    composer = next(r for r, row in enumerate(rows) if "\u203a" in row)
    assert s.screen.attr_at(composer - 1, 2).bg == 236, "the composer's padding"
    gap = composer - 2
    assert not rows[gap].strip(), rows[gap]
    assert s.screen.attr_at(gap, 2).bg is None, "the gap is not the panel"
    assert rows[gap - 1].strip(), "the transcript should fill the view"
