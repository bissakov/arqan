"""Read-only shell commands: a pipeline of reading programs runs without asking.

The classifier is a first-word check over each segment of the pipeline that
fails closed on redirection, substitution, grouping, heredocs and background
jobs. Anything it does not pass is an ordinary bash call and asks as before.
A read-only command that names a path outside the project asks like `read`
does, and on Linux a read-only command runs under Landlock with writes denied.
"""

import json
import os
import subprocess
import tempfile
from pathlib import Path


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
    "find . -name '*.txt' | wc -l",
    "timeout 5 ls",
    'echo "a > b is not a redirect"',
    "grep -c 'x|y' notes.txt; true",
    "test -f notes.txt && echo yes",
    "git log --oneline -3 >/dev/null; echo ok",
    "tail -n 1 notes.txt\nhead -n 1 notes.txt",
    "sed -n 's/alpha/omega/p' notes.txt",
    "sed -E '/^a/!d' notes.txt",
    "awk -F: '{print $1}' notes.txt",
    "awk 'NR==1' notes.txt",
    "sort -r notes.txt",
    "diff notes.txt /dev/null; true",
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
    "FOO=1 ls notes.txt || true",
    "PATH=/tmp/x ls",
    "awk '{system(\"touch out.txt\")}' notes.txt",
    "awk '{print > \"out.txt\"}' notes.txt",
    "awk -f prog.awk notes.txt",
    "sed 's/alpha/omega/w out.txt' notes.txt",
    "sed '1e touch out.txt' notes.txt",
    "sed --in-p s/a/b/ notes.txt",
    "sed -f script notes.txt",
    "sort --o=out.txt notes.txt",
    "sort --compress-program=touch notes.txt",
    "xxd notes.txt out.txt",
    "tree -o out.txt",
    "rg --hostname-bin=touch alpha",
    "git grep -Otouch alpha",
    "git log --out=out.txt",
    'sed -"i" s/a/b/ notes.txt',
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


OUTSIDE = [
    "cat ~/.bashrc",
    "ls /etc",
    "cat ../secret",
    "echo $HOME",
    "cd .. && ls",
    "grep -r x --file=/etc/passwd",
]


def test_a_read_only_command_on_an_outside_path_asks_like_read(ctx):
    a_small_tree(ctx)
    (ctx.home / "secret").write_text("outside body\n")
    ctx.scenario(",".join(bash(c, f"call {i:02d}") for i, c in enumerate(OUTSIDE))
                 + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("look outside")
    for i, _ in enumerate(OUTSIDE):
        s.wait_text(f"call {i:02d}")
        s.wait_status("allow read outside the project?")
        s.key("esc")
    s.wait_text("done")
    s.wait_turn_done()

    assert "allow bash?" not in s.text(), s.text()
    results = ctx.mock.tool_results()
    assert len(results) == len(OUTSIDE), results
    assert all(r.startswith("DENIED:") for r in results), results
    assert not any("outside body" in r for r in results), results


def test_a_remembered_outside_grant_covers_later_outside_commands(ctx):
    a_small_tree(ctx)
    (ctx.home / "secret").write_text("outside body\n")
    ctx.scenario(",".join(bash(c, f"call {i:02d}") for i, c in enumerate(OUTSIDE))
                 + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("look outside")
    s.wait_status("allow read outside the project?")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("done")
    s.wait_turn_done()

    results = ctx.mock.tool_results()
    assert len(results) == len(OUTSIDE), results
    assert not any(r.startswith("DENIED:") for r in results), results
    assert "outside body" in results[2], results


def landlock_available() -> bool:
    lsm = Path("/sys/kernel/security/lsm")
    return lsm.exists() and "landlock" in lsm.read_text()


def test_a_read_only_command_cannot_write_under_the_sandbox(ctx):
    """The classifier lets `sort` through; Landlock stops its temporary file.
    Git's opportunistic index refresh must tolerate the same denial."""
    if not landlock_available():
        return
    a_small_tree(ctx)
    ctx.write_file("big.txt", "".join(f"line {i:08d} {'x' * 40}\n"
                                      for i in range(4000)))
    git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
    for args in (["init", "-q"], ["add", "."], ["commit", "-q", "-m", "init"]):
        subprocess.run(git + args, cwd=ctx.work, check=True,
                       env={**os.environ, "HOME": str(ctx.home)})
    ctx.scenario(bash("sort -S 1K -T . big.txt | tail -n 1") + ","
                 + bash("git status --short") + ","
                 + bash("git log --oneline -1") + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("sort it")
    s.wait_text("done")
    s.wait_turn_done()

    assert "allow bash?" not in s.text(), s.text()
    results = ctx.mock.tool_results()
    assert len(results) == 3, results
    assert "Permission denied" in results[0], results[0]
    assert not [p for p in os.listdir(ctx.work) if p.startswith("sort")], \
        os.listdir(ctx.work)
    assert "[exit 0]" in results[1], results[1]
    assert "init" in results[2] and "[exit 0]" in results[2], results[2]


ROOT = Path(__file__).resolve().parent.parent.parent


def test_the_sandbox_builds_without_landlock_headers(ctx):
    """The release image, Debian 11, has no <linux/landlock.h> and a libc
    without the Landlock syscall numbers; the source must build there."""
    with tempfile.TemporaryDirectory() as fake:
        (Path(fake) / "linux").mkdir()
        (Path(fake) / "sys").mkdir()
        (Path(fake) / "linux" / "landlock.h").write_text(
            "#error the Landlock header is not installed\n")
        (Path(fake) / "sys" / "syscall.h").write_text(
            "#include_next <sys/syscall.h>\n"
            "#undef SYS_landlock_create_ruleset\n"
            "#undef SYS_landlock_add_rule\n"
            "#undef SYS_landlock_restrict_self\n")
        build = subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c17", "-fsyntax-only",
             "-Wall", "-Wextra", "-Wpedantic", "-Wconversion", "-Werror",
             "-DAGENT_CURL_DLOPEN=1", "-isystem", fake, "src/main.c"],
            cwd=ROOT, capture_output=True, text=True, timeout=60)
    assert build.returncode == 0, build.stderr
