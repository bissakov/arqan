"""Older images stop being sent once a request would carry more of them than
the image budget allows, so a session full of screenshots keeps working.

The budget counts images as they travel, base64-encoded. The cut moves
forward in steps of half the budget, so most requests repeat the one before
and the prompt cache holds; a withheld image leaves a note in its slot."""

from .test_image import attach, png, sessions_dir

BUDGET = 16 << 20
SHOT_BYTES = 2_500_000
WITHHELD = "no longer sent"


def big_shot(ctx, name="shot.png", size=SHOT_BYTES):
    data = png(4, 3)
    data += b"\x00" * (size - len(data))
    (ctx.work / name).write_bytes(data)
    return data


def request_images(body):
    """Every image a chat-completions body carries, in order."""
    return [part["image_url"]["url"].split(",", 1)[1]
            for msg in body["messages"]
            if isinstance(msg.get("content"), list)
            for part in msg["content"] if part.get("type") == "image_url"]


def anthropic_images(body):
    """Every image an Anthropic body carries, tool results included."""
    out = []
    for msg in body["messages"]:
        blocks = msg["content"] if isinstance(msg["content"], list) else []
        for block in blocks:
            inner = block.get("content")
            for part in [block] + (inner if isinstance(inner, list) else []):
                if part.get("type") == "image":
                    out.append(part["source"]["data"])
    return out


def encoded_bytes(images):
    return sum(len(data) for data in images)


def read_rounds(ctx, rounds, s=None, **env):
    ctx.scenario('tool=read:{"path":"shot.png"},tool_rounds=%d,'
                 'final_text=inspected' % rounds)
    s = s or ctx.spawn(**env)
    s.submit("compare the screenshots")
    s.wait_text("inspected")
    s.wait_turn_done()
    return s


def newest_call_sent(body):
    """True when the image of the newest tool call is in the request."""
    calls = [call["id"] for msg in body["messages"]
             for call in msg.get("tool_calls") or []]
    labels = [part["text"] for msg in body["messages"]
              if isinstance(msg.get("content"), list)
              for part in msg["content"] if part.get("type") == "text"]
    return any(label.startswith(f"From call {calls[-1]} ") for label in labels)


def test_image_budget_keeps_a_screenshot_session_going(ctx):
    """Regression: 16 screenshots in one turn once ran the request out of
    memory, and every later message failed the same way."""
    big_shot(ctx)
    read_rounds(ctx, 16)
    requests = ctx.mock.requests
    assert len(requests) == 17, len(requests)
    sizes = [encoded_bytes(request_images(body)) for body in requests]
    assert max(sizes) <= BUDGET, sizes
    last = requests[-1]
    assert len(request_images(last)) >= 2, sizes
    assert newest_call_sent(last)


def test_image_budget_moves_the_cut_in_steps(ctx):
    """Most rounds send the same images as the round before plus one."""
    big_shot(ctx)
    read_rounds(ctx, 16)
    counts = [len(request_images(body)) for body in ctx.mock.requests]
    grew = sum(1 for a, b in zip(counts, counts[1:]) if b == a + 1)
    assert grew * 2 > len(counts), counts


def test_image_budget_notes_a_withheld_image(ctx):
    big_shot(ctx)
    read_rounds(ctx, 8)
    last = ctx.mock.requests[-1]
    results = [m["content"] for m in last["messages"] if m["role"] == "tool"]
    assert WITHHELD in results[0], results[0]
    assert WITHHELD not in results[-1], results[-1]


def test_image_budget_caps_the_number_of_images(ctx):
    """Past 20 images a provider shrinks what it accepts, so none go over."""
    (ctx.work / "shot.png").write_bytes(png(4, 3))
    read_rounds(ctx, 30)
    counts = [len(request_images(body)) for body in ctx.mock.requests]
    assert max(counts) <= 20, counts
    assert counts[-1] >= 10, counts


def test_image_budget_anthropic(ctx):
    big_shot(ctx)
    read_rounds(ctx, 12, ARQAN_API="anthropic")
    sizes = [encoded_bytes(anthropic_images(body))
             for body in ctx.mock.requests]
    assert max(sizes) <= BUDGET, sizes
    assert sizes[-1] > 0, sizes
    results = ctx.mock.tool_results()
    assert any(WITHHELD in result for result in results), results


def test_image_budget_setting(ctx):
    big_shot(ctx)
    read_rounds(ctx, 8, ARQAN_IMAGE_BUDGET_MB="8")
    sizes = [encoded_bytes(request_images(body))
             for body in ctx.mock.requests]
    assert max(sizes) <= 8 << 20, sizes
    assert sizes[-1] > 0, sizes


def test_image_budget_rejects_a_value_out_of_range(ctx):
    big_shot(ctx)
    read_rounds(ctx, 8, ARQAN_IMAGE_BUDGET_MB="4")
    sizes = [encoded_bytes(request_images(body))
             for body in ctx.mock.requests]
    assert max(sizes) <= BUDGET, sizes
    assert max(sizes) > 8 << 20, sizes


def test_image_budget_resumes_a_session_over_the_budget(ctx):
    """A session saved with more images than the budget allows resumes and
    answers, which is what a session from before the budget looks like."""
    big_shot(ctx)
    s = read_rounds(ctx, 8, ARQAN_IMAGE_BUDGET_MB="64")
    assert encoded_bytes(request_images(ctx.mock.requests[-1])) > BUDGET
    s.submit("/exit")
    s.wait_exit()
    assert len(list(sessions_dir(ctx).rglob("*.png"))) == 1

    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("inspected")
    ctx.scenario("text=resumed,final_text=resumed")
    sent = len(ctx.mock.requests)
    s.submit("continue")
    s.wait_text("resumed")
    s.wait_turn_done()
    assert len(ctx.mock.requests) == sent + 1
    images = request_images(ctx.mock.requests[-1])
    assert 0 < encoded_bytes(images) <= BUDGET


def test_image_budget_bounds_the_compaction_request(ctx):
    big_shot(ctx)
    s = read_rounds(ctx, 12)
    ctx.scenario("text=ok")
    s.submit("two")
    s.wait_turn_done()
    ctx.scenario("text=##+Goal\\nCompare+the+screenshots")
    sent = len(ctx.mock.requests)
    s.submit("/compact")
    s.wait_text("compacted: a new session continues")
    summarize = ctx.mock.requests[sent]
    assert 0 < encoded_bytes(request_images(summarize)) <= BUDGET


def test_image_budget_refuses_an_attachment_over_it(ctx):
    """One message cannot carry more than the budget, so the image that
    would take it over is refused and the ones before it stay."""
    for name in ("a.png", "b.png", "c.png"):
        big_shot(ctx, name, 4_500_000)
    s = ctx.spawn(cols=160)
    attach(s, "a.png", 1)
    attach(s, "b.png", 2)
    s.type("/attach c.png").sync()
    s.key("enter")
    s.wait_text("image_budget_mb allows 16.0 MB")
    ctx.scenario("text=seen")
    s.submit("look")
    s.wait_text("seen")
    s.wait_turn_done()
    assert len(request_images(ctx.mock.requests[-1])) == 2
