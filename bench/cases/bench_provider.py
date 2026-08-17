"""Provider paths: both wire formats, both stream settings, and the pickers.

The request is rebuilt from the whole conversation on every turn, so a turn
late in a long session pays for everything said before it. These cases keep
the reply fixed and vary what has to be sent, which is the only way to see
that cost on its own.
"""

from __future__ import annotations

from bench.case import needs, slow
from bench.fixtures import png_image, prose
from tests.mockprovider import Scenario


def turns(b, s, count: int, words: int = 120):
    b.ctx.scenario(f"words={words},paragraphs=2,chunk=8")
    for i in range(count):
        s.submit(f"turn {i} " + prose(b.rng, 20))
        s.wait_turn_done(timeout=120.0)


@needs("proc")
def bench_openai_turn(b):
    """The OpenAI chat-completions path, on a conversation of known size."""
    s = b.spawn()
    turns(b, s, b.scale(10, floor=3))
    b.ctx.scenario("words=200,paragraphs=2,chunk=8")
    b.sample("turn", lambda: (s.submit("again"), s.wait_turn_done(timeout=120.0)),
             repeat=b.scale(5, floor=2), unit="turn", budget_ms=90.0)
    sent = len(str(b.ctx.mock.requests[-1]))
    b.row("request body", units=sent, unit="byte",
          note=f"{len(b.ctx.mock.requests[-1].get('messages', []))} messages sent")
    b.alive(s)


@needs("proc")
def bench_anthropic_turn(b):
    """The Anthropic messages path, on the same shape of conversation."""
    s = b.spawn(ARQAN_API="anthropic")
    turns(b, s, b.scale(10, floor=3))
    b.ctx.scenario("words=200,paragraphs=2,chunk=8")
    b.sample("turn", lambda: (s.submit("again"), s.wait_turn_done(timeout=120.0)),
             repeat=b.scale(5, floor=2), unit="turn", budget_ms=90.0)
    sent = len(str(b.ctx.mock.requests[-1]))
    b.row("request body", units=sent, unit="byte",
          note=f"{len(b.ctx.mock.requests[-1].get('messages', []))} messages sent")
    b.alive(s)


@needs("proc")
def bench_non_streaming_turn(b):
    """A whole reply delivered at once: one paint instead of hundreds."""
    s = b.spawn(ARQAN_STREAM="false")
    words = b.scale(2000, floor=200)
    with b.step("whole reply", units=words, unit="word", budget_ms=0.5):
        b.ctx.scenario(f"words={words},paragraphs=8")
        s.submit("write it all at once")
        s.wait_turn_done(timeout=180.0)
    b.alive(s)


@needs("proc")
def bench_request_grows_with_the_conversation(b):
    """Turn cost against conversation length: the curve, not one point.

    The whole conversation is serialized for every turn, so cost per turn is
    expected to climb. What must not happen is the climb accelerating.
    """
    s = b.spawn()
    b.ctx.scenario("words=100,paragraphs=2,chunk=8")
    marks = []
    blocks = b.scale(4, floor=2)
    per_block = b.scale(6, floor=3)
    for block in range(blocks):
        start = b.probe.read()
        for i in range(per_block):
            s.submit(f"block {block} turn {i} " + prose(b.rng, 40))
            s.wait_turn_done(timeout=120.0)
        d = b.probe.since(start)
        marks.append(d.cpu_ms / per_block)
        b.row(f"turns {block * per_block + 1}-{(block + 1) * per_block}",
              wall_ms=d.wall_ms, cpu_ms=d.cpu_ms, units=per_block, unit="turn",
              priv_kb=d.priv_kb, peak_kb=d.peak_kb)
    ratio = marks[-1] / marks[0] if marks[0] > 0 else 0.0
    b.note(f"cost per turn grew x{ratio:.2f} over the session")
    b.check(ratio < 4.0, f"cost per turn grew x{ratio:.1f} over the session")
    b.alive(s)


@needs("proc")
def bench_retry_and_recover(b):
    """A turn that fails and is retried costs the failures too."""
    s = b.spawn(ARQAN_RETRIES=3, ARQAN_RETRY_DELAY_MS=10)
    b.ctx.scenario("fail_times=3,fail_status=503,text=recovered")
    with b.step("three failures then a reply", budget_ms=400.0):
        s.submit("try me")
        s.wait_turn_done(timeout=120.0)
    b.check("recovered" in s.text(), "the turn never recovered")
    b.alive(s)


@needs("proc")
def bench_model_picker(b):
    """A provider serving a thousand models: listing, filtering, choosing."""
    count = b.scale(1000, floor=100)
    b.ctx.scenario(f"model_count={count}")
    s = b.spawn()
    with b.step("open the picker", budget_ms=400.0):
        s.submit("/model")
        s.wait_status("pick a model", timeout=60.0)
    b.row("models", units=count, unit="model")
    b.keys("filter", s, list("0"), literal=True, budget_ms=20.0)
    b.keys("walk", s, ["down"] * b.scale(20, floor=5), budget_ms=10.0)
    s.key("esc").sync()
    b.alive(s)


@slow
@needs("proc")
def bench_reasoning_heavy_session(b):
    """Every turn carries reasoning: two panels per turn, kept in the session."""
    s = b.spawn()
    rounds = b.scale(10, floor=3)
    thoughts = "\n\n".join(prose(b.rng, 60) for _ in range(4))
    b.ctx.scenario(Scenario(reasoning=thoughts, words=150, chunk=6))

    def one():
        s.submit("think about it")
        s.wait_turn_done(timeout=120.0)

    b.sample("turn", one, repeat=rounds, unit="turn", budget_ms=150.0)
    b.alive(s)


@needs("proc")
def bench_turn_carrying_an_image(b):
    """A conversation with an image in it.

    The request is rebuilt from the whole conversation every turn, so the
    image is encoded again for each one: what this measures is the cost a
    later turn pays for a picture sent earlier.
    """
    image = png_image(b.rng, b.scale(1200, floor=200), b.scale(800, floor=150))
    (b.ctx.work / "shot.png").write_bytes(image)
    b.note(f"image {len(image) // 1024} KB")

    s = b.spawn()
    s.type("/attach shot.png")
    s.key("enter")
    s.wait_text("attached [Image #1]")
    b.ctx.scenario("words=200,paragraphs=2,chunk=8")
    with b.step("turn that attaches", budget_ms=250.0):
        s.submit("what is [Image #1]")
        s.wait_turn_done(timeout=120.0)
    b.sample("turn that replays it",
             lambda: (s.submit("again"), s.wait_turn_done(timeout=120.0)),
             repeat=b.scale(5, floor=2), unit="turn", budget_ms=250.0)
    sent = len(str(b.ctx.mock.requests[-1]))
    b.note(f"request body {sent // 1024} KB")
    b.check("image_url" in str(b.ctx.mock.requests[-1]),
            "the replayed turn carried no image")
    b.alive(s)
