"""Sessions and history: what it costs to persist a turn and to come back.

Resuming is the one operation whose input is unbounded and entirely the
user's: a conversation saved over weeks is read, parsed, replayed and
re-wrapped before the first frame of the resumed session. Everything here is
measured against a planted session of a known size.
"""

from __future__ import annotations

from bench.case import needs, slow
from bench.fixtures import (find_counts, plant_session, prose, resume_at,
                            seed_session, session_dir)

MARKER = "zqmark"


@needs("proc")
def bench_resume_a_large_session(b):
    """Read, parse and replay a session of a hundred thousand tokens."""
    seed_session(b.ctx)
    tokens = b.scale(100_000, floor=10_000)
    planted = plant_session(b.ctx, "big", tokens=tokens,
                            turns=b.scale(60, floor=10), tag="bigsess",
                            marker=MARKER, markers=b.scale(60, floor=10))
    b.row("session file", units=int(planted.megabytes * 1000) or 1, unit="KB",
          note=planted.describe())

    s = b.spawn()
    with b.step("open the picker", budget_ms=400.0):
        s.submit("/resume")
        s.wait_status("pick a session", timeout=120.0)
    with b.step("replay", budget_ms=2500.0):
        s.key("enter")
        s.wait_status("ready", timeout=300.0)
        s.settle(timeout=120.0)
    b.check("bigsess" in s.text(), "the resumed screen shows nothing of it")
    b.alive(s)

    b.keys("scroll back", s, ["pageup"] * b.scale(20, floor=5), budget_ms=8.0)


@needs("proc")
def bench_search_a_resumed_session(b):
    """Find over a resumed transcript: the whole conversation, per keystroke."""
    seed_session(b.ctx)
    markers = b.scale(60, floor=10)
    planted = plant_session(b.ctx, "searchme",
                            tokens=b.scale(60_000, floor=8_000),
                            turns=b.scale(60, floor=10), tag="searchme",
                            marker=MARKER, markers=markers)
    s = b.spawn()
    resume_at(s, 0)
    s.key("ctrl-r").sync()
    b.keys("query", s, list(MARKER), literal=True, budget_ms=12.0)
    index, count = find_counts(s)
    b.check(count == planted.markers,
            f"find counted {count} of {planted.markers} planted markers")
    b.keys("next match", s, ["enter"] * b.scale(20, floor=5), budget_ms=12.0)
    s.key("esc").sync()
    b.alive(s)


@needs("proc")
def bench_resume_one_unbroken_line(b):
    """A session whose replies have no line breaks: the wrap path on replay."""
    seed_session(b.ctx)
    plant_session(b.ctx, "oneline", tokens=b.scale(40_000, floor=6_000),
                  turns=b.scale(20, floor=5), tag="oneline", one_line=True)
    s = b.spawn()
    with b.step("replay", budget_ms=2500.0):
        resume_at(s, 0)
    b.check("oneline" in s.text(), "the resumed screen shows nothing of it")
    b.alive(s)


@needs("proc")
def bench_picker_with_many_sessions(b):
    """The picker lists every session under this directory."""
    seed_session(b.ctx)
    count = b.scale(200, floor=20)
    for i in range(count):
        plant_session(b.ctx, f"s{i:04d}", tokens=400, turns=2, tag=f"tag{i:04d}")
    b.row("sessions", units=count, unit="session",
          note=f"{len(list(session_dir(b.ctx).iterdir()))} files on disk")

    s = b.spawn()
    with b.step("open the picker", budget_ms=600.0):
        s.submit("/resume")
        s.wait_status("pick a session", timeout=120.0)
    b.keys("walk the list", s, ["down"] * b.scale(30, floor=10), budget_ms=10.0)
    b.keys("filter", s, list("tag00"), literal=True, budget_ms=10.0)
    s.key("esc").sync()
    b.alive(s)


@needs("proc")
def bench_persisting_turns(b):
    """Every turn is appended to the session file: what that append costs."""
    s = b.spawn()
    b.ctx.scenario("words=120,paragraphs=2,chunk=8")
    turns = b.scale(15, floor=4)
    start = b.probe.read()

    def one():
        s.submit("write something")
        s.wait_turn_done(timeout=60.0)

    b.sample("turn", one, repeat=turns, unit="turn", budget_ms=90.0)
    d = b.probe.since(start)
    b.row("session io", units=turns, unit="turn", write_kb=d.write_kb,
          read_kb=d.read_kb, priv_kb=d.priv_kb, peak_kb=d.peak_kb,
          note=f"wrote {d.write_kb}K over {turns} turns")
    b.alive(s)


@needs("proc")
def bench_append_to_a_large_session(b):
    """Saving new work must not rewrite the conversation already on disk."""
    seed_session(b.ctx)
    planted = plant_session(b.ctx, "append", tokens=b.scale(100_000, floor=20_000),
                            turns=b.scale(60, floor=12), tag="appendbase")
    initial_bytes = planted.path.stat().st_size
    initial_lines = len(planted.path.read_bytes().splitlines())
    b.row("session file", units=int(planted.megabytes * 1000) or 1, unit="KB",
          note=planted.describe())

    s = b.spawn()
    resume_at(s, 0)
    saves = b.scale(12, floor=4)
    start = b.probe.read()

    def one():
        s.submit("!:")
        s.wait_status("ready", timeout=30.0)

    b.sample("append", one, repeat=saves, unit="save", budget_ms=30.0)
    d = b.probe.since(start)
    final_bytes = planted.path.stat().st_size
    final_lines = len(planted.path.read_bytes().splitlines())
    growth = final_bytes - initial_bytes
    b.row("session io", units=saves, unit="save", write_kb=d.write_kb,
          read_kb=d.read_kb, priv_kb=d.priv_kb, peak_kb=d.peak_kb,
          note=f"file grew {growth}B; process wrote {d.write_kb}K")
    b.check(final_lines - initial_lines == saves,
            f"{saves} saves added {final_lines - initial_lines} records")
    b.check(d.write_kb * 1024 < initial_bytes,
            f"incremental saves wrote {d.write_kb}K around a "
            f"{initial_bytes // 1024}K session")
    b.alive(s)


@needs("proc")
def bench_history_recall(b):
    """A full prompt history file, walked from the newest entry to the oldest."""
    hist = b.ctx.home / ".local" / "state" / "arqan" / "history"
    hist.parent.mkdir(parents=True, exist_ok=True)
    entries = b.scale(500, floor=50)
    hist.write_text("".join(f"{i:04d} {prose(b.rng, 12)}\n" for i in range(entries)))
    b.row("history", units=entries, unit="entry")

    s = b.spawn()
    b.keys("recall", s, ["up"] * b.scale(60, floor=10), budget_ms=4.0)
    b.keys("forward", s, ["down"] * b.scale(60, floor=10), budget_ms=4.0)
    b.alive(s)


@slow
@needs("proc")
def bench_resume_the_biggest_session(b):
    """A session past the scrollback, read in one go.

    A rendered transcript larger than the bounded scrollback keeps its newest
    half, and the resumed view must still open on the newest row: the check
    below is what says the reader is looking at the end of the conversation
    rather than at the top of what survived the trim.
    """
    seed_session(b.ctx)
    planted = plant_session(b.ctx, "huge", tokens=b.scale(400_000, floor=50_000),
                            turns=b.scale(200, floor=20), tag="hugesess")
    b.row("session file", units=int(planted.megabytes * 1000) or 1, unit="KB",
          note=planted.describe())
    s = b.spawn()
    with b.step("replay", budget_ms=12000.0):
        resume_at(s, 0, timeout=600.0)
    b.check("hugesess" in s.text(),
            "the resumed view opened somewhere other than the newest row")
    b.keys("scroll back", s, ["pageup"] * b.scale(20, floor=5), budget_ms=15.0)
    b.check("hugesess" in s.text(), "the transcript is not there at all")
    b.alive(s)
