"""Sequential batches use the normal tools, approvals and transcript rendering."""

import json
import signal
import sys
from pathlib import Path


def write_mcp(ctx):
    server = str(Path(__file__).resolve().parents[1] / "harness" / "mcpserver.py")
    path = ctx.xdg / "arqan" / "mcp.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"servers": {"demo": {
        "command": sys.executable, "args": [server, "--mode", "ok"]}}}))


def step(tool, **args):
    return {"tool": tool, "args": args}


def batch_call(steps):
    return "batch:" + json.dumps({"steps": steps})


def run_batch(ctx, steps, **env):
    ctx.scenario("tool=" + batch_call(steps) + ",final_text=done")
    s = ctx.spawn(**env)
    s.submit("run the batch")
    s.wait_text("done")
    s.wait_turn_done()
    return s


def result(ctx, at=0):
    return json.loads(ctx.mock.tool_results()[at])


def batch_schema(ctx):
    tools = ctx.mock.requests[0]["tools"]
    found = [tool for tool in tools
             if tool.get("function", {}).get("name") == "batch"
             or tool.get("name") == "batch"]
    assert len(found) == 1, found
    return found[0].get("function", {}).get("parameters") or found[0]["input_schema"]


def saved_batch_results(ctx):
    root = ctx.home / ".local/share/arqan/sessions"
    records = [json.loads(line) for path in root.rglob("*.jsonl")
               for line in path.read_text().splitlines()]
    updates = [record for record in records
               if record.get("role") == "tool"
               or record.get("type") == "tool_update"]
    return records, updates


def test_batch_edits_then_reads_in_one_provider_round(ctx):
    ctx.write_file("edit.txt", "old\n")
    patch = "--- edit.txt\n+++ edit.txt\n@@\n-old\n+new\n"
    s = run_batch(ctx, [step("patch", patch=patch), step("read", path="edit.txt")])

    out = result(ctx)
    assert out["status"] == "completed", out
    assert out["attempted"] == 2 and out["skipped"] == 0, out
    assert [r["tool"] for r in out["steps"]] == ["patch", "read"], out
    assert out["steps"][1]["result"] == "new\n", out
    assert len(ctx.mock.requests) == 2, ctx.mock.requests
    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == ["system", "user", "assistant", "tool"]
    assert messages[2]["tool_calls"][0]["function"]["name"] == "batch"
    assert "\u25c6  patch edit.txt" in s.text(), s.text()
    assert "\u2502 -old" in s.text() and "\u2502 +new" in s.text(), s.text()
    assert "\u25c6  read edit.txt" in s.text(), s.text()
    assert '"steps"' not in s.text(), s.text()


def test_batch_write_creates_directories_then_reads(ctx):
    run_batch(ctx, [step("write", path="nested/new.txt", content="hello\n"),
                    step("read", path="nested/new.txt")])
    out = result(ctx)
    assert out["status"] == "completed", out
    assert out["steps"][1]["result"] == "hello\n", out
    assert (ctx.work / "nested/new.txt").read_text() == "hello\n"


def test_batch_grep_empty_result_is_summarised_once(ctx):
    ctx.write_file("notes.txt", "alpha\n")
    s = run_batch(ctx, [step("grep", pattern="absent", path="notes.txt")])

    text = s.text()
    assert text.count("\u2514\u2500 0 matches") == 1, text
    assert "no matches" not in text, text
    out = result(ctx)
    assert out["status"] == "completed", out
    assert out["steps"][0]["result"] == "no matches\n", out


def test_batch_children_have_a_rail_and_regular_tools_do_not(ctx):
    ctx.write_file("first.txt", "first output\n")
    ctx.write_file("second.txt", "second output\n")
    ctx.scenario("tool=" + batch_call([
        step("read", path="first.txt"), step("read", path="second.txt")])
        + ',tool=read:{"path":"first.txt"},final_text=done')
    s = ctx.spawn(rows=40)
    s.submit("run it")
    s.wait_text("done")
    s.wait_turn_done()
    rows = [line.strip() for line in s.text().splitlines()]
    head = rows.index("\u25c6  batch 2 steps")
    end = next(i for i, row in enumerate(rows)
               if row.startswith("\u2514\u2500 batch completed:"))
    assert all(row.startswith("\u2502") for row in rows[head + 1:end]), rows
    assert "\u2502  \u25c6  read first.txt" in rows, rows
    assert "\u2502  \u25c6  read second.txt" in rows, rows
    assert "\u2502     first output" in rows, rows
    assert "\u2502     second output" in rows, rows
    assert rows[end - 1] == "\u2502", rows
    assert "\u25c6  read first.txt" in rows[end + 1:], rows
    assert "first output" in rows[end + 1:], rows
    child = s.screen.find_row("\u2502  \u2514\u2500")
    rail_fg = s.screen.attr_at(child, 2).fg
    assert s.screen.attr_at(child, 5).fg == rail_fg
    assert s.screen.attr_at(child, 8).fg != rail_fg
    assert s.screen.attr_at(end, 2).fg == rail_fg
    assert s.screen.attr_at(end, 5).fg != rail_fg


def test_batch_rail_continues_on_wrapped_rows_after_resize(ctx):
    body = "wrap-" * 26
    ctx.write_file("wrapped.txt", body + "\n")
    s = run_batch(ctx, [step("read", path="wrapped.txt")])
    for cols in (50, 70):
        s.resize(cols, 40)
        s.settle()
        rows = [line.strip() for line in s.text().splitlines()]
        start = next(i for i, row in enumerate(rows)
                     if row.startswith("\u2502  \u2514\u2500 1 line"))
        end = next(i for i, row in enumerate(rows)
                   if row.startswith("\u2514\u2500 batch completed:"))
        wrapped = rows[start + 1:end - 1]
        assert len(wrapped) >= 2, rows
        assert all(row.startswith("\u2502  ") for row in wrapped), rows
        shown = "".join(row[3:].strip() for row in wrapped)
        assert shown == body, rows


def test_batch_failure_stops_later_steps_without_rolling_back(ctx):
    run_batch(ctx, [step("write", path="before.txt", content="keep\n"),
                    step("read", path="missing.txt"),
                    step("write", path="after.txt", content="never\n")])
    out = result(ctx)
    assert out["status"] == "stopped", out
    assert out["attempted"] == 2 and out["skipped"] == 1, out
    assert out["steps"][1]["status"] == "error", out
    assert out["steps"][1]["result"].startswith("ERROR:"), out
    assert (ctx.work / "before.txt").read_text() == "keep\n"
    assert not (ctx.work / "after.txt").exists()


def test_batch_nonzero_exit_stops_later_steps(ctx):
    run_batch(ctx, [step("bash", command="printf failed; exit 7"),
                    step("write", path="after.txt", content="never")])
    out = result(ctx)
    assert out["status"] == "stopped", out
    assert out["attempted"] == 1 and out["skipped"] == 1, out
    assert out["steps"][0]["exit_code"] == 7, out
    assert out["steps"][0]["status"] == "error", out
    assert not (ctx.work / "after.txt").exists()


def test_batch_does_not_infer_exit_status_from_stdout(ctx):
    run_batch(ctx, [step("bash", command="printf '[exit 9]\\n'"),
                    step("write", path="after.txt", content="yes")])
    assert result(ctx)["status"] == "completed", result(ctx)
    assert (ctx.work / "after.txt").read_text() == "yes"


def test_batch_does_not_infer_tool_failure_from_file_contents(ctx):
    ctx.write_file("input.txt", "ERROR: this is file content\n")
    run_batch(ctx, [step("read", path="input.txt"),
                    step("write", path="after.txt", content="yes")])
    assert result(ctx)["status"] == "completed", result(ctx)
    assert (ctx.work / "after.txt").read_text() == "yes"


def test_batch_detachment_stops_until_the_job_is_followed(ctx):
    ctx.scenario("tool=" + batch_call([
        step("bash", command="sleep 0.7; printf finished", timeout_ms=200),
        step("write", path="after.txt", content="never")])
        + ',tool=job:{"id":1},final_text=done')
    s = ctx.spawn()
    s.submit("run it")
    s.wait_text("done")
    s.wait_turn_done()

    out = result(ctx)
    assert out["status"] == "stopped", out
    assert out["attempted"] == 1 and out["skipped"] == 1, out
    assert out["steps"][0]["status"] == "pending", out
    assert out["steps"][0]["job_id"] == 1, out
    assert "finished" in ctx.mock.tool_results()[1], ctx.mock.tool_results()
    assert not (ctx.work / "after.txt").exists()


def test_batch_job_poll_stops_if_the_job_is_still_running(ctx):
    ctx.scenario('tool=bash:{"command":"sleep 3","timeout_ms":200},tool='
        + batch_call([step("job", id=1, action="poll"),
                      step("write", path="after.txt", content="never")])
        + ",final_text=done")
    s = ctx.spawn()
    s.submit("run it")
    s.wait_text("done")
    s.wait_turn_done()
    out = result(ctx, 1)
    assert out["status"] == "stopped", out
    assert out["steps"][0]["status"] == "pending", out
    assert out["steps"][0]["job_id"] == 1, out
    assert not (ctx.work / "after.txt").exists()


def test_batch_completed_job_with_nonzero_exit_stops(ctx):
    ctx.scenario('tool=bash:{"command":"sleep 0.6; exit 7","timeout_ms":200},tool='
        + batch_call([step("job", id=1, timeout_ms=5000),
                      step("write", path="after.txt", content="never")])
        + ",final_text=done")
    s = ctx.spawn()
    s.submit("run it")
    s.wait_text("done")
    s.wait_turn_done()
    out = result(ctx, 1)
    assert out["status"] == "stopped", out
    assert out["steps"][0]["exit_code"] == 7, out
    assert not (ctx.work / "after.txt").exists()


def test_batch_rejects_unknown_tools_before_any_step(ctx):
    run_batch(ctx, [step("write", path="before.txt", content="never"),
                    step("not_a_tool")])
    out = ctx.mock.tool_results()[0]
    assert out.startswith("ERROR:") and "step 2" in out, out
    assert not (ctx.work / "before.txt").exists()


def test_batch_rejects_disabled_children_before_any_step(ctx):
    run_batch(ctx, [step("write", path="before.txt", content="never"),
                    step("bash", command="true")], ARQAN_DISABLE_TOOLS="bash")
    out = ctx.mock.tool_results()[0]
    assert out.startswith("ERROR:") and "step 2" in out, out
    assert "bash" in out and "available" in out, out
    assert not (ctx.work / "before.txt").exists()


def test_batch_cannot_bypass_plan_mode(ctx):
    ctx.write_file("input.txt", "read only\n")
    run_batch(ctx, [step("read", path="input.txt"),
                    step("write", path="after.txt", content="never")], ARQAN_MODE="plan")
    out = ctx.mock.tool_results()[0]
    assert out.startswith("ERROR:") and "step 2" in out, out
    assert not (ctx.work / "after.txt").exists()


def test_batch_read_only_steps_are_available_in_plan_mode(ctx):
    ctx.write_file("input.txt", "read only\n")
    run_batch(ctx, [step("read", path="input.txt")], ARQAN_MODE="plan")
    out = result(ctx)
    assert out["status"] == "completed", out
    assert out["steps"][0]["result"] == "read only\n", out


def test_batch_can_be_disabled(ctx):
    run_batch(ctx, [step("write", path="after.txt", content="never")],
              ARQAN_DISABLE_TOOLS="batch")
    assert ctx.mock.tool_results()[0].startswith("ERROR:"), ctx.mock.tool_results()
    assert not (ctx.work / "after.txt").exists()


def test_batch_rejects_nested_batches_before_any_step(ctx):
    run_batch(ctx, [step("write", path="before.txt", content="never"),
                    step("batch", steps=[step("read", path="before.txt")])])
    out = ctx.mock.tool_results()[0]
    assert out.startswith("ERROR:") and "step 2" in out, out
    assert not (ctx.work / "before.txt").exists()


def test_batch_rejects_interactive_steps_before_any_step(ctx):
    run_batch(ctx, [step("write", path="before.txt", content="never"),
                    step("ask_user", question="proceed?", options=[])])
    out = ctx.mock.tool_results()[0]
    assert out.startswith("ERROR:") and "step 2" in out, out
    assert not (ctx.work / "before.txt").exists()


def test_batch_rejects_todo_steps_before_any_step(ctx):
    run_batch(ctx, [step("write", path="before.txt", content="never"),
                    step("todo", items=[])])
    out = ctx.mock.tool_results()[0]
    assert out.startswith("ERROR:") and "step 2" in out, out
    assert not (ctx.work / "before.txt").exists()


def test_batch_rejects_empty_step_lists(ctx):
    run_batch(ctx, [])
    assert ctx.mock.tool_results()[0].startswith("ERROR:"), ctx.mock.tool_results()


def test_batch_rejects_more_than_eight_steps(ctx):
    run_batch(ctx, [step("write", path=f"file-{i}.txt", content="never") for i in range(9)])
    out = ctx.mock.tool_results()[0]
    assert out.startswith("ERROR:") and "8" in out, out
    assert not list(ctx.work.glob("file-*.txt"))


def test_batch_rejects_non_object_child_arguments_before_any_step(ctx):
    run_batch(ctx, [step("write", path="before.txt", content="never"),
                    {"tool": "read", "args": "input.txt"}])
    out = ctx.mock.tool_results()[0]
    assert out.startswith("ERROR:") and "step 2" in out, out
    assert not (ctx.work / "before.txt").exists()


def test_batch_rejects_missing_required_child_arguments_before_any_step(ctx):
    run_batch(ctx, [step("write", path="before.txt", content="never"),
                    step("read")])
    out = ctx.mock.tool_results()[0]
    assert out.startswith("ERROR:") and "step 2 args.path is required" in out, out
    assert not (ctx.work / "before.txt").exists()


def test_batch_rejects_wrong_child_argument_types_before_any_step(ctx):
    run_batch(ctx, [step("write", path="before.txt", content="never"),
                    step("read", path=7)])
    out = ctx.mock.tool_results()[0]
    assert out.startswith("ERROR:") and "step 2 args.path must be a string" in out, out
    assert not (ctx.work / "before.txt").exists()


def test_batch_rejects_child_argument_bounds_before_any_step(ctx):
    run_batch(ctx, [step("write", path="before.txt", content="never"),
                    step("bash", command="true", timeout_ms=999999999)])
    out = ctx.mock.tool_results()[0]
    assert out.startswith("ERROR:") and "step 2 args.timeout_ms" in out, out
    assert "at most" in out, out
    assert not (ctx.work / "before.txt").exists()


def test_batch_schema_discriminates_child_tools_and_arguments(ctx):
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.submit("show tools")
    s.wait_text("ok")
    s.wait_turn_done()

    schema = batch_schema(ctx)
    choices = schema["properties"]["steps"]["items"]["oneOf"]
    by_name = {choice["properties"]["tool"]["const"]: choice for choice in choices}
    assert "batch" not in by_name
    assert "ask_user" not in by_name
    assert by_name["read"]["properties"]["args"]["required"] == ["path"]
    assert by_name["patch"]["properties"]["args"]["required"] == ["patch"]
    timeout = by_name["bash"]["properties"]["args"]["properties"]["timeout_ms"]
    assert timeout["maximum"] > 0


def test_batch_anthropic_schema_discriminates_children(ctx):
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_API="anthropic")
    s.submit("show tools")
    s.wait_text("ok")
    s.wait_turn_done()

    schema = batch_schema(ctx)
    choices = schema["properties"]["steps"]["items"]["oneOf"]
    names = {choice["properties"]["tool"]["const"] for choice in choices}
    assert "read" in names and "write" in names and "batch" not in names


def test_batch_plan_schema_excludes_build_tools(ctx):
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_MODE="plan")
    s.submit("show tools")
    s.wait_text("ok")
    s.wait_turn_done()

    schema = batch_schema(ctx)
    choices = schema["properties"]["steps"]["items"]["oneOf"]
    names = {choice["properties"]["tool"]["const"] for choice in choices}
    assert "read" in names
    assert "write" not in names and "bash" not in names


def test_batch_asks_for_each_guarded_child(ctx):
    patch = "--- edit.txt\n+++ edit.txt\n@@\n-old\n+new\n"
    ctx.scenario("tool=" + batch_call([
        step("write", path="edit.txt", content="old\n"),
        step("patch", patch=patch)]) + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("edit it")
    s.wait_status("allow write?")
    assert "write edit.txt" in s.text(), s.text()
    s.key("enter")
    s.wait_status("allow patch?")
    assert "patch edit.txt" in s.text(), s.text()
    s.key("enter")
    s.wait_text("done")
    s.wait_turn_done()
    assert result(ctx)["status"] == "completed", result(ctx)
    assert (ctx.work / "edit.txt").read_text() == "new\n"


def test_batch_denial_stops_later_steps(ctx):
    ctx.scenario("tool=" + batch_call([
        step("write", path="before.txt", content="never"),
        step("write", path="after.txt", content="never")]) + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("write it")
    s.wait_status("allow write?")
    s.key("esc")
    s.wait_text("done")
    s.wait_turn_done()
    out = result(ctx)
    assert out["steps"][0]["status"] == "denied", out
    assert out["attempted"] == 1 and out["skipped"] == 1, out
    assert not (ctx.work / "before.txt").exists()
    assert not (ctx.work / "after.txt").exists()


def test_batch_one_shot_ask_denies_without_followup(ctx):
    ctx.scenario("tool=" + batch_call([
        step("write", path="after.txt", content="never")]) + ",final_text=never")
    out = ctx.run_cli("-p", "run it", ARQAN_PERMISSIONS="ask")
    assert out.returncode == 1, out
    assert len(ctx.mock.requests) == 1, ctx.mock.requests
    assert not (ctx.work / "after.txt").exists()


def test_batch_reads_outside_the_project_require_approval(ctx):
    outside = ctx.tmp / "outside.txt"
    outside.write_text("secret\n")
    ctx.scenario("tool=" + batch_call([step("read", path=str(outside))]) + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("read it")
    s.wait_status("allow read outside the project?")
    s.key("esc")
    s.wait_text("done")
    s.wait_turn_done()
    out = result(ctx)
    assert out["steps"][0]["status"] == "denied", out
    assert "secret" not in json.dumps(out), out


def test_batch_mcp_children_use_the_normal_registry(ctx):
    write_mcp(ctx)
    run_batch(ctx, [step("demo_echo", text="ping"), step("demo_echo", text="pong")],
              ARQAN_MCP="on")
    out = result(ctx)
    assert out["status"] == "completed", out
    assert [r["result"] for r in out["steps"]] == ["echo: ping", "echo: pong"], out


def test_batch_mcp_errors_stop_later_steps(ctx):
    write_mcp(ctx)
    run_batch(ctx, [step("demo_boom"), step("write", path="after.txt", content="never")],
              ARQAN_MCP="on")
    out = result(ctx)
    assert out["status"] == "stopped", out
    assert "the server refused" in out["steps"][0]["result"], out
    assert not (ctx.work / "after.txt").exists()


def test_batch_output_keeps_each_tools_page_and_spill_note(ctx):
    run_batch(ctx, [step("bash", command="head -c 10000 /dev/zero") for _ in range(8)])
    out = result(ctx)
    assert out["status"] == "completed", out
    assert len(out["steps"]) == 8, out
    for row in out["steps"]:
        assert len(row["result"].encode()) <= 8192, row
        assert "[full output:" in row["result"], row["result"][-500:]
    assert len(ctx.mock.tool_results()[0].encode()) <= 400000


def test_batch_results_and_diffs_survive_session_replay(ctx):
    ctx.write_file("edit.txt", "old\n")
    patch = "--- edit.txt\n+++ edit.txt\n@@\n-old\n+new\n"
    s = run_batch(ctx, [step("patch", patch=patch), step("read", path="edit.txt")])
    s.submit("/exit")
    s.wait_exit()
    _, saved = saved_batch_results(ctx)
    assert len(saved) == 3, saved
    assert json.loads(saved[-1]["content"])["status"] == "completed"

    resumed = ctx.spawn()
    resumed.submit("/resume")
    resumed.wait_text("pick a session")
    resumed.key("enter")
    resumed.wait_gone("pick a session")
    resumed.settle()
    text = resumed.text()
    assert "\u25c6  patch edit.txt" in text, text
    assert "\u2502 -old" in text and "\u2502 +new" in text, text
    assert "\u25c6  read edit.txt" in text, text
    assert "\u2502  \u25c6  patch edit.txt" in text, text
    assert "\u2502  \u25c6  read edit.txt" in text, text
    assert "\u2502     new" in text, text
    assert '"steps"' not in text, text


def test_batch_in_progress_children_survive_a_resize(ctx):
    ctx.scenario("tool=" + batch_call([
        step("write", path="before.txt", content="kept\n"),
        step("bash", command="sleep 1.5; printf finished", timeout_ms=5000),
        step("read", path="before.txt")]) + ",final_text=done")
    s = ctx.spawn()
    s.submit("run it")
    s.wait_activity("running bash")
    s.resize(90, 40)
    s.wait_text("sleep 1.5; printf finished")
    assert "write before.txt" in s.text(), s.text()
    assert "\u2502  \u25c6  write before.txt" in s.text(), s.text()
    assert "\u2502  \u25c6  bash sleep 1.5; printf finished" in s.text(), s.text()
    s.wait_text("done")
    s.wait_turn_done()
    assert result(ctx)["status"] == "completed", result(ctx)


def test_batch_persists_progress_and_interrupt_stops_later_steps(ctx):
    ctx.scenario("tool=" + batch_call([
        step("write", path="before.txt", content="kept\n"),
        step("bash", command="sleep 30", timeout_ms=30000),
        step("write", path="after.txt", content="never")]))
    s = ctx.spawn()
    s.submit("run it")
    s.wait_activity("running bash")

    saved, updates = saved_batch_results(ctx)
    tools = [record for record in saved if record.get("role") == "tool"]
    assert len(tools) == 1, saved
    progress = json.loads(tools[0]["content"])
    assert progress["status"] == "running", progress
    assert progress["attempted"] == 1, progress
    assert progress["steps"][0]["status"] == "ok", progress

    s.signal(signal.SIGINT)
    s.wait_text("[interrupted]")
    s.wait_turn_done()
    assert (ctx.work / "before.txt").read_text() == "kept\n"
    assert not (ctx.work / "after.txt").exists()

    saved, updates = saved_batch_results(ctx)
    final = json.loads(updates[-1]["content"])
    assert final["status"] == "stopped", final
    assert final["attempted"] == 2 and final["skipped"] == 1, final
    assert final["steps"][1]["status"] == "error", final
    assert final["steps"][1]["exit_code"] == 130, final


def test_batch_anthropic_has_one_tool_use_and_one_result(ctx):
    ctx.write_file("input.txt", "hello\n")
    run_batch(ctx, [step("read", path="input.txt"),
                    step("write", path="new.txt", content="done\n")], ARQAN_API="anthropic")
    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == ["user", "assistant", "user"], messages
    uses = [b for b in messages[1]["content"] if b["type"] == "tool_use"]
    replies = [b for b in messages[2]["content"] if b["type"] == "tool_result"]
    assert len(uses) == 1 and uses[0]["name"] == "batch", uses
    assert len(replies) == 1 and replies[0]["tool_use_id"] == uses[0]["id"], replies
    assert result(ctx)["status"] == "completed", result(ctx)


def test_batch_rejects_conditional_steps_before_any_step(ctx):
    conditional = step("write", path="after.txt", content="never")
    conditional["if"] = "previous result is successful"
    run_batch(ctx, [step("write", path="before.txt", content="never"), conditional])
    out = ctx.mock.tool_results()[0]
    assert out.startswith("ERROR:") and "step 2" in out, out
    assert not (ctx.work / "before.txt").exists()
    assert not (ctx.work / "after.txt").exists()


def test_batch_rejects_unknown_top_level_options(ctx):
    args = {"steps": [step("write", path="after.txt", content="never")],
            "continue_on_error": True}
    ctx.scenario("tool=batch:" + json.dumps(args) + ",final_text=done")
    s = ctx.spawn()
    s.submit("run it")
    s.wait_text("done")
    s.wait_turn_done()
    assert ctx.mock.tool_results()[0].startswith("ERROR:"), ctx.mock.tool_results()
    assert not (ctx.work / "after.txt").exists()


def test_batch_output_window_shows_text_not_encoded_json(ctx):
    ctx.write_file("big.txt", "\n".join(f"line {i:04d} of output" for i in range(40)))
    ctx.scenario("tool=" + batch_call([step("read", path="big.txt")])
                 + ",hold_final,final_text=done")
    s = ctx.spawn()
    s.submit("read it")
    tail = "\u25be 28 more lines"
    s.wait_text(tail)
    s.settle()
    row = s.screen.find_row(tail) + 1
    s.mouse("down", row, 9).mouse("up", row, 9).sync()
    s.wait_text("batch output")
    s.wait_text("line 0012 of output")
    assert '"steps"' not in s.text(), s.text()


def test_batch_input_window_shows_content_not_encoded_json(ctx):
    body = "\n".join(f"line {i:04d} of input" for i in range(40))
    ctx.scenario("tool=" + batch_call([step("write", path="big.txt", content=body)])
                 + ",hold_final,final_text=done")
    s = ctx.spawn()
    s.submit("write it")
    tail = "\u25be 32 more lines"
    s.wait_text(tail)
    s.settle()
    row = s.screen.find_row(tail) + 1
    s.mouse("down", row, 9).mouse("up", row, 9).sync()
    s.wait_text("batch input")
    s.wait_text("line 0010 of input")
    assert '"steps"' not in s.text(), s.text()


def test_batch_image_results_survive_session_replay(ctx):
    from .test_image import png

    for i in range(8):
        (ctx.work / f"image-{i}.png").write_bytes(png(1, 1))
    s = run_batch(ctx, [step("read", path=f"image-{i}.png") for i in range(8)],
                  ARQAN_IMAGES="on")
    messages = ctx.mock.requests[-1]["messages"]
    images = [b for m in messages if isinstance(m.get("content"), list)
              for b in m["content"] if b["type"] == "image_url"]
    assert len(images) == 8, messages
    s.submit("/exit")
    s.wait_exit()

    resumed = ctx.spawn(ARQAN_IMAGES="on")
    resumed.submit("/resume")
    resumed.wait_text("pick a session")
    resumed.key("enter")
    resumed.wait_gone("pick a session")
    resumed.settle()
    ctx.scenario("text=ok,final_text=ok")
    resumed.submit("check the saved images")
    resumed.wait_text("ok")
    resumed.wait_turn_done()
    messages = ctx.mock.requests[-1]["messages"]
    images = [b for m in messages if isinstance(m.get("content"), list)
              for b in m["content"] if b["type"] == "image_url"]
    assert len(images) == 8, messages
