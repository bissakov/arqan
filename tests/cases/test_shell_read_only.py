"""Read-only shell commands: a pipeline of reading programs runs without asking.

The classifier is a first-word check over each segment of the pipeline that
fails closed on redirection, substitution, grouping, heredocs and background
jobs. Anything it does not pass is an ordinary bash call and asks as before.
"""

import json


def bash(command: str, description: str = "look") -> str:
    return "tool=bash:" + json.dumps({"command": command, "description": description})


def a_small_tree(ctx):
    ctx.write_file("src/one.c", "int alpha(void);\n")
    ctx.write_file("notes.txt", "alpha is a letter\nbeta is another\n")


READ_ONLY = [
    "grep -rn alpha . | sort | head -5",
    "cd src && ls -la",
    "sed -n '1,2p' notes.txt",
    "cat notes.txt 2>/dev/null | wc -l",
    "git -C . status --short 2>&1",
    "FOO=1 ls notes.txt || true",
    "find . -name '*.txt' | wc -l",
    "timeout 5 ls",
    'echo "a > b is not a redirect"',
    "grep -c 'x|y' notes.txt; true",
    "test -f notes.txt && echo yes",
    "git log --oneline -3 >/dev/null; echo ok",
    "tail -n 1 notes.txt\nhead -n 1 notes.txt",
]


def test_read_only_pipelines_run_in_ask_mode_without_a_prompt(ctx):
    a_small_tree(ctx)
    ctx.scenario(",".join(bash(c) for c in READ_ONLY) + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("look around")
    s.wait_text("done")
    s.wait_turn_done()

    assert "allow bash?" not in s.text(), s.text()
    results = ctx.mock.tool_results()
    assert len(results) == len(READ_ONLY), results
    assert not any(r.startswith("DENIED:") for r in results), results
    assert "notes.txt:1:alpha is a letter" in results[0], results[0]
    assert results[2].startswith("alpha is a letter\nbeta is another\n"), results[2]


WRITING = [
    "echo hi > out.txt",
    "echo hi >> out.txt",
    "cat $(echo notes.txt)",
    'grep "$(id)" notes.txt',
    "cat `echo notes.txt`",
    "sed -i s/alpha/omega/ notes.txt",
    "sed -ni p notes.txt",
    "find . -name '*.txt' -delete",
    "find . -name '*.txt' -exec rm {} \\;",
    "git add .",
    "git log --output=out.txt",
    "python3 -c 'open(\"out.txt\",\"w\")'",
    "ls; rm -f notes.txt",
    "ls && touch out.txt",
    "cat <<EOF\nhi\nEOF",
    "ls &",
    "ls |& cat",
    "sort -o out.txt notes.txt",
    "(ls)",
    "{ ls; }",
    "cat notes.txt | tee out.txt",
    "rg --pre cat alpha .",
    "cat < notes.txt",
    "env FOO=1 touch out.txt",
    "/bin/ls",
    "",
]


def test_anything_else_asks_as_before(ctx):
    a_small_tree(ctx)
    ctx.scenario(",".join(bash(c, f"call {i:02d}") for i, c in enumerate(WRITING))
                 + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("change things")
    for i, _ in enumerate(WRITING):
        s.wait_text(f"call {i:02d}")
        s.wait_status("allow bash?")
        s.key("esc")
    s.wait_text("done")
    s.wait_turn_done()

    results = ctx.mock.tool_results()
    assert len(results) == len(WRITING), results
    assert all(r.startswith("DENIED:") for r in results), results
    assert not (ctx.work / "out.txt").exists()
    assert (ctx.work / "notes.txt").read_text().startswith("alpha is a letter")


def test_a_remembered_grant_still_covers_the_writing_commands(ctx):
    """The read-only rule narrows what asks; it does not change what a grant
    means."""
    a_small_tree(ctx)
    ctx.scenario(bash("touch first.txt") + "," + bash("ls") + ","
                 + bash("touch second.txt") + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("make files")
    s.wait_status("allow bash?")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("done")
    s.wait_turn_done()

    assert (ctx.work / "first.txt").exists()
    assert (ctx.work / "second.txt").exists()
    assert "notes.txt" in ctx.mock.tool_results()[1], ctx.mock.tool_results()
