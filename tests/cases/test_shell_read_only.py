"""Read-only shell commands: a pipeline of reading programs runs without asking.

The classifier is a first-word check over each segment of the pipeline that
fails closed on redirection, substitution, grouping, heredocs and background
jobs. Anything it does not pass is an ordinary bash call and asks as before.
A read-only command that names a path outside the project asks like `read`
does, and on Linux a read-only command runs under Landlock with writes and TCP
denied and reads limited to the project and the system directories.
"""

import ctypes
import json
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib.parse import urlparse


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


def landlock_abi() -> int:
    """The kernel's Landlock ABI version, or 0 without Landlock."""
    if not sys.platform.startswith("linux"):
        return 0
    libc = ctypes.CDLL(None, use_errno=True)
    libc.syscall.restype = ctypes.c_long
    abi = libc.syscall(ctypes.c_long(444), None, ctypes.c_size_t(0),
                       ctypes.c_uint32(1))
    return max(0, abi)


def git(ctx, *args, cwd=None):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t",
                    "-c", "init.defaultBranch=main", *args],
                   cwd=cwd or ctx.work, check=True, capture_output=True,
                   env={**os.environ, "HOME": str(ctx.home),
                        "XDG_CONFIG_HOME": str(ctx.xdg)})


def a_committed_tree(ctx, at=None):
    at = at or ctx.work
    git(ctx, "init", "-q", cwd=at)
    git(ctx, "add", ".", cwd=at)
    git(ctx, "commit", "-q", "-m", "init", cwd=at)


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


def test_a_link_out_of_the_project_is_not_followed_under_the_sandbox(ctx):
    """The words name a file inside, so nothing asks; the read itself is what
    leaves the project, and the sandbox refuses it."""
    if not landlock_available():
        return
    (ctx.home / "secret").write_text("outside body\n")
    os.symlink(ctx.home / "secret", ctx.work / "link")
    ctx.scenario(bash("cat link") + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("read the link")
    s.wait_text("done")
    s.wait_turn_done()

    assert "allow" not in s.text(), s.text()
    result = ctx.mock.tool_results()[0]
    assert "Permission denied" in result, result
    assert "outside body" not in result, result


def test_a_git_driver_runs_inside_the_sandbox(ctx):
    """The repository's own config names a textconv driver. It runs, but it
    cannot write, read outside the project, or open a TCP connection."""
    if not landlock_available():
        return
    a_small_tree(ctx)
    a_committed_tree(ctx)
    (ctx.home / "secret").write_text("outside body\n")
    url = urlparse(ctx.mock.base_url)
    probe = ctx.work / ".git" / "probe"
    probe.write_text(f"""#!/bin/sh
echo probe-ran
if echo leaked > '{ctx.work}/leaked.txt'; then echo write-ok; else echo write-denied; fi
if cat '{ctx.home}/secret' >/dev/null; then echo read-ok; else echo read-denied; fi
python3 -c '
import socket
try:
    socket.create_connection(("{url.hostname}", {url.port}), 2).close()
    print("tcp-ok")
except OSError as e:
    print("tcp-denied", e.errno)
'
cat "$1"
""")
    os.chmod(probe, 0o755)
    ctx.write_file(".gitattributes", "*.txt diff=probe\n")
    ctx.write_file("fresh.txt", "fresh body\n")
    git(ctx, "config", "diff.probe.textconv", str(probe))
    git(ctx, "add", "-N", "fresh.txt")
    ctx.scenario(bash("git diff") + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("show the diff")
    s.wait_text("done")
    s.wait_turn_done()

    assert "allow" not in s.text(), s.text()
    result = ctx.mock.tool_results()[0]
    assert "probe-ran" in result, result
    assert "write-denied" in result and "write-ok" not in result, result
    assert not (ctx.work / "leaked.txt").exists()
    assert "read-denied" in result and "read-ok" not in result, result
    if landlock_abi() >= 4:
        assert "tcp-denied" in result and "tcp-ok" not in result, result


def test_git_status_does_not_run_the_repository_fsmonitor(ctx):
    if not landlock_available():
        return
    a_small_tree(ctx)
    a_committed_tree(ctx)
    monitor = ctx.work / ".git" / "monitor"
    monitor.write_text("#!/bin/sh\necho fsmonitor-ran >&2\nexit 1\n")
    os.chmod(monitor, 0o755)
    git(ctx, "config", "core.fsmonitor", str(monitor))
    ctx.scenario(bash("git status --short") + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("status")
    s.wait_text("done")
    s.wait_turn_done()

    assert "allow" not in s.text(), s.text()
    result = ctx.mock.tool_results()[0]
    assert "[exit 0]" in result, result
    assert "fsmonitor-ran" not in result, result


GIT_READS = [
    "git status --short",
    "git log --oneline -1",
    "git rev-parse --show-toplevel",
]


def git_reads_work_from(ctx, cwd: Path):
    ctx.scenario(",".join(bash(c) for c in GIT_READS) + ",final_text=done")
    s = ctx.spawn(cwd=str(cwd), ARQAN_PERMISSIONS="ask")
    s.submit("look at the repository")
    s.wait_text("done")
    s.wait_turn_done()

    assert "allow" not in s.text(), s.text()
    results = ctx.mock.tool_results()
    assert len(results) == len(GIT_READS), results
    for r in results:
        assert "[exit 0]" in r, results
    assert "init" in results[1], results[1]


def test_git_works_from_a_subdirectory_of_the_repository(ctx):
    a_small_tree(ctx)
    a_committed_tree(ctx)
    git_reads_work_from(ctx, ctx.work / "src")


def test_git_works_from_a_linked_worktree(ctx):
    a_small_tree(ctx)
    a_committed_tree(ctx)
    git(ctx, "worktree", "add", "-q", str(ctx.home / "linked"))
    git_reads_work_from(ctx, ctx.home / "linked")


def test_git_works_where_dot_git_is_a_file(ctx):
    """A submodule checkout: `.git` is a file naming the git directory by a
    relative path."""
    checkout = ctx.home / "module"
    checkout.mkdir()
    (checkout / "notes.txt").write_text("alpha\n")
    git(ctx, "init", "-q", "--separate-git-dir", str(ctx.home / "module.git"),
        cwd=checkout)
    (checkout / ".git").write_text("gitdir: ../module.git\n")
    git(ctx, "add", ".", cwd=checkout)
    git(ctx, "commit", "-q", "-m", "init", cwd=checkout)
    git_reads_work_from(ctx, checkout)


DRIVER_GIT = [
    "git diff",
    "git show",
    "git log -p -1",
    "git blame notes.txt",
    "git status",
    "git describe --always",
    "git grep alpha",
]
PLAIN_GIT = [
    "git ls-files",
    "git rev-parse HEAD",
    "git shortlog -s HEAD",
]


def test_without_landlock_git_that_can_run_a_driver_asks(ctx):
    a_small_tree(ctx)
    a_committed_tree(ctx)
    ctx.scenario(",".join(bash(c, f"plain {i:02d}") for i, c in enumerate(PLAIN_GIT))
                 + "," + ",".join(bash(c, f"call {i:02d}")
                                  for i, c in enumerate(DRIVER_GIT))
                 + ",final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask", ARQAN_TEST_NO_LANDLOCK="1")
    s.submit("look at the repository")
    for i, _ in enumerate(DRIVER_GIT):
        s.wait_text(f"call {i:02d}")
        s.wait_status("allow bash?")
        s.key("esc")
    s.wait_text("done")
    s.wait_turn_done()

    results = ctx.mock.tool_results()
    assert len(results) == len(PLAIN_GIT) + len(DRIVER_GIT), results
    plain = results[:len(PLAIN_GIT)]
    assert all("[exit 0]" in r for r in plain), plain
    assert all(r.startswith("DENIED:") for r in results[len(PLAIN_GIT):]), results


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
            [*shlex.split(os.environ.get("CC") or "cc"), "-std=c17",
             "-fsyntax-only", "-Wall", "-Wextra", "-Wpedantic", "-Wconversion",
             "-Werror", "-DAGENT_CURL_DLOPEN=1", "-isystem", fake, "src/main.c"],
            cwd=ROOT, capture_output=True, text=True, timeout=60)
    assert build.returncode == 0, build.stderr
