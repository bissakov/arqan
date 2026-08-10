"""Rebuilding the transcript from a conversation paints once, not per line.

A replay lays the transcript down a line at a time, and a finished line asks
for a frame. Only the last of those frames is ever seen, so the rebuild holds
them. The measure here is how much the terminal was made to consume, which
says how many frames were painted without depending on how fast the machine
ran them.
"""

import json


def sessions_dir(ctx):
    root = ctx.home / ".local" / "share" / "yoke" / "sessions"
    dirs = [p for p in root.iterdir() if p.is_dir()] if root.exists() else []
    assert len(dirs) == 1, sorted(p.name for p in dirs)
    return dirs[0]


def seed_session_dir(ctx):
    """One real turn, so the per-cwd session directory exists and is named."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.submit("seed")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()
    return sessions_dir(ctx)


def plant(ctx, name: str, tag: str, turns: int, lines: int, words: int = 12):
    """A session file of `turns` replies, each `lines` short paragraphs."""
    d = seed_session_dir(ctx)
    para = " ".join(f"{tag}{i:03d}" for i in range(words))
    path = d / f"{name}.jsonl"
    with path.open("w") as f:
        for i in range(turns):
            f.write(json.dumps({"role": "user",
                                "content": f"turn {i} {tag}"}) + "\n")
            body = "\n\n".join(f"{para} L{j:04d}" for j in range(lines))
            f.write(json.dumps({"role": "assistant", "content": body}) + "\n")
    return path


def resume_first(s, timeout: float = 60.0):
    """Open /resume and take the newest entry, which is the planted file."""
    s.submit("/resume")
    s.wait_text("pick a session", timeout=timeout)
    s.key("down")          # row 0 is the seed turn, row 1 the planted session
    s.settle()
    s.key("enter")
    s.wait_gone("pick a session", timeout=timeout)
    s.settle(timeout=timeout)


def test_replay_paints_once_not_per_line(ctx):
    """A replay of ~2000 lines costs a handful of frames, not two thousand."""
    plant(ctx, "20240101-000001", "alpha", turns=20, lines=100)
    s = ctx.spawn()

    before = len(s.raw)
    resume_first(s)
    written = len(s.raw) - before

    assert "alpha" in s.text(), "the session did not load"
    # A frame on this screen is a few KB. Painting one per line put megabytes
    # through the terminal; a held rebuild puts a few frames' worth.
    assert written < 400_000, f"replay wrote {written} bytes to the terminal"


def test_replay_restores_the_conversation(ctx):
    """Holding the frames does not change what ends up on screen."""
    plant(ctx, "20240101-000001", "bravo", turns=3, lines=2)
    s = ctx.spawn()
    resume_first(s)

    out = s.text()
    assert "bravo" in out
    assert "turn 2" in out, out[-500:]
    assert "L0001" in out, out[-500:]
    assert s.status_field(0).endswith("ready") or "ready" in s.status_line()
