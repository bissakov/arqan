"""Telemetry: an anonymized record of a session, off until it is asked for."""

import json


def log_dir(ctx, state=None):
    return (state or ctx.home / ".local" / "state") / "arqan" / "telemetry"


def log_files(ctx, state=None):
    """Every record under the telemetry dir, oldest write first."""
    d = log_dir(ctx, state)
    if not d.is_dir():
        return []
    return sorted(d.rglob("*.jsonl"), key=lambda p: (p.stat().st_mtime, p.name))


def body(ctx):
    return "".join(p.read_text() for p in log_files(ctx))


def events(ctx, which=None):
    """Every event recorded, or one file's when `which` indexes log_files."""
    files = log_files(ctx)
    assert files, sorted(p.name for p in ctx.home.rglob("*"))
    if which is not None:
        files = [files[which]]
    return [json.loads(line) for f in files
            for line in f.read_text().splitlines() if line]


def kinds(ctx, which=None):
    return [e["ev"] for e in events(ctx, which)]


def test_recording_is_off_until_it_is_asked_for(ctx):
    """A session records nothing of its own accord."""
    ctx.scenario("text=hello+there")
    s = ctx.spawn()
    s.submit("say hi")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()
    assert not log_files(ctx)


def test_telemetry_toggle_records_the_turn(ctx):
    """With it on, a turn leaves the events a report is read from."""
    ctx.scenario(
        "text=hello+there,usage=200/12,cache_read=128,cache_creation=64"
    )
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("say hi")
    s.wait_turn_done()

    seen = kinds(ctx)
    assert seen[0] == "session", seen
    for ev in ("turn_start", "request", "turn_end"):
        assert ev in seen, seen

    request = [e for e in events(ctx) if e["ev"] == "request"][-1]
    assert request["prompt_tokens"] == 200
    assert request["completion_tokens"] == 12
    assert request["cache_read_tokens"] == 128
    assert request["cache_creation_tokens"] == 64
    assert request["reply_bytes"] == len("hello there")
    turn = [e for e in events(ctx) if e["ev"] == "turn_end"][-1]
    assert turn["ok"] is True and turn["rounds"] == 1, turn
    assert turn["persist_used"] > 0


def test_the_record_keeps_no_conversation(ctx):
    """A message is a size and a line count, never its text."""
    ctx.write_file("secret.txt", "the passphrase is swordfish")
    ctx.scenario(
        'tool=read:{"path":"secret.txt"},final_text=I+read+the+secret+file'
    )
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("read secret.txt, my private question")
    s.wait_turn_done()

    text = body(ctx)
    for leaked in ("private question", "swordfish", "secret.txt",
                   "I read the secret file", str(ctx.work)):
        assert leaked not in text, text

    start = [e for e in events(ctx) if e["ev"] == "turn_start"][-1]
    assert start["prompt_bytes"] == len("read secret.txt, my private question")
    assert start["prompt_lines"] == 1

    tool = [e for e in events(ctx) if e["ev"] == "tool"][-1]
    assert tool["name"] == "read"
    assert tool["args"] == "path", tool          # the key, never the path
    assert tool["ok"] is True
    assert tool["result_bytes"] > 0


def test_telemetry_off_stops_the_recording(ctx):
    """Toggling it back leaves the file where it was."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("first")
    s.wait_turn_done()

    s.settings_toggle("Telemetry")
    s.submit("second")
    s.wait_turn_done()

    seen = events(ctx)
    # The command that stopped it is the last thing recorded, so the file
    # says why it ends rather than simply stopping.
    assert seen[-1] == {**seen[-1], "ev": "command", "name": "/settings"}
    assert kinds(ctx).count("turn_start") == 1, kinds(ctx)


def test_the_setting_survives_the_session(ctx):
    """It is remembered, so a run that cannot type the command records too."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("/exit")
    s.wait_exit()

    again = ctx.spawn()
    again.submit("later run")
    again.wait_turn_done()
    # Each run owns a file, and each holds the session it recorded.
    assert len(log_files(ctx)) == 2, log_files(ctx)
    for i in (0, 1):
        assert kinds(ctx, i).count("session") == 1, events(ctx, i)


def test_a_second_session_does_not_touch_the_first(ctx):
    """The record of a run is the run, so a later one appends nowhere near."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("first run")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()
    first = log_files(ctx)[0]
    kept = first.read_text()

    again = ctx.spawn()
    again.submit("second run")
    again.wait_turn_done()
    again.submit("/exit")
    again.wait_exit()

    assert first.read_text() == kept, first
    files = log_files(ctx)
    assert len(files) == 2 and first in files, files
    second = [p for p in files if p != first][0]
    lines = [json.loads(l) for l in second.read_text().splitlines() if l]
    assert lines[0]["seq"] == 0 and lines[0]["ev"] == "session", lines[0]


def test_the_record_lands_in_the_state_dir(ctx):
    """XDG_STATE_HOME moves the file, setting and record alike."""
    state = ctx.tmp / "state"
    s = ctx.spawn(XDG_STATE_HOME=str(state))
    s.settings_toggle("Telemetry")
    s.submit("/exit")
    s.wait_exit()

    settings = ctx.settings(state / "arqan" / "state.toml")
    assert settings[""]["telemetry"] == "true", settings
    assert len(log_files(ctx, state)) == 1, log_dir(ctx, state)
    assert not log_files(ctx), "the default must stay unused"


def test_commands_and_mode_switches_are_recorded(ctx):
    """The hidden half of a session: what was toggled and when."""
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("/mode")
    s.wait_text("plan mode")
    s.settings_toggle("Verbose tool output")
    # The events of a run that started no conversation are written when it
    # ends, so the file is read after it.
    s.submit("/exit")
    s.wait_exit()

    seen = events(ctx)
    names = [e["name"] for e in seen if e["ev"] == "command"]
    assert names == ["/mode", "/settings", "/exit"], names
    mode = [e for e in seen if e["ev"] == "mode"][-1]
    assert mode["from"] == "build" and mode["to"] == "plan", mode


def test_an_unknown_command_is_not_named(ctx):
    """A rejected command records only the anonymous unknown marker."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("/my-private-note")
    s.wait_text("unknown command")
    s.submit("bind the telemetry file")
    s.wait_turn_done()

    text = body(ctx)
    assert "my-private-note" not in text, text
    names = [e["name"] for e in events(ctx) if e["ev"] == "command"]
    assert names == ["(unknown)"], names


def test_the_transfer_is_recorded_with_its_timings(ctx):
    """A turn's request is a network event: curl's phases and counters."""
    ctx.scenario("text=hello+there,chunk=1")
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("say hi")
    s.wait_turn_done()

    http = [e for e in events(ctx) if e["ev"] == "http"][-1]
    assert http["method"] == "POST"
    assert http["path"] == "/chat/completions"
    assert http["status"] == 200 and http["curl"] == 0, http
    assert http["loopback"] is True and http["tls"] is False, http
    assert http["up_bytes"] > 0 and http["down_bytes"] > 0, http
    assert http["sse_lines"] > 0 and http["polls"] > 0, http
    assert http["ip"] == "v4", http
    for phase in ("dns_ms", "connect_ms", "ttfb_ms", "total_ms", "stall_ms"):
        assert phase in http, http


def test_the_endpoint_is_a_hash_not_a_url(ctx):
    """A host names its owner, so it is recorded the way the cwd is."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("say hi")
    s.wait_turn_done()

    text = body(ctx)
    assert "127.0.0.1" not in text, text
    assert ctx.mock.base_url not in text, text
    http = [e for e in events(ctx) if e["ev"] == "http"][-1]
    assert len(http["host"]) == 16 and int(http["host"], 16) >= 0, http


def test_a_refused_request_records_its_status(ctx):
    """An HTTP failure is the status it came back with."""
    ctx.scenario("status=500")
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("this will fail")
    s.wait_turn_done()

    http = [e for e in events(ctx) if e["ev"] == "http"][-1]
    assert http["status"] == 500 and http["curl"] == 0, http


def test_the_model_listing_is_a_transfer_too(ctx):
    """/model reaches the network, so the record says so."""
    ctx.scenario("models=one|two")
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("/model")
    s.wait_text("pick a model")
    s.key("esc").sync()
    s.submit("/exit")
    s.wait_exit()

    gets = [e for e in events(ctx) if e["ev"] == "http" and e["method"] == "GET"]
    assert gets and gets[-1]["path"] == "/models", events(ctx)
    assert gets[-1]["status"] == 200, gets[-1]


def test_diagnostics_land_beside_the_events(ctx):
    """A provider failure is recorded as the status it was."""
    ctx.scenario("status=500")
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("this will fail")
    s.wait_turn_done()

    errors = [e for e in events(ctx) if e["ev"] == "error"]
    assert errors and errors[-1]["detail"] == "HTTP 500", events(ctx)


def test_a_file_where_the_directory_goes_leaves_the_session_alone(ctx):
    """The record root may be taken by a file; a session must not care."""
    d = log_dir(ctx)
    d.parent.mkdir(parents=True, exist_ok=True)
    d.write_text("not a directory\n")
    ctx.write_config("telemetry = true\n")
    ctx.scenario("text=ok")

    s = ctx.spawn()
    s.submit("say hi")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    assert d.read_text() == "not a directory\n", d.read_text()


def test_the_record_is_the_conversation_it_belongs_to(ctx):
    """/clear starts a session, so it starts a record: one file each."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("first conversation")
    s.wait_turn_done()
    first = log_files(ctx)
    assert len(first) == 1, first

    s.submit("/clear")
    s.wait_for(lambda t: "first conversation" not in t.text(),
               "transcript to clear")
    s.submit("second conversation")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    files = log_files(ctx)
    assert len(files) == 2, files
    # Each stands on its own: a header, then the turn it recorded.
    for f in files:
        seen = [json.loads(l)["ev"] for l in f.read_text().splitlines() if l]
        assert seen[0] == "session", (f, seen)
        assert seen.count("turn_start") == 1, (f, seen)


def test_the_record_is_named_after_the_session_file(ctx):
    """The record of a conversation is found from the conversation."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("hello")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    sessions = sorted(
        (ctx.home / ".local" / "share" / "arqan" / "sessions").rglob("*.jsonl")
    )
    assert len(sessions) == 1, sessions
    record = log_files(ctx)[0]
    assert record.name == sessions[0].name, (record, sessions)
    assert record.parent.name == sessions[0].parent.name, (record, sessions)


def test_resuming_continues_the_record_it_reopened(ctx):
    """A resumed conversation appends to the record it already had."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("the first turn")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()
    record = log_files(ctx)[0]
    before = len(record.read_text().splitlines())

    again = ctx.spawn()
    again.submit("/resume")
    again.wait_status("pick a session")
    again.key("enter")
    again.wait_text("the first turn")
    again.submit("the second turn")
    again.wait_turn_done()
    again.submit("/exit")
    again.wait_exit()

    lines = [json.loads(l) for l in record.read_text().splitlines() if l]
    assert len(lines) > before               # the same record, continued
    # The resumed run opens with a session event of its own, since a reader
    # of the file has to see where one run ended and the next began.
    assert [e["ev"] for e in lines].count("session") == 2, lines
    assert [e["ev"] for e in lines].count("turn_start") == 2, lines
    # Opening arqan and resuming leaves that record and nothing beside it.
    assert log_files(ctx) == [record], log_files(ctx)


def test_opening_and_resuming_leaves_one_record(ctx):
    """The /resume that picked a conversation belongs to the one it picked."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("the only turn")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()
    record = log_files(ctx)[0]

    again = ctx.spawn()
    again.submit("/resume")
    again.wait_status("pick a session")
    again.key("enter")
    again.wait_text("the only turn")
    again.submit("/exit")
    again.wait_exit()

    assert log_files(ctx) == [record], log_files(ctx)
    names = [e["name"] for e in events(ctx) if e["ev"] == "command"]
    assert names.count("/resume") == 1, events(ctx)
