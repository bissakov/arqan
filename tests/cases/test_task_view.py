"""The task view: a delegated task's own transcript, on Ctrl-O.

The transcript is a pure function of a conversation, so the screen can show
the delegate's instead of the parent's. While it does, the parent's writes are
dropped and rebuilt from `Conv` on the way back, which is the property most of
these cases are about.

The two sides are told apart the way `test_subagent.py` does it:
`ARQAN_SUBAGENT_MODEL=small` with a `small_model` of `mock:<scenario>` gives
the delegate a scenario of its own.
"""

import json


def a_small_tree(ctx):
    ctx.write_file("src/one.c", "int alpha(void);\n")
    ctx.write_file("notes.txt", "alpha is a letter\n")


def spawn(ctx, sub="text=the+cat+is+in+src/one.c", **env):
    env.setdefault("ARQAN_SUBAGENT_MODEL", "small")
    env.setdefault("ARQAN_SMALL_MODEL", f"mock:{sub}")
    env.setdefault("ARQAN_AUTO_TITLE", "false")
    return ctx.spawn(**env)


def delegate(prompt="find the cat", label="cats", **args):
    call = dict(args)
    if prompt is not None:
        call["prompt"] = prompt
    if label is not None:
        call["label"] = label
    return f"tool=task:{json.dumps(call)},final_text=done"


def parent_requests(ctx):
    return [r for r in ctx.mock.requests
            if not str(r.get("model", "")).startswith("mock:")]


def collect(prompt="find the cat", label="cats", task_id=1):
    """A parent that starts a task and waits for its report."""
    start = json.dumps({"prompt": prompt, "label": label})
    poll = json.dumps({"id": task_id, "wait_ms": 30000})
    return f"tool=task:{start},tool=task:{poll}"


# ---- the regression the detach mechanism exists for ------------------------


def test_the_parents_reply_written_behind_the_view_is_not_lost(ctx):
    """The screen shows one conversation at a time, and the other is rebuilt
    rather than remembered.

    The view goes up while the parent's own next request is held, so
    everything it writes afterwards lands while the task's transcript is the
    one on screen. Coming back has to show all of it.
    """
    ctx.scenario("hold_final=1,"
                 'tool=task:{"prompt":"find the cat","label":"cats"}'
                 ",final_text=done")
    s = spawn(ctx, sub="text=found+it")
    s.submit("delegate it")
    s.wait_for(lambda _: len(parent_requests(ctx)) >= 2,
               "the parent's next request")
    s.key("ctrl-o").sync()
    assert "task 1" in s.text(), s.text()

    ctx.mock.release()
    s.wait_turn_done()
    assert "done" not in s.text(), "the parent wrote onto the task's view"

    s.key("ctrl-o").sync()
    text = s.text()
    assert "done" in text, text
    assert "delegate it" in text, text
    assert "Ctrl-O returns" not in text, text


# ---- switching ------------------------------------------------------------


def test_ctrl_o_with_no_task_says_so_and_leaves_the_transcript_alone(ctx):
    ctx.scenario("text=hello+there")
    s = ctx.spawn(ARQAN_AUTO_TITLE="false")
    s.submit("say hi")
    s.wait_text("hello there")
    s.wait_turn_done()

    s.key("ctrl-o").sync()
    text = s.text()
    assert "no task has run in this conversation" in text, text
    assert "hello there" in text, text
    assert "say hi" in text, text


def test_ctrl_o_shows_the_tasks_own_conversation(ctx):
    """Its task prompt, its rounds and its tool blocks, none of which the
    parent's transcript ever carried."""
    a_small_tree(ctx)
    ctx.scenario(collect() + ",final_text=done")
    s = spawn(ctx, sub='tool=grep:{"pattern":"alpha"},tool_rounds=1,'
                       'final_text=found+it')
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    s.key("ctrl-o").sync()
    ctx.check_screen(s)


def test_ctrl_o_returns_and_the_parent_transcript_is_whole(ctx):
    a_small_tree(ctx)
    ctx.scenario(delegate())
    s = spawn(ctx, sub='tool=grep:{"pattern":"alpha"},final_text=found+it')
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    s.key("ctrl-o").sync()
    assert "find the cat" in s.text(), s.text()

    s.key("ctrl-o").sync()
    text = s.text()
    assert "delegate it" in text, text
    assert "done" in text, text
    assert "Ctrl-O returns" not in text, text


def test_the_task_command_does_what_the_key_does(ctx):
    ctx.scenario(delegate())
    s = spawn(ctx)
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    s.submit("/task")
    s.wait_text("find the cat")
    s.submit("/task")
    s.wait_text("delegate it")


# ---- the header -----------------------------------------------------------


def test_the_header_names_the_small_model_the_delegate_runs_on(ctx):
    """The answer to "what is subagent_model set to": it is in front of you
    whenever you look at the delegation."""
    ctx.scenario(delegate())
    s = spawn(ctx, sub="text=found+it")
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    s.key("ctrl-o").sync()
    text = s.text()
    assert "mock:text=found+it" in text, text
    assert "small model" in text, text


def test_the_header_names_the_main_model_when_the_delegate_runs_on_it(ctx):
    ctx.scenario(delegate())
    s = ctx.spawn(ARQAN_SUBAGENT_MODEL="main", ARQAN_AUTO_TITLE="false")
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    s.key("ctrl-o").sync()
    text = s.text()
    assert "mock-model" in text, text
    assert "small model" not in text, text


# ---- how long it stays reachable ------------------------------------------


def test_a_reported_task_is_still_reachable(ctx):
    ctx.scenario(collect() + ",final_text=done")
    s = spawn(ctx)
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    s.key("ctrl-o").sync()
    text = s.text()
    assert "find the cat" in text, text
    assert "finished" in text, text


def test_clear_drops_the_task_and_the_view_returns_with_it(ctx):
    """A conversation that no longer exists can never be left on screen."""
    ctx.scenario(delegate())
    s = spawn(ctx)
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    s.key("ctrl-o").sync()
    assert "find the cat" in s.text(), s.text()

    s.submit("/clear")
    s.wait_gone("find the cat")
    s.key("ctrl-o").sync()
    assert "no task has run in this conversation" in s.text(), s.text()


def test_a_second_task_replaces_the_first_ones_transcript(ctx):
    """One slot, one view: the delegation being watched is the current one."""
    ctx.scenario(collect("find the cat", "cats", 1) + ","
                 + collect("find the dog", "dogs", 2) + ",final_text=done")
    s = spawn(ctx, sub="text=found+it")
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    s.key("ctrl-o").sync()
    text = s.text()
    assert "find the dog" in text, text
    assert "find the cat" not in text, text
    assert "task 2" in text, text


# ---- re-rendering while the task view is the one showing -------------------


def test_the_view_fills_in_while_the_delegate_is_still_working(ctx):
    """The delegate runs in another process, so its rounds reach the view
    through the log the parent drains between keystrokes. Held on its second
    round, the first round's tool block is already on screen."""
    a_small_tree(ctx)
    ctx.scenario(collect() + ",final_text=done")
    s = spawn(ctx, sub='tool=grep:{"pattern":"alpha"},hold_round=2,'
                       'final_text=found+it')
    s.submit("delegate it")
    s.wait_for(lambda _: len(ctx.mock.requests) >= 3, "the delegate's round")
    s.key("ctrl-o").sync()

    s.wait_text("grep alpha")
    text = s.text()
    assert "in progress" in text, text
    assert "found it" not in text, text

    ctx.mock.release()
    s.wait_text("found it")
    s.key("ctrl-o").sync()
    s.wait_text("delegate it")


def test_a_reflow_rebuilds_the_task_view_rather_than_blanking_it(ctx):
    """Every rebuild clears the transcript first, so one aimed at the parent
    while the task is showing would wipe the screen and write nothing back."""
    a_small_tree(ctx)
    ctx.scenario(delegate())
    s = spawn(ctx, sub='tool=grep:{"pattern":"alpha"},final_text=found+it')
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    s.key("ctrl-o").sync()
    s.resize(70, 24)
    s.wait_text("find the cat")
    text = s.text()
    assert "Ctrl-O returns" in text, text
    assert "delegate it" not in text, text


def test_clicking_a_truncated_block_expands_it_inside_the_task_view(ctx):
    """Blocks are numbered by the conversation showing, so the click that
    reaches the delegate's grep must not land on a block of the parent."""
    for i in range(30):
        ctx.write_file(f"src/f{i:02}.c", "int alpha(void);\n")
    ctx.scenario(delegate())
    s = spawn(ctx, sub='tool=grep:{"pattern":"alpha"},final_text=found+it')
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    s.key("ctrl-o").sync()
    tail = "\u25be 18 more lines"
    row = s.screen.find_row(tail)
    assert row >= 0, s.text()
    s.mouse("down", row + 1, 6)
    s.mouse("up", row + 1, 6)
    s.wait_text("\u25b4 show less")
    text = s.text()
    assert "src/f29.c" in text, text
    assert "delegate it" not in text, "the click reached the parent instead"
