"""Optional Tree-sitter syntax highlighting and its companion protocol."""

from __future__ import annotations

import json
import struct
import subprocess
import time

from tests.context import ROOT


HELPER = ROOT / "bin" / "yoke-highlight"
MAGIC = b"YHL1"
REQ = struct.Struct("<4sIB3xII")
RESP = struct.Struct("<4sIB3xI")
RUN = struct.Struct("<IIB3x")

OK = 0
UNKNOWN = 1
TOO_LARGE = 2
TOO_COMPLEX = 3

TEXT = 253
MUTED = 245
GREEN = 114
YELLOW = 221
PURPLE = 177
CYAN = 81
BLUE = 75
MONO = 180
CODE_BG = 235


def request(proc, request_id: int, hint_kind: int, hint: bytes, source: bytes):
    proc.stdin.write(REQ.pack(MAGIC, request_id, hint_kind, len(hint), len(source)))
    proc.stdin.write(hint + source)
    proc.stdin.flush()
    header = proc.stdout.read(RESP.size)
    assert len(header) == RESP.size, header
    magic, got_id, status, count = RESP.unpack(header)
    assert magic == MAGIC and got_id == request_id
    runs = [RUN.unpack(proc.stdout.read(RUN.size)) for _ in range(count)]
    return status, runs


def helper():
    return subprocess.Popen(
        [str(HELPER)], stdin=subprocess.PIPE, stdout=subprocess.PIPE
    )


def cell(s, needle: str, offset: int = 0):
    for row in range(s.screen.rows):
        text = s.screen.row_text(row)
        if needle in text:
            return s.screen.attr_at(row, text.index(needle) + offset)
    raise AssertionError(f"{needle!r} is not on screen:\n{s.text()}")


def test_helper_diagnostics_are_stable(ctx):
    """The private diagnostic commands expose only version and language names."""
    version = subprocess.run([HELPER, "--version"], capture_output=True, text=True)
    assert version.returncode == 0
    assert version.stdout == "yoke-highlight 1 (tree-sitter 0.26.11)\n"
    listed = subprocess.run(
        [HELPER, "--list-languages"], capture_output=True, text=True
    )
    assert listed.returncode == 0
    assert listed.stdout.splitlines() == [
        "c", "cpp", "rust", "go", "python", "javascript", "typescript",
        "tsx", "bash", "json", "toml", "yaml",
    ]


def test_helper_highlights_repeated_requests(ctx):
    """One helper handles several languages without restarting."""
    cases = [
        (b"c", b"// note\nint main(void) { return 7; }\n"),
        (b"python", b"# note\ndef f():\n    return 'yes'\n"),
        (b"json", b'{"ready": true, "count": 3}\n'),
        (b"bash", b"# note\nif true; then echo 'yes'; fi\n"),
        (b"typescript", b"interface Item { name: string }\nconst n = 3;\n"),
    ]
    proc = helper()
    try:
        for request_id, (hint, source) in enumerate(cases, 1):
            status, runs = request(proc, request_id, 1, hint, source)
            assert status == OK, (hint, status)
            assert runs, hint
            assert len(runs) <= 16384
            assert all(0 <= a < b <= len(source) and 1 <= kind <= 7
                       for a, b, kind in runs)
            assert all(runs[i - 1][1] <= runs[i][0]
                       for i in range(1, len(runs)))
    finally:
        proc.terminate()
        proc.wait(timeout=2)


def test_every_bundled_language_has_a_capture(ctx):
    """Each shipped parser and query pair produces at least one useful run."""
    cases = {
        b"c": b"int n = 1; // c\n",
        b"cpp": b"class C { public: int n = 1; }; // c\n",
        b"rust": b"fn main() { let n: i32 = 1; } // c\n",
        b"go": b"package main\nfunc main() { n := 1 } // c\n",
        b"python": b"def f(): return 1 # c\n",
        b"javascript": b"function f() { return 1; } // c\n",
        b"typescript": b"const n: number = 1; // c\n",
        b"tsx": b"const x = <div title='s'>x</div>; // c\n",
        b"bash": b"if true; then echo 1; fi # c\n",
        b"json": b'{"n": 1, "ok": true}\n',
        b"toml": b"n = 1 # c\n",
        b"yaml": b"ok: true # c\n",
    }
    proc = helper()
    try:
        for request_id, (hint, source) in enumerate(cases.items(), 1):
            status, runs = request(proc, request_id, 1, hint, source)
            assert status == OK and runs, (hint, status, runs)
    finally:
        proc.terminate()
        proc.wait(timeout=2)


def test_helper_request_local_fallbacks_keep_it_alive(ctx):
    """Unknown and oversized requests do not terminate the persistent helper."""
    proc = helper()
    try:
        status, runs = request(proc, 1, 1, b"unknown", b"plain\n")
        assert status == UNKNOWN and runs == []
        status, runs = request(proc, 2, 1, b"c", b"x" * 65537)
        assert status == TOO_LARGE and runs == []
        dense = b"1 " * 17000
        status, runs = request(proc, 3, 1, b"c", dense)
        assert status == TOO_COMPLEX and runs == []
        status, runs = request(proc, 4, 1, b"c", b"int n = 1;\n")
        assert status == OK and runs
    finally:
        proc.terminate()
        proc.wait(timeout=2)


def test_predicate_patterns_are_disabled(ctx):
    """A capture that depends on a predicate is not applied speculatively."""
    source = b"const X = value;\n"
    proc = helper()
    try:
        status, runs = request(proc, 1, 1, b"javascript", source)
        assert status == OK and runs
        x = source.index(b"X")
        assert not any(a <= x < b for a, b, _ in runs), runs
    finally:
        proc.terminate()
        proc.wait(timeout=2)


def test_closed_fence_gains_syntax_colours(ctx):
    """A closed C fence keeps its code panel and gains semantic foregrounds."""
    ctx.scenario("text=```c\n//+note\nint+main(void)+{+return+\"ok\"[0];+}\n```")
    s = ctx.spawn()
    s.submit("show C")
    s.wait_text('return "ok"')
    s.wait_turn_done()
    assert cell(s, "int main").bg == CODE_BG
    assert cell(s, "int main").fg == CYAN
    assert cell(s, "return").fg == PURPLE
    assert cell(s, '"ok"').fg == GREEN
    assert cell(s, "// note").fg == MUTED


def test_open_fence_streams_before_retroactive_colour(ctx):
    """Code is readable immediately and is recoloured only after the close."""
    ctx.scenario("text=```c\nint+n+=+1;\n```\nafter,chunk=1,delay=0.12")
    s = ctx.spawn()
    s.submit("stream C")
    s.wait_text("int n")
    assert cell(s, "int n").fg == TEXT
    assert cell(s, "int n").bg == CODE_BG
    s.wait_text("after")
    s.wait_turn_done()
    assert cell(s, "int n").fg == CYAN
    assert cell(s, "1").fg == YELLOW


def test_tilde_and_unterminated_fences_highlight(ctx):
    """Tilde fences and a final unterminated fence use the same close-time path."""
    ctx.scenario("text=~~~python\ndef+f():\n++++return+7\n~~~\n```json\n{\"ok\":true}")
    s = ctx.spawn()
    s.submit("show two")
    s.wait_turn_done()
    assert cell(s, "def f").fg == PURPLE
    assert cell(s, "return 7").fg == PURPLE
    assert cell(s, "7").fg == YELLOW
    assert cell(s, '"ok"').fg == GREEN


def test_unknown_fence_and_no_color_stay_plain(ctx):
    """Unavailable languages and NO_COLOR retain readable base styling."""
    ctx.scenario("text=```not-a-language\nkeyword+\"string\"\n```")
    s = ctx.spawn()
    s.submit("unknown")
    s.wait_turn_done()
    assert cell(s, 'keyword "string"').fg == TEXT
    assert cell(s, 'keyword "string"').bg == CODE_BG

    s2 = ctx.spawn(NO_COLOR="1")
    s2.submit("no color")
    s2.wait_turn_done()
    assert cell(s2, 'keyword "string"').fg is None


def test_read_result_uses_call_path(ctx):
    """A read result is highlighted from its call path without changing gutters."""
    ctx.write_file("sample.c", "// note\nint answer = 42;\n")
    ctx.scenario('tool=read:{"path":"sample.c"},final_text=done')
    s = ctx.spawn()
    s.submit("read it")
    s.wait_turn_done()
    assert cell(s, "// note").fg == MUTED
    assert cell(s, "int answer").fg == CYAN
    assert cell(s, "int answer", 4).fg == TEXT
    assert cell(s, "42").fg == YELLOW
    assert cell(s, "   // note").fg == MUTED


def test_raw_mode_disables_tool_syntax_highlighting(ctx):
    """The raw display switch governs tool syntax as well as Markdown."""
    ctx.write_file("sample.c", "int answer = 42;\n")
    ctx.scenario('tool=read:{"path":"sample.c"},final_text=done')
    s = ctx.spawn(YOKE_RAW_MARKDOWN="true")
    s.submit("read it")
    s.wait_turn_done()
    assert cell(s, "int answer").fg == TEXT
    assert cell(s, "42").fg == TEXT

    s.settings_toggle("Display raw")
    ctx.write_file("sample.c", "int restored = 7;\n")
    ctx.scenario('tool=read:{"path":"sample.c"},tool_rounds=2,final_text=again')
    s.submit("read it again")
    s.wait_turn_done()
    assert cell(s, "int restored").fg == CYAN
    assert cell(s, "7").fg == YELLOW


def test_patch_preview_highlights_code_but_not_diff_markers(ctx):
    """A patch path types its hunk while markers keep their diff colours."""
    ctx.write_file("sample.c", "int answer = 1;\n")
    diff = (
        "--- a/sample.c\n+++ b/sample.c\n@@ -1 +1 @@\n"
        "-int answer = 1;\n+int answer = 42;\n"
    )
    args = json.dumps({"patch": diff})
    ctx.scenario(f"tool=patch:{args},final_text=done")
    s = ctx.spawn()
    s.submit("patch it")
    s.wait_turn_done()
    assert cell(s, "-int answer").fg == 203
    assert cell(s, "+int answer").fg == GREEN
    assert cell(s, "-int answer", 1).fg == CYAN
    assert cell(s, "+int answer", 1).fg == CYAN
    assert cell(s, "+int answer", 5).fg == TEXT
    assert cell(s, "42").fg == YELLOW


def test_source_bearing_tool_calls_are_highlighted(ctx):
    """Write previews and shell commands use their reliable source context."""
    content = "def answer():\n    return 'yes'\n"
    args = json.dumps({"path": "answer.py", "content": content})
    ctx.scenario(f"tool=write:{args},final_text=written")
    s = ctx.spawn()
    s.submit("write it")
    s.wait_turn_done()
    assert cell(s, "def answer").fg == PURPLE
    assert cell(s, "return 'yes'").fg == PURPLE
    assert cell(s, "'yes'").fg == GREEN
    assert cell(s, "def answer", 4).fg == BLUE

    command = "if true; then echo 'yes'; fi"
    args = json.dumps({"command": command})
    ctx.scenario(f"tool=bash:{args},final_text=ran")
    s2 = ctx.spawn()
    s2.submit("run it")
    s2.wait_turn_done()
    assert cell(s2, "if true").fg == PURPLE
    assert cell(s2, "'yes'").fg == GREEN


def test_typed_grep_colours_only_match_text(ctx):
    """A single-extension grep colours fragments but leaves prefixes muted."""
    ctx.write_file("one.c", "int answer = 42;\n")
    args = json.dumps({"pattern": "answer", "glob": "*.c"})
    ctx.scenario(f"tool=grep:{args},final_text=done")
    s = ctx.spawn()
    s.submit("find it")
    s.wait_turn_done()
    assert cell(s, "one.c:1:").fg == MUTED
    assert cell(s, "int answer").fg == CYAN
    assert cell(s, "int answer", 4).fg == TEXT
    assert cell(s, "42").fg == YELLOW


def test_untyped_grep_and_shell_like_source_stay_plain(ctx):
    """Unreliable search context and arbitrary shell output are never guessed."""
    ctx.write_file("one.c", "int answer = 42;\n")
    ctx.scenario('tool=grep:{"pattern":"answer"},final_text=done')
    s = ctx.spawn()
    s.submit("find it")
    s.wait_turn_done()
    assert cell(s, "int answer").fg == MUTED

    args = json.dumps({"command": "printf 'int answer = 42;\\n'"})
    ctx.scenario(f"tool=bash:{args},final_text=done")
    s2 = ctx.spawn()
    s2.submit("run it")
    s2.wait_turn_done()
    assert cell(s2, "   int answer", 3).fg == MUTED


def test_missing_helper_is_a_silent_fallback(ctx):
    """A missing override preserves the transcript and emits no warning row."""
    ctx.scenario("text=```c\nint+n+=+1;\n```")
    s = ctx.spawn(YOKE_HIGHLIGHTER=ctx.work / "missing-helper")
    s.submit("show it")
    s.wait_turn_done()
    assert cell(s, "int n").fg == TEXT
    assert "highlight" not in s.text().lower(), s.text()


def test_malformed_and_hanging_helpers_fall_back_silently(ctx):
    """Invalid offsets and a timeout disable syntax without damaging the frame."""
    invalid = ctx.write_file(
        "invalid-helper",
        "#!/usr/bin/python3\n"
        "import os, struct\n"
        "h = os.read(0, 20)\n"
        "rid = struct.unpack_from('<I', h, 4)[0]\n"
        "hn, sn = struct.unpack_from('<II', h, 12)\n"
        "os.read(0, hn + sn)\n"
        "os.write(1, struct.pack('<4sIB3xI', b'YHL1', rid, 0, 1))\n"
        "os.write(1, struct.pack('<IIB3x', 0, sn + 1, 4))\n",
    )
    invalid.chmod(0o755)
    ctx.scenario("text=```c\nint+n+=+1;\n```")
    s = ctx.spawn(YOKE_HIGHLIGHTER=invalid)
    s.submit("invalid")
    s.wait_turn_done()
    assert cell(s, "int n").fg == TEXT
    assert "highlight" not in s.text().lower(), s.text()

    hanging = ctx.write_file(
        "hanging-helper",
        "#!/usr/bin/python3\nimport os, time\nos.read(0, 20)\ntime.sleep(10)\n",
    )
    hanging.chmod(0o755)
    before = time.monotonic()
    s2 = ctx.spawn(YOKE_HIGHLIGHTER=hanging)
    s2.submit("hanging")
    s2.wait_turn_done()
    assert time.monotonic() - before < 3
    assert cell(s2, "int n").fg == TEXT
    assert "highlight" not in s2.text().lower(), s2.text()
