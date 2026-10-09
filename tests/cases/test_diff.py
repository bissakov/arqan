"""/diff: what git says changed in the files this session wrote."""

import json
import subprocess

from tests.cases.test_shell_read_only import git_available
from tests.cases.test_tools import patch_call

MUTED = 245
ADD_BG = 22
DEL_BG = 52


def git(ctx, *args):
    subprocess.run(
        ["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid",
         "-c", "commit.gpgsign=false", *args],
        cwd=str(ctx.work), env=ctx.env(), check=True, capture_output=True,
    )


def repo_with(ctx, files: dict):
    """A git repository in the working directory with `files` committed."""
    git(ctx, "init", "-q")
    for name, body in files.items():
        ctx.write_file(name, body)
    git(ctx, "add", "-A")
    git(ctx, "commit", "-q", "-m", "init")


def wait_window(s):
    s.wait_for(lambda t: t.contains("┌") and t.contains("┐"),
               "the window to open")
    s.settle()


def window_rows(s):
    """Rows between the window's top and bottom borders."""
    lines = s.screen.lines()
    top = next(i for i, line in enumerate(lines) if "┌" in line and "┐" in line)
    bottom = next(i for i, line in enumerate(lines[top + 1:], top + 1)
                  if "└" in line and "┘" in line)
    return range(top + 1, bottom)


def window_row(s, needle: str) -> int:
    for r in window_rows(s):
        if needle in s.screen.row_text(r):
            return r
    raise AssertionError(f"{needle!r} is not in the window:\n{s.text()}")


def patch_turn(ctx, s):
    diff = (
        "--- a/diff.txt\n+++ b/diff.txt\n@@ -1,3 +1,3 @@\n"
        " keep\n-old one\n+new one\n keep\n"
    )
    ctx.scenario(patch_call(diff, final_text="patched"))
    s.submit("patch it")
    s.wait_text("patched")
    s.wait_turn_done()


def write_turn(ctx, s, path="new.txt", body="hello\n"):
    args = json.dumps({"path": path, "content": body})
    ctx.scenario(f"tool=write:{args},final_text=written")
    s.submit("write it")
    s.wait_text("written")
    s.wait_turn_done()


def test_diff_with_nothing_written_says_so(ctx):
    """Before any write or patch there is nothing to diff."""
    s = ctx.spawn()
    s.submit("/diff")
    s.wait_text("no files were written in this session")


def test_diff_without_git_says_git_is_missing(ctx):
    """With no git on PATH the notice names the missing program."""
    s = ctx.spawn(PATH=str(ctx.home / "no-bin"))
    write_turn(ctx, s)

    s.submit("/diff")
    s.wait_text("git is not installed")


def test_diff_shows_a_patched_file_with_tinted_rows(ctx):
    """A patched, committed file shows as git's diff: removed rows on the
    delete tint, added rows on the add tint, headers muted."""
    if not git_available():
        return
    repo_with(ctx, {"diff.txt": "keep\nold one\nkeep\n"})
    s = ctx.spawn()
    patch_turn(ctx, s)

    s.submit("/diff")
    wait_window(s)
    old_row = window_row(s, "-old one")
    new_row = window_row(s, "+new one")
    head_row = window_row(s, "--- a/diff.txt")
    old_col = s.screen.row_text(old_row).index("-old one")
    new_col = s.screen.row_text(new_row).index("+new one")
    head_col = s.screen.row_text(head_row).index("--- a/diff.txt")
    assert s.screen.attr_at(old_row, old_col).bg == DEL_BG
    assert s.screen.attr_at(old_row, old_col + 3).bg == DEL_BG
    assert s.screen.attr_at(new_row, new_col).bg == ADD_BG
    assert s.screen.attr_at(new_row, new_col + 3).bg == ADD_BG
    assert s.screen.attr_at(head_row, head_col).fg == MUTED
    ctx.check_screen(s)


def test_diff_shows_a_new_file_against_dev_null(ctx):
    """A file git does not track yet is diffed against /dev/null."""
    if not git_available():
        return
    repo_with(ctx, {"keep.txt": "keep\n"})
    s = ctx.spawn()
    write_turn(ctx, s)

    s.submit("/diff")
    wait_window(s)
    window_row(s, "+++ b/new.txt")
    window_row(s, "--- /dev/null")
    window_row(s, "+hello")


def test_diff_outside_a_repository_carries_gits_message(ctx):
    """Without a repository the notice says what git said."""
    if not git_available():
        return
    s = ctx.spawn()
    write_turn(ctx, s)

    s.submit("/diff")
    s.wait_text("a git repository")


def test_diff_survives_resume_and_resets_on_clear(ctx):
    """The touched list is saved with the session and comes back with it;
    /clear starts a fresh one."""
    if not git_available():
        return
    repo_with(ctx, {"keep.txt": "keep\n"})
    s = ctx.spawn()
    write_turn(ctx, s)
    s.submit("/exit")
    s.wait_exit()

    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("written")
    s.submit("/diff")
    wait_window(s)
    window_row(s, "+++ b/new.txt")
    s.key("esc").sync()

    s.submit("/clear")
    s.wait_for(lambda t: not t.contains("write it"), "the transcript to clear")
    s.submit("/diff")
    s.wait_text("no files were written in this session")


def test_diff_stays_off_the_wire(ctx):
    """/diff is for the user; the next request carries no diff text."""
    if not git_available():
        return
    repo_with(ctx, {"diff.txt": "keep\nold one\nkeep\n"})
    s = ctx.spawn()
    patch_turn(ctx, s)
    s.submit("/diff")
    wait_window(s)
    window_row(s, "+new one")
    s.key("esc").sync()

    ctx.scenario("text=next+reply,final_text=next+reply")
    s.submit("and now")
    s.wait_text("next reply")
    s.wait_turn_done()
    messages = ctx.mock.requests[-1]["messages"]
    assert "diff --git" not in json.dumps(messages), messages
