"""Ctrl-G: the composer draft, edited in vi, vim or nvim.

The editor is a stand-in script named like the real one. It records the file
it was given and its arguments, then does what FAKE_EDITOR_ACTION says, so a
case asserts on the plumbing without a real vim on the machine.
"""

import json
import os
import stat
from pathlib import Path

FAKE_EDITOR = """#!/usr/bin/env python3
import json, os, sys
path = sys.argv[-1]
with open(path, encoding="utf-8") as f:
    before = f.read()
record = {
    "name": os.path.basename(sys.argv[0]),
    "argv": sys.argv[1:],
    "before": before,
    "suffix": os.path.splitext(path)[1],
    "mode": oct(os.stat(path).st_mode & 0o777),
}
with open(os.environ["FAKE_EDITOR_LOG"], "a", encoding="utf-8") as f:
    f.write(json.dumps(record) + "\\n")
action = os.environ.get("FAKE_EDITOR_ACTION", "keep")
if action.startswith("write:"):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(action[len("write:"):] + "\\n")
elif action == "fail":
    with open(path, "w", encoding="utf-8") as f:
        f.write("thrown away\\n")
    sys.exit(1)
elif action == "big":
    with open(path, "w", encoding="utf-8") as f:
        f.write("x" * (1 << 20) + "\\n")
"""


def editors(ctx, *names: str) -> Path:
    """A directory of stand-in editors, one per name."""
    bin_dir = ctx.tmp / "editors"
    bin_dir.mkdir(exist_ok=True)
    for name in names:
        p = bin_dir / name
        p.write_text(FAKE_EDITOR)
        p.chmod(p.stat().st_mode | stat.S_IXUSR)
    return bin_dir


def log_path(ctx) -> Path:
    return ctx.tmp / "editor.log"


def runs(ctx) -> list[dict]:
    p = log_path(ctx)
    if not p.exists():
        return []
    return [json.loads(line) for line in p.read_text().splitlines()]


def spawn(ctx, bin_dir: Path, action: str = "keep", **env):
    path = f"{bin_dir}:{os.environ.get('PATH', '/usr/bin:/bin')}"
    env.setdefault("PATH", path)
    return ctx.spawn(
        FAKE_EDITOR_LOG=str(log_path(ctx)), FAKE_EDITOR_ACTION=action, **env
    )


def test_ctrl_g_puts_the_saved_file_in_the_composer(ctx):
    """The draft goes in, the saved text comes back, and nothing is sent."""
    bin_dir = editors(ctx, "vim")
    s = spawn(ctx, bin_dir, "write:hello from vim", EDITOR=bin_dir / "vim")
    s.type("hello").sync()
    s.key("ctrl-g").sync()
    s.wait_for(lambda t: s.composer_text() == "hello from vim", "the saved text")
    [run] = runs(ctx)
    assert run["name"] == "vim", run
    assert run["before"] == "hello", run
    assert run["suffix"] == ".md", run
    assert run["mode"] == "0o600", run
    assert ctx.mock.requests == [], "the editor never sends the message"


def test_vim_opens_at_the_composer_cursor(ctx):
    """Line and byte column of the cursor reach vim as a cursor() call."""
    bin_dir = editors(ctx, "nvim")
    s = spawn(ctx, bin_dir, EDITOR=bin_dir / "nvim")
    s.type("one").key("newline").type("two").key("left").sync()
    s.key("ctrl-g").sync()
    [run] = runs(ctx)
    assert run["before"] == "one\ntwo", run
    assert run["argv"][0] == "+call cursor(2,3)", run


def test_vi_opens_at_the_cursor_line(ctx):
    """Plain vi may not be vim, so it gets only the line."""
    bin_dir = editors(ctx, "vi")
    s = spawn(ctx, bin_dir, EDITOR=bin_dir / "vi")
    s.type("one").key("newline").type("two").sync()
    s.key("ctrl-g").sync()
    [run] = runs(ctx)
    assert run["argv"][0] == "+2", run


def test_an_unchanged_file_keeps_the_draft(ctx):
    """Quitting without a write is a cancel."""
    bin_dir = editors(ctx, "vim")
    s = spawn(ctx, bin_dir, "keep", EDITOR=bin_dir / "vim")
    s.type("keep me").sync()
    s.key("ctrl-g").sync()
    assert len(runs(ctx)) == 1
    assert s.composer_text() == "keep me", s.composer_text()


def test_a_failed_exit_keeps_the_draft(ctx):
    """:cq exits non-zero, which drops what the file holds."""
    bin_dir = editors(ctx, "vim")
    s = spawn(ctx, bin_dir, "fail", EDITOR=bin_dir / "vim")
    s.type("keep me").sync()
    s.key("ctrl-g")
    s.wait_text("the draft is unchanged")
    assert "status 1" in s.text(), s.text()
    assert s.composer_text() == "keep me", s.composer_text()


def test_saved_text_is_cleaned_like_a_paste(ctx):
    """Tabs become spaces and CRLF becomes LF, as they do in a paste."""
    bin_dir = editors(ctx, "vim")
    s = spawn(ctx, bin_dir, "write:a\tb\r\nc", EDITOR=bin_dir / "vim")
    s.type("x").sync()
    s.key("ctrl-g").sync()
    s.wait_for(lambda t: s.composer_body(2) == ["a    b", "c"], "the cleaned text")


def test_text_over_the_composer_limit_is_refused(ctx):
    """The composer is not cut short: the old draft stays and the limit is named."""
    bin_dir = editors(ctx, "vim")
    s = spawn(ctx, bin_dir, "big", EDITOR=bin_dir / "vim")
    s.type("keep me").sync()
    s.key("ctrl-g")
    s.wait_text("the draft is unchanged")
    assert "composer's 1.0 MB" in s.text(), s.text()
    assert s.composer_text() == "keep me", s.composer_text()


def test_visual_comes_before_editor(ctx):
    """VISUAL wins when both name a vim."""
    bin_dir = editors(ctx, "vi", "vim")
    s = spawn(ctx, bin_dir, VISUAL=bin_dir / "vi", EDITOR=bin_dir / "vim")
    s.type("x").sync()
    s.key("ctrl-g").sync()
    [run] = runs(ctx)
    assert run["name"] == "vi", run


def test_an_editor_that_is_not_a_vim_is_passed_over(ctx):
    """EDITOR=nano is ignored and the first vim on PATH runs instead."""
    bin_dir = editors(ctx, "nano", "nvim")
    s = spawn(ctx, bin_dir, EDITOR=bin_dir / "nano")
    s.type("x").sync()
    s.key("ctrl-g").sync()
    [run] = runs(ctx)
    assert run["name"] == "nvim", run


def test_extra_words_in_editor_reach_vim(ctx):
    """EDITOR may carry options; they come before the cursor and the file."""
    bin_dir = editors(ctx, "vim")
    s = spawn(ctx, bin_dir, EDITOR=f"{bin_dir / 'vim'} -u NONE")
    s.type("x").sync()
    s.key("ctrl-g").sync()
    [run] = runs(ctx)
    assert run["argv"][:3] == ["-u", "NONE", "+call cursor(1,2)"], run


def test_no_vim_on_path_says_so(ctx):
    """Without vi, vim or nvim the draft stays and the notice names them."""
    empty = ctx.tmp / "no-editors"
    empty.mkdir()
    s = ctx.spawn(PATH=str(empty), EDITOR=None, VISUAL=None)
    s.type("keep me").sync()
    s.key("ctrl-g")
    s.wait_text("no vi, vim or nvim")
    assert s.composer_text() == "keep me", s.composer_text()


def test_ctrl_g_waits_for_the_turn(ctx):
    """While a turn runs the editor does not open, and the notice says why."""
    bin_dir = editors(ctx, "vim")
    ctx.scenario("hold,text=done")
    s = spawn(ctx, bin_dir, "write:nope", EDITOR=bin_dir / "vim")
    s.submit("go on")
    s.wait_activity("thinking")
    s.type("later").sync()
    s.key("ctrl-g")
    s.wait_text("the editor opens between turns")
    assert runs(ctx) == [], runs(ctx)
    assert s.composer_text() == "later", s.composer_text()
    ctx.mock.release()
    s.wait_turn_done()
