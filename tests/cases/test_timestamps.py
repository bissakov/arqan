"""Turn timestamps stay outside the user message and survive replay."""

import json
import re
import time

from tests.cases.test_resume import plant_session, sessions_dir


def timestamp_rows(s):
    return [i for i, row in enumerate(s.screen.lines())
            if re.fullmatch(r"Sent at \d{2}:\d{2}", row.strip())]


def test_timestamp_is_dim_and_outside_the_user_block(ctx):
    ctx.scenario('tool=bash:{"command":"ls","description":"list files"},'
                 'final_text=answered')
    s = ctx.spawn()
    s.submit("my question")
    s.wait_text("answered")
    s.wait_turn_done()

    stamps = timestamp_rows(s)
    assert len(stamps) == 1, s.text()
    stamp = stamps[0]
    user = s.screen.find_row("my question")
    assert stamp == user - 2, s.text()
    col = s.screen.row_text(stamp).index("Sent at")
    attr = s.screen.attr_at(stamp, col)
    assert attr.fg == 245, attr
    assert attr.bg is None, attr
    assert s.screen.attr_at(user - 1, 2).bg == 238
    assert s.screen.attr_at(user, 2).bg == 238


def test_timestamp_is_saved_but_not_sent_to_the_provider(ctx):
    ctx.scenario("text=answered")
    s = ctx.spawn(TZ="UTC0")
    before = int(time.time())
    s.submit("my question")
    s.wait_text("answered")
    s.wait_turn_done()
    after = int(time.time())

    path, = sessions_dir(ctx).glob("*.jsonl")
    records = [json.loads(line) for line in path.read_text().splitlines()]
    user, = [r for r in records if r.get("role") == "user"]
    assert isinstance(user["sent_at"], int), user
    assert before <= user["sent_at"] <= after, user
    assert user["content"] == "my question", user
    assert all("sent_at" not in r for r in records
               if r.get("role") != "user"), records
    wire, = [m for m in ctx.mock.requests[-1]["messages"]
             if m["role"] == "user"]
    assert wire == {"role": "user", "content": "my question"}, wire
    shown = s.screen.row_text(timestamp_rows(s)[0]).strip()
    assert shown == time.strftime("Sent at %H:%M", time.gmtime(user["sent_at"]))

    s.settings_toggle("Verbose tool output")
    assert s.screen.row_text(timestamp_rows(s)[0]).strip() == shown
    s.submit("/exit")
    s.wait_exit()
    resumed = ctx.spawn(TZ="UTC0")
    resumed.submit("/resume")
    resumed.wait_status("pick a session")
    resumed.key("enter")
    resumed.wait_text("answered")
    assert resumed.screen.row_text(timestamp_rows(resumed)[0]).strip() == shown

    resumed.submit("/fork")
    resumed.sync()
    paths = sorted(sessions_dir(ctx).glob("*.jsonl"))
    assert len(paths) == 2, paths
    for saved in paths:
        records = [json.loads(line) for line in saved.read_text().splitlines()]
        kept, = [r for r in records if r.get("role") == "user"]
        assert kept["sent_at"] == user["sent_at"], kept


def test_resume_uses_the_saved_time_in_local_time(ctx):
    plant_session(ctx, json.dumps({"role": "user", "content": "old question",
                                   "sent_at": 1704112440}) + "\n")
    s = ctx.spawn(TZ="UTC-2")
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("old question")
    s.wait_text("Sent at 14:34")
    assert len(timestamp_rows(s)) == 1, s.text()


def test_resume_without_a_timestamp_does_not_invent_one(ctx):
    plant_session(ctx, '{"role":"user","content":"old question"}\n')
    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("old question")
    assert not timestamp_rows(s), s.text()


def test_resume_ignores_invalid_timestamps(ctx):
    values = [-1, 0, 1.5, "1704112440", 1e100, None]
    plant_session(ctx, "".join(json.dumps({
        "role": "user", "content": f"invalid stamp {i}", "sent_at": value,
    }) + "\n" for i, value in enumerate(values)))
    s = ctx.spawn(rows=40)
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("invalid stamp 5")
    assert not timestamp_rows(s), s.text()


def test_two_turns_have_two_timestamps(ctx):
    ctx.scenario("text=answered")
    s = ctx.spawn(rows=40)
    for prompt in ("first question", "second question"):
        s.submit(prompt)
        s.wait_turn_done()
    assert len(timestamp_rows(s)) == 2, s.text()
