"""Abuse: hostile input, hostile providers, and the sizes nobody tests by hand.

Nothing here has a comfortable budget. What is asserted is survival: the
process is still running, still painting, and still accepts a message
afterwards. A stress case that ends in a live prompt has passed even when it
was slow, and a case that ends in a dead child has failed however fast it was.
"""

from __future__ import annotations

import json

from bench.case import needs, slow
from bench.fixtures import big_file, prose, wide_text
from tests.mockprovider import Scenario

# The line discipline owns a few of these: ^C, ^\ and ^Z signal the child and
# ^S/^Q stop its output, none of which is input the program ever sees.
TTY_OWNED = (3, 4, 13, 17, 19, 26, 28)
CONTROL = "".join(chr(c) for c in range(1, 32) if c not in TTY_OWNED)
ESCAPES = ["\x1b[200~", "\x1b[201~", "\x1b[<0;1;1M", "\x1b[1;5D", "\x1b[999;999R",
           "\x1b]52;c;AAAA\x07", "\x1bOA", "\x1b[Z", "\x1b[6~", "\x1b[3~"]


def still_alive(b, s, what: str):
    """The end of every stress case: it survived and still paints.

    Abuse leaves overlays open and turns running, so the check first puts the
    UI back where a user would: Esc out of whatever is on top, then type.
    """
    b.alive(s, what)
    for _ in range(3):
        s.key("esc")
        s.pump(0.05)
    s.wait_status("ready", timeout=60.0)
    s.key("ctrl-u")
    s.sync(timeout=30.0)
    s.type("still here")
    s.sync(timeout=30.0)
    b.check("still here" in s.composer_text(3),
            f"the composer stopped accepting text after {what}")
    s.key("ctrl-u").sync(timeout=30.0)


@needs("proc")
def bench_random_byte_storm(b):
    """Random bytes, control codes and half-finished escapes, unsynchronised."""
    s = b.spawn()
    rounds = b.scale(400, floor=50)
    payload = []
    for _ in range(rounds):
        pick = b.rng.random()
        if pick < 0.4:
            payload.append(prose(b.rng, 3))
        elif pick < 0.6:
            payload.append(b.rng.choice(ESCAPES))
        elif pick < 0.8:
            payload.append(b.rng.choice(CONTROL))
        else:
            payload.append("\x1b[" + str(b.rng.randint(0, 9999)))
    blob = "".join(payload)
    start = b.probe.read()
    s.send(blob)
    s.sync(timeout=60.0)
    d = b.probe.since(start)
    b.row("byte storm", wall_ms=d.wall_ms, cpu_ms=d.cpu_ms, units=len(blob),
          unit="byte", priv_kb=d.priv_kb, peak_kb=d.peak_kb, budget_ms=0.5)
    still_alive(b, s, "a byte storm")


@needs("proc")
def bench_invalid_utf8(b):
    """Truncated and illegal UTF-8 in the composer and from the provider."""
    s = b.spawn()
    junk = b"".join(bytes([c]) for c in (0xC3, 0xA9, 0xE2, 0x82, 0xF0, 0x9F,
                                         0x80, 0xFF, 0xFE, 0xC0, 0x80)) * b.scale(200, floor=20)
    with b.step("paste invalid bytes", units=len(junk), unit="byte",
                budget_ms=0.5):
        s.send(junk)
        s.sync(timeout=60.0)
    still_alive(b, s, "invalid UTF-8 input")

    b.ctx.scenario(Scenario(text="ok \ufffd\ufffd " + wide_text(b.rng, 200),
                            chunk=1))
    with b.step("stream odd glyphs", budget_ms=400.0):
        s.submit("say something odd")
        s.wait_turn_done(timeout=120.0)
    still_alive(b, s, "an odd reply")


@needs("proc")
def bench_resize_fuzz(b):
    """Every terminal size from one cell to enormous, in a random order."""
    s = b.spawn()
    sizes = [(1, 1), (2, 24), (20, 3), (200, 60), (400, 100), (80, 1),
             (37, 9), (132, 43), (1000, 24), (80, 24)]
    rounds = b.scale(4, floor=1)
    start = b.probe.read()
    changes = 0
    for _ in range(rounds):
        order = list(sizes)
        b.rng.shuffle(order)
        for cols, rows in order:
            s.resize(cols, rows)
            s.pump(0.02)
            changes += 1
    s.resize(80, 24)
    s.sync(timeout=60.0)
    d = b.probe.since(start)
    b.row("resize", wall_ms=d.wall_ms, cpu_ms=d.cpu_ms, units=changes,
          unit="resize", priv_kb=d.priv_kb, peak_kb=d.peak_kb, budget_ms=30.0)
    still_alive(b, s, "a resize storm")


@needs("proc")
def bench_overlay_churn(b):
    """Opening and dismissing every overlay, over and over."""
    s = b.spawn()
    rounds = b.scale(10, floor=3)
    with b.step("overlays", units=rounds * 4, unit="open", budget_ms=60.0):
        for _ in range(rounds):
            s.key("ctrl-r").sync()          # find box
            s.key("esc").sync()
            s.type("@").sync()              # path picker
            s.key("esc").sync()
            s.key("ctrl-u").sync()
            s.type("/").sync()              # command popup
            s.key("esc").sync()
            s.key("ctrl-u").sync()
            s.open_settings()               # settings screen
            s.key("esc")
            s.wait_gone("Verbose tool output")
            s.settle()
    still_alive(b, s, "overlay churn")


@needs("proc")
def bench_hostile_provider(b):
    """Errors, dropped connections and streams that stop mid-sentence."""
    s = b.spawn(ARQAN_RETRIES=1, ARQAN_RETRY_DELAY_MS=10)
    for label, spec in (
        ("http 500", "status=500,error=boom"),
        ("connection dropped", "fail_times=1,fail_mode=close,text=recovered"),
        ("stream aborted", "words=200,chunk=2,abort_after=20"),
        ("empty reply", "text="),
        ("no [DONE]", "words=40,chunk=4,keep_open=1"),
    ):
        b.ctx.scenario(spec)
        with b.step(label, budget_ms=400.0):
            s.submit(f"try {label.replace(' ', '-')}")
            s.wait_turn_done(timeout=120.0)
        b.alive(s, f"after {label}")
    still_alive(b, s, "a hostile provider")


@needs("proc")
def bench_hostile_tool_arguments(b):
    """Oversized, deep and malformed tool arguments, all refused in a turn."""
    big_file(b.ctx.work / "notes.txt", lines=200)
    s = b.spawn()
    deep = json.dumps({"path": "notes.txt", "extra": _nest(60)})
    for label, name, args in (
        ("path too long", "read", json.dumps({"path": "d/" * 3000 + "f.txt"})),
        ("command too long", "bash",
         json.dumps({"command": "echo " + "x" * 70000})),
        ("deeply nested arguments", "read", deep),
        ("arguments are not an object", "read", '"just a string"'),
        ("unknown tool", "nosuchtool", "{}"),
    ):
        b.ctx.scenario(f"tool={name}:{args},final_text=handled")
        with b.step(label, budget_ms=400.0):
            s.submit("/clear")
            s.settle()
            s.submit(f"try {label.replace(' ', '-')}")
            s.wait_turn_done(timeout=120.0)
        b.alive(s, f"after {label}")
    still_alive(b, s, "hostile tool arguments")


def _nest(depth: int):
    node = "leaf"
    for _ in range(depth):
        node = {"n": node}
    return node


@needs("proc")
def bench_rapid_interrupts(b):
    """Start a turn and stop it, as fast as the loop will take it."""
    s = b.spawn()
    b.ctx.scenario("words=3000,chunk=2,delay=0.002")
    rounds = b.scale(15, floor=4)

    def one():
        s.submit("go")
        s.wait_for(lambda t: s.status_kind() != "ready", "the turn to start",
                   timeout=30.0)
        s.key("esc")
        s.wait_status("ready", timeout=60.0)

    b.sample("submit and interrupt", one, repeat=rounds, unit="round",
             budget_ms=120.0)
    still_alive(b, s, "rapid interrupts")


@slow
@needs("proc")
def bench_transcript_past_the_scrollback(b):
    """Stream more than the scrollback holds: the trim path, repeatedly.

    Once the bounded transcript fills, every further reply throws away the
    oldest half and rebuilds every index over it. The cost of that must not
    grow with how many times it has already happened.
    """
    s = b.spawn()
    turns = b.scale(12, floor=3)
    b.ctx.scenario("words=6000,paragraphs=30,chunk=32")
    costs = []
    walls = []
    for i in range(turns):
        start = b.probe.read()
        s.submit(f"flood {i}")
        s.wait_turn_done(timeout=300.0)
        d = b.probe.since(start)
        costs.append(d.cpu_ms)
        walls.append(d.wall_ms)
    b.row("flood turn", units=turns, unit="turn", cpu_ms=sum(costs),
          wall_ms=sum(walls),
          priv_kb=b.probe.read().priv_kb,
          note=f"first {costs[0]:.0f}ms, last {costs[-1]:.0f}ms")
    ratio = costs[-1] / costs[0] if costs[0] > 0 else 0.0
    b.check(ratio < 3.0,
            f"a turn costs x{ratio:.1f} more once the scrollback has wrapped")
    b.keys("scroll the wrapped scrollback", s, ["pageup"] * b.scale(20, floor=5),
           budget_ms=15.0)
    still_alive(b, s, "a wrapped scrollback")


@slow
@needs("proc")
def bench_soak(b):
    """A long mixed session: turns, scrolling, searching, resizing, tools.

    Nothing here is measured for its own sake. The figure that matters is the
    growth in private pages from the first round to the last: a session that
    keeps working for an hour must not keep growing for an hour.
    """
    big_file(b.ctx.work / "notes.txt", lines=400)
    s = b.spawn()
    rounds = b.scale(10, floor=3)
    first = b.probe.read()
    marks = []
    for i in range(rounds):
        b.ctx.scenario("words=200,paragraphs=3,chunk=8")
        s.submit(f"round {i}")
        s.wait_turn_done(timeout=120.0)
        s.key("pageup", "pageup", "pagedown", "pagedown")
        s.key("ctrl-r")
        s.type("round")
        s.key("esc")
        s.resize(100 if i % 2 else 80, 24)
        s.type(prose(b.rng, 6))
        s.key("ctrl-u")
        s.sync(timeout=60.0)
        marks.append(b.probe.read().priv_kb)
    s.resize(80, 24)
    d = b.probe.since(first)
    b.row("soak", wall_ms=d.wall_ms, cpu_ms=d.cpu_ms, units=rounds, unit="round",
          priv_kb=d.priv_kb, peak_kb=d.peak_kb, growth_kb=d.priv_growth_kb,
          note=f"private dirty {marks[0]}K -> {marks[-1]}K")
    half = len(marks) // 2
    late = marks[-1] - marks[half]
    b.check(late < 8 * 1024,
            f"private dirty grew {late}K over the second half of the soak")
    still_alive(b, s, "a soak")
