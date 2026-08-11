"""Rendering: markdown, syntax highlighting and the toggles that redraw all of it.

The transcript is rebuilt from the conversation whenever anything about how
it is drawn changes, so a display toggle costs a full re-render of everything
said so far. Code blocks cost more than prose, because they leave the process
for the highlighter and come back as spans.
"""

from __future__ import annotations

from bench.case import needs, slow
from bench.fixtures import markdown_doc, prose, wide_text
from tests.mockprovider import Scenario

LANGS = ("c", "python", "rust", "go", "javascript", "typescript", "bash",
         "json", "toml", "yaml")


def code_reply(b, blocks: int, lines: int = 30) -> str:
    """A reply that is mostly fenced code, spread over every language."""
    out = []
    for i in range(blocks):
        lang = LANGS[i % len(LANGS)]
        body = "\n".join(
            f"{'    ' * (n % 3)}value_{n} = call_{n}(arg_{n}, {n});"
            for n in range(lines)
        )
        out.append(f"### block {i} ({lang})\n\n```{lang}\n{body}\n```")
    return "\n\n".join(out)


@needs("proc")
def bench_syntax_highlighting(b):
    """Fenced code in ten languages: the highlighter, over and over."""
    blocks = b.scale(20, floor=4)
    doc = code_reply(b, blocks)
    s = b.spawn()
    b.ctx.scenario(Scenario(text=doc, chunk=10))
    with b.step("stream code", units=blocks, unit="block", budget_ms=120.0):
        s.submit("show me code")
        s.wait_turn_done(timeout=180.0)
    b.keys("scroll it", s, ["pageup"] * b.scale(20, floor=5), budget_ms=8.0)
    b.alive(s)


@needs("proc")
def bench_one_enormous_code_block(b):
    """A single fence of thousands of lines: one highlight request, not many."""
    lines = b.scale(3000, floor=300)
    body = "\n".join(f"static int f{n}(void) {{ return {n}; }}" for n in range(lines))
    s = b.spawn()
    b.ctx.scenario(Scenario(text=f"```c\n{body}\n```", chunk=16))
    with b.step("stream one block", units=lines, unit="line", budget_ms=1.0):
        s.submit("dump the file")
        s.wait_turn_done(timeout=300.0)
    b.keys("scroll it", s, ["pageup"] * b.scale(20, floor=5), budget_ms=10.0)
    b.alive(s)


@needs("proc")
def bench_display_toggles_rerender(b):
    """Each display toggle rebuilds the whole transcript from the source."""
    s = b.spawn()
    for i in range(b.scale(4, floor=1)):
        b.ctx.scenario(Scenario(text=markdown_doc(b.rng, b.scale(8, floor=2)),
                                chunk=12))
        s.submit(f"document {i}")
        s.wait_turn_done(timeout=180.0)

    for label in ("Display raw", "Display raw"):
        with b.step(f"toggle {label.lower()}", budget_ms=250.0):
            s.settings_toggle(label)
            s.settle()
    b.alive(s)


@needs("proc")
def bench_verbose_tool_output_toggle(b):
    """Turning verbose tool output on redraws every tool result in the session."""
    (b.ctx.work / "notes.txt").write_text(
        "\n".join(prose(b.rng, 12) for _ in range(b.scale(400, floor=50))) + "\n")
    b.ctx.scenario('tool=read:{"path":"notes.txt"},tool_rounds=4,final_text=read')
    s = b.spawn()
    s.submit("read it a few times")
    s.wait_turn_done(timeout=180.0)
    with b.step("toggle verbose", budget_ms=250.0):
        s.settings_toggle("Verbose tool output")
        s.settle()
    with b.step("toggle back", budget_ms=250.0):
        s.settings_toggle("Verbose tool output")
        s.settle()
    b.alive(s)


@needs("proc")
def bench_wide_glyph_layout(b):
    """A transcript of CJK, emoji and combining marks, wrapped and re-wrapped."""
    s = b.spawn()
    b.ctx.scenario(Scenario(text=wide_text(b.rng, b.scale(1500, floor=200)),
                            chunk=8))
    s.submit("write it wide")
    s.wait_turn_done(timeout=180.0)
    widths = [60, 100, 72, 132, 48, 80]
    start = b.probe.read()
    for w in widths:
        s.resize(w, 24)
        s.sync()
    d = b.probe.since(start)
    b.row("re-wrap", wall_ms=d.wall_ms, cpu_ms=d.cpu_ms, units=len(widths),
          unit="resize", priv_kb=d.priv_kb, peak_kb=d.peak_kb, budget_ms=30.0)
    b.keys("scroll", s, ["pageup"] * b.scale(15, floor=5), budget_ms=8.0)
    b.alive(s)


@slow
@needs("proc")
def bench_pathological_markdown(b):
    """Markdown built to be awkward: deep nesting, long tables, ragged fences.

    None of this renders usefully; what matters is that it renders at all, in
    a time that does not depend on how badly it is formed.
    """
    parts = []
    rows = b.scale(200, floor=40)
    parts.append("| " + " | ".join(f"col{i}" for i in range(12)) + " |")
    parts.append("| " + " | ".join("---" for _ in range(12)) + " |")
    for r in range(rows):
        parts.append("| " + " | ".join(f"{r}-{c}" for c in range(12)) + " |")
    parts.append("\n".join("  " * (i % 12) + f"- item {i}" for i in range(rows)))
    parts.append("\n".join(">" * (i % 8 + 1) + f" quote {i}" for i in range(rows)))
    parts.append("```" + "\n```".join(f"unclosed {i}" for i in range(20)))
    parts.append("*" * 400 + "\n" + "`" * 400 + "\n" + "#" * 400)
    parts.append("\n".join(f"[link {i}](http://example.invalid/{i})" for i in range(rows)))
    doc = "\n\n".join(parts)
    s = b.spawn()
    b.ctx.scenario(Scenario(text=doc, chunk=6))
    with b.step("render", units=len(doc), unit="byte", budget_ms=0.5):
        s.submit("render this")
        s.wait_turn_done(timeout=300.0)
    b.keys("scroll", s, ["pageup"] * b.scale(20, floor=5), budget_ms=10.0)
    b.alive(s)
