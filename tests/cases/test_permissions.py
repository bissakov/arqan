"""Ask/Free approval policy for assistant-issued effectful tools."""

import json
import re


def write_call(path="guarded.txt", content="changed") -> str:
    args = json.dumps({"path": path, "content": content})
    return f"tool=write:{args}"


def test_ask_approves_one_guarded_call(ctx):
    """Yes runs the rendered call and lets the model continue."""
    ctx.scenario(write_call() + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("write it")
    s.wait_status("allow write?")

    assert "write guarded.txt" in s.text(), s.text()
    assert "Yes and remember" in s.text(), s.text()
    s.key("enter")
    s.wait_text("done")
    s.wait_turn_done()

    assert (ctx.work / "guarded.txt").read_text() == "changed"
    assert len(ctx.mock.requests) == 2, ctx.mock.requests


def test_no_denies_without_running_and_the_model_can_adjust(ctx):
    """No appends a stable denial result and continues the provider loop."""
    ctx.scenario(write_call() + ",final_text=understood")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("write it")
    s.wait_status("allow write?")
    s.key("down", "down").sync()
    s.key("enter")
    s.wait_text("understood")
    s.wait_turn_done()

    assert not (ctx.work / "guarded.txt").exists()
    assert ctx.mock.tool_results() == [
        "DENIED: the user did not approve this write call. Do not retry it blindly."
    ]


def test_yes_and_remember_grants_the_class_for_the_process(ctx):
    """A remembered write grant avoids a second picker in a later turn."""
    ctx.scenario(write_call("one.txt") + ",final_text=first")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("first")
    s.wait_status("allow write?")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("first")
    s.wait_turn_done()
    s.submit("/clear")
    s.wait_status("ready")

    ctx.scenario(write_call("two.txt") + ",final_text=second")
    s.submit("second")
    s.wait_text("second")
    s.wait_turn_done()
    assert (ctx.work / "one.txt").exists()
    assert (ctx.work / "two.txt").exists()


def test_escape_denies_the_call(ctx):
    ctx.scenario(write_call() + ",final_text=understood")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("write it")
    s.wait_status("allow write?")
    s.key("esc")
    s.wait_text("understood")
    s.wait_turn_done()
    assert not (ctx.work / "guarded.txt").exists()


def test_free_runs_guarded_calls_without_a_picker(ctx):
    ctx.scenario(write_call() + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="free")
    s.submit("write it")
    s.wait_text("done")
    s.wait_turn_done()
    assert (ctx.work / "guarded.txt").exists()


def test_user_shell_is_direct_even_in_plan_mode(ctx):
    """A user-entered bang command bypasses tool approvals and mode tools."""
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.key("shift-tab")
    s.wait_for(lambda t: s.status_field(2) == "plan", "plan mode")
    s.submit("!printf direct > user-shell.txt")
    s.wait_text("exit 0")
    assert (ctx.work / "user-shell.txt").read_text() == "direct"


def test_one_shot_ask_denies_and_stops(ctx):
    """Headless Ask never runs effects or asks the provider to continue."""
    ctx.scenario(write_call() + ",final_text=must+not+appear")
    out = ctx.run_cli("-p", "write it", ARQAN_PERMISSIONS="ask")

    assert out.returncode == 1, out
    assert out.stdout == "", out.stdout
    assert "approval required for assistant write" in out.stderr, out.stderr
    assert not (ctx.work / "guarded.txt").exists()
    assert len(ctx.mock.requests) == 1, ctx.mock.requests


def test_project_cannot_switch_permissions_to_free(ctx):
    """A cloned repository cannot disable approvals."""
    ctx.write_project_config("permissions = free\n")
    ctx.scenario(write_call())
    out = ctx.run_cli("-p", "write it", ARQAN_PERMISSIONS=None)

    assert out.returncode == 1, out
    assert "permissions" in out.stderr and "may not set it" in out.stderr
    assert not (ctx.work / "guarded.txt").exists()


def test_default_policy_and_status_field_are_ask(ctx):
    """Without an override, Ask is visible in the appended status field."""
    s = ctx.spawn(ARQAN_PERMISSIONS=None, ARQAN_STATUS_FIELDS=None)
    assert s.status_line().endswith("ask"), s.status_line()
    s.submit("/statusline")
    s.wait_status("status line")
    s.settings_select("Permissions")
    assert "[x] Permissions" in s.popup_selected(), s.text()


def test_settings_remembers_the_policy(ctx):
    """Permissions uses the normal state setting path."""
    s = ctx.spawn(ARQAN_PERMISSIONS=None)
    s.open_settings().settings_select("Permissions")
    assert s.settings_option("Permissions") == "Ask", s.text()
    s.key("right").sync()
    assert s.settings_option("Permissions") == "Free", s.text()
    assert ctx.state()["permissions"] == "free", ctx.state()


# ---- reads outside the project ---------------------------------------------


OUTSIDE_PROMPT = "allow read outside the project?"


def outside_file(ctx, name="secret.txt", body="outside body\n"):
    p = ctx.tmp / "elsewhere" / name
    p.parent.mkdir(exist_ok=True)
    p.write_text(body)
    return p


def read_call(path) -> str:
    return "tool=read:" + json.dumps({"path": str(path)})


def test_an_outside_read_asks_and_no_denies_it(ctx):
    """The model can pair read with page_fetch, so a file outside the
    project is the user's to hand over."""
    p = outside_file(ctx)
    ctx.scenario(read_call(p) + ",final_text=understood")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("read it")
    s.wait_status(OUTSIDE_PROMPT)
    assert "Read this path" in s.text(), s.text()
    s.key("down", "down").sync()
    s.key("enter")
    s.wait_text("understood")
    s.wait_turn_done()
    results = ctx.mock.tool_results()
    assert results[-1].startswith("DENIED:"), results
    assert "outside body" not in results[-1], results


def test_yes_and_remember_allows_later_outside_reads(ctx):
    one = outside_file(ctx, "one.txt", "first body\n")
    two = outside_file(ctx, "two.txt", "second body\n")
    ctx.scenario(read_call(one) + ",final_text=first")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("first")
    s.wait_status(OUTSIDE_PROMPT)
    assert "Allow reads outside the project" in s.text(), s.text()
    s.key("down").sync()
    s.key("enter")
    s.wait_text("first")
    s.wait_turn_done()
    assert "first body" in ctx.mock.tool_results()[-1]

    ctx.scenario(read_call(two) + ",final_text=second")
    s.submit("second")
    s.wait_text("second")
    s.wait_turn_done()
    assert "second body" in ctx.mock.tool_results()[-1]


def test_grep_and_find_outside_the_project_ask(ctx):
    p = outside_file(ctx)
    for name, args in (("grep", {"pattern": "outside", "path": str(p.parent)}),
                       ("find", {"name": "*.txt", "path": str(p.parent)})):
        ctx.scenario(f"tool={name}:{json.dumps(args)},final_text=answered")
        s = ctx.spawn(ARQAN_PERMISSIONS="ask")
        s.submit("look")
        s.wait_status(OUTSIDE_PROMPT)
        s.key("esc")
        s.wait_text("answered")
        s.wait_turn_done()
        assert ctx.mock.tool_results()[-1].startswith("DENIED:"), name
        s.close()


def test_reads_inside_the_project_do_not_ask(ctx):
    """A relative path, a missing one, and a link that stays inside all
    resolve under the working directory."""
    ctx.write_file("src/inside.txt", "inside body\n")
    (ctx.work / "link.txt").symlink_to(ctx.work / "src" / "inside.txt")
    ctx.scenario(
        read_call("src/inside.txt")
        + "," + read_call(ctx.work / "link.txt")
        + "," + read_call("src/missing/deeper.txt")
        + ',tool=grep:{"pattern":"inside"}'
        + ',tool=find:{"name":"*.txt","path":"src"}'
        + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("read")
    s.wait_text("done")
    s.wait_turn_done()
    results = ctx.mock.tool_results()
    assert "inside body" in results[0], results
    assert "inside body" in results[1], results
    assert not any(r.startswith("DENIED:") for r in results), results


def test_a_link_that_leaves_the_project_asks(ctx):
    p = outside_file(ctx)
    (ctx.work / "escape.txt").symlink_to(p)
    ctx.scenario(read_call("escape.txt") + ",final_text=understood")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("read it")
    s.wait_status(OUTSIDE_PROMPT)
    s.key("esc")
    s.wait_text("understood")
    s.wait_turn_done()
    assert ctx.mock.tool_results()[-1].startswith("DENIED:")


def test_a_spill_file_named_by_a_result_is_readable_without_asking(ctx):
    """The agent wrote it, and the result told the model to read it."""
    tmp = ctx.tmp / "spill"
    tmp.mkdir()
    ctx.scenario('tool=find:{"name":"*.txt","limit":1},final_text=listed')
    for i in range(30):
        ctx.write_file(f"f{i:02}.txt", "x\n")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask", TMPDIR=str(tmp))
    s.submit("list")
    s.wait_text("listed")
    s.wait_turn_done()
    m = re.search(r"\[full output: (\S+?),", ctx.mock.tool_results()[-1])
    assert m, ctx.mock.tool_results()[-1]
    ctx.scenario(read_call(m.group(1)) + ",final_text=read+back")
    s.submit("read the rest")
    s.wait_text("read back")
    s.wait_turn_done()
    assert "f29.txt" in ctx.mock.tool_results()[-1], ctx.mock.tool_results()


def test_free_reads_outside_without_asking(ctx):
    p = outside_file(ctx)
    ctx.scenario(read_call(p) + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="free")
    s.submit("read it")
    s.wait_text("done")
    s.wait_turn_done()
    assert "outside body" in ctx.mock.tool_results()[-1]


def test_one_shot_ask_denies_an_outside_read(ctx):
    p = outside_file(ctx)
    ctx.scenario(read_call(p) + ",final_text=must+not+appear")
    out = ctx.run_cli("-p", "read it", ARQAN_PERMISSIONS="ask")
    assert out.returncode == 1, out
    assert "approval required for assistant read outside the project" \
        in out.stderr, out.stderr
    assert len(ctx.mock.requests) == 1, ctx.mock.requests
