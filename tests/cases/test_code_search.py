"""code_search: structural search through ast-grep, offered only when it is
installed. The cases that run the real program return early without it."""

import json
import os
import shutil
import stat

ON = {"ARQAN_TEST_CODE_SEARCH": "1"}

C_SOURCE = """\
#include <stdio.h>
#define MAX 4
#define TWICE(x) helper(x) + helper(x)

typedef struct Pair { int a, b; } Pair;
struct Node { int v; };
enum Color { RED, BLUE };
static int counter = 0;
int helper(int x);

static int square(int x) {
    return x * x;
}

int helper(int x) {
    /* helper is cited in a comment */
    printf("helper\\n");
    if (x <= 0) return 0;
    return square(x) + helper(x - 1);
}

int main(void) {
    int (*fp)(int) = helper;
    return helper(MAX) + fp(1) + TWICE(2);
}
"""


def have_ast_grep() -> bool:
    return shutil.which("ast-grep") is not None


def run(ctx, args: dict, **env) -> str:
    """One code_search call, and what the model got back for it."""
    ctx.scenario("tool=code_search:" + json.dumps(args, separators=(",", ":"))
                 + ",final_text=searched")
    s = ctx.spawn(**{**ON, **env})
    s.submit("search")
    s.wait_text("searched")
    s.wait_turn_done()
    s.close()
    return ctx.mock.tool_results()[-1]


def tools_offered(ctx, **env) -> set:
    ctx.scenario("final_text=done")
    s = ctx.spawn(**env)
    s.submit("hello")
    s.wait_turn_done()
    s.close()
    return {t["function"]["name"] for t in ctx.mock.requests[-1]["tools"]}


def test_the_tool_is_offered_only_with_ast_grep_on_path(ctx):
    """A tool the model cannot run is not offered at all."""
    empty = ctx.tmp / "empty-bin"
    empty.mkdir()
    assert "code_search" not in tools_offered(ctx, **ON, PATH=str(empty))
    if not have_ast_grep():
        return
    assert "code_search" in tools_offered(ctx, **ON)


def test_a_program_named_ast_grep_that_is_not_executable_is_skipped(ctx):
    bin_dir = ctx.tmp / "bin"
    bin_dir.mkdir()
    (bin_dir / "ast-grep").write_text("#!/bin/sh\n")
    assert "code_search" not in tools_offered(ctx, **ON, PATH=str(bin_dir))


def fake_ast_grep(ctx, script: str) -> str:
    """A stand-in ast-grep that prints what the script says; returns PATH."""
    bin_dir = ctx.tmp / "fake-bin"
    bin_dir.mkdir(exist_ok=True)
    exe = bin_dir / "ast-grep"
    exe.write_text("#!/bin/sh\n" + script)
    exe.chmod(exe.stat().st_mode | stat.S_IXUSR)
    return f"{bin_dir}:{os.environ.get('PATH', '/usr/bin:/bin')}"


def test_ast_grep_runs_with_no_shell_and_with_project_config_ignored(ctx):
    """The query reaches ast-grep as one argument, never through a shell,
    and the flags that keep a project's sgconfig.yml and native grammars
    out are always there. Landlock is off so the stand-in can log."""
    log = ctx.tmp / "argv.txt"
    path = fake_ast_grep(ctx, f'for a in "$@"; do printf "%s\\0" "$a"; done '
                              f'>> {log}; printf "\\0" >> {log}\n')
    ctx.write_file("a.c", "int x;\n")
    run(ctx, {"mode": "pattern", "lang": "c", "pattern": "f($A); $(touch pwned)",
              "path": "a.c"}, PATH=path, ARQAN_TEST_NO_LANDLOCK="1")
    scan, debug, _ = log.read_text().split("\0\0")
    argv = scan.split("\0")
    assert not (ctx.work / "pwned").exists()
    assert argv[:2] == ["scan", "-c"], argv
    assert argv[2] == "/dev/null", argv
    assert "--custom-languages" in argv and "ignore" in argv, argv
    assert argv[-2:] == ["--", "a.c"], argv
    rules = argv[argv.index("--inline-rules") + 1]
    assert "f($A); $(touch pwned)" in rules, rules
    assert debug.split("\0")[:5] == ["run", "-c", "/dev/null",
                                     "--custom-languages", "ignore"], debug


def test_a_failure_of_ast_grep_is_reported_with_its_message(ctx):
    path = fake_ast_grep(ctx, 'echo "Error: Cannot parse rule" >&2; exit 2\n')
    ctx.write_file("a.c", "int x;\n")
    out = run(ctx, {"mode": "outline", "path": "a.c"}, PATH=path)
    assert "exit code 2" in out, out
    assert "Cannot parse rule" in out, out


def test_ast_grep_help_and_decoration_are_left_out_of_an_error(ctx):
    """Only the lines that say what is wrong reach the model."""
    msg = ("Error: Cannot parse rule INLINE_RULES\n"
           "Help: The file is not a valid ast-grep rule. Please refer to doc.\n"
           "See also: https://ast-grep.github.io/guide/rule-config.html\n"
           "\n"
           "\u2716 Caused by\n"
           "\u2570\u25bb Rule contains invalid kind matcher.\n"
           "\u2570\u25bb Kind `nope` is invalid.\n")
    script = ctx.tmp / "msg.txt"
    script.write_text(msg)
    path = fake_ast_grep(ctx, f"cat {script} >&2; exit 8\n")
    ctx.write_file("a.c", "int x;\n")
    out = run(ctx, {"mode": "rule", "lang": "c", "rule": {"kind": "nope"}},
              PATH=path, ARQAN_TEST_NO_LANDLOCK="1")
    assert out == ("ERROR: ast-grep failed with exit code 8:\n"
                   "Rule contains invalid kind matcher.\n"
                   "Kind `nope` is invalid."), out


def test_a_search_stopped_for_a_user_message_says_so(ctx):
    path = fake_ast_grep(ctx, "sleep 5\n")
    ctx.write_file("a.c", "int x;\n")
    ctx.scenario('tool=code_search:{"mode":"outline","path":"a.c"},'
                 'final_text=never')
    s = ctx.spawn(**ON, PATH=path)
    s.submit("search")
    s.wait_activity("running code_search")
    ctx.scenario("text=heard+you,final_text=heard+you")
    s.submit("something else")
    s.wait_text("heard you")
    s.wait_turn_done()
    out = ctx.mock.tool_results()[0]
    assert out == "ERROR: code_search stopped for a user message", out


def test_a_json_line_too_long_to_hold_is_counted_not_parsed(ctx):
    """A match bigger than the line buffer is dropped and named, and the
    matches after it still count."""
    good = json.dumps({"file": "a.c", "ruleId": "match", "text": "x",
                       "lines": "int x;",
                       "range": {"byteOffset": {"start": 0, "end": 1},
                                 "start": {"line": 0, "column": 0},
                                 "end": {"line": 0, "column": 1}}})
    path = fake_ast_grep(
        ctx, "head -c 5000000 /dev/zero | tr '\\0' a; echo\n"
             f"printf '%s\\n' '{good}'\n")
    ctx.write_file("a.c", "int x;\n")
    out = run(ctx, {"mode": "pattern", "lang": "c", "pattern": "x",
                    "path": "a.c"}, PATH=path)
    assert out.startswith("1 match in 1 file\n"), out
    assert ("[1 match not listed: its code is too large; search with a "
            "narrower pattern]") in out, out


def test_bad_arguments_are_refused_before_ast_grep_runs(ctx):
    log = ctx.tmp / "ran"
    path = fake_ast_grep(ctx, f"touch {log}\n")
    ctx.write_file("a.c", "int x;\n")
    cases = [
        ({"mode": "grep"}, "mode must be"),
        ({"mode": "callers"}, "needs name"),
        ({"mode": "definition", "name": "a b"}, "needs name"),
        ({"mode": "pattern", "pattern": "x"}, "needs lang"),
        ({"mode": "pattern", "lang": "c c", "pattern": "x"}, "lang must be"),
        ({"mode": "outline", "lang": "java"},
         "outline, definition and callers know"),
        ({"mode": "rule", "lang": "c", "rule": "kind: x"}, "rule object"),
        ({"mode": "outline", "path": "missing.c"}, "does not exist"),
        ({"mode": "outline", "path": "a\u0000.c"}, "path holds a NUL byte"),
        ({"mode": "outline", "limit": 0}, "limit must be"),
    ]
    for args, want in cases:
        out = run(ctx, args, PATH=path)
        assert want in out, (args, out)
    assert not log.exists()


# ---- with the real ast-grep ----------------------------------------------


def test_outline_lists_definitions_with_line_ranges(ctx):
    if not have_ast_grep():
        return
    ctx.write_file("src/a.c", C_SOURCE)
    out = run(ctx, {"mode": "outline", "path": "src"})
    assert out.startswith("11 definitions in 1 file\n"), out
    for line in ("  2 macro MAX", "  3 macro TWICE", "  5 typedef Pair",
                 "  5 struct Pair", "  6 struct Node", "  7 enum Color",
                 "  8 var counter", "  9 proto helper", "  11-13 fn square",
                 "  15-20 fn helper", "  22-25 fn main"):
        assert line + "\n" in out, (line, out)


def test_definition_returns_the_source_of_each_definition(ctx):
    if not have_ast_grep():
        return
    ctx.write_file("a.c", C_SOURCE)
    out = run(ctx, {"mode": "definition", "name": "helper"})
    assert out.startswith("2 definitions of helper\n"), out
    assert "a.c:9 proto helper\nint helper(int x);\n" in out, out
    assert "a.c:15-20 fn helper\nint helper(int x) {\n" in out, out
    assert "return square(x) + helper(x - 1);\n}" in out, out


def test_callers_name_the_function_and_list_every_other_mention(ctx):
    """A call in a macro body and a use as a value are not calls to
    tree-sitter, so they show as other mentions instead of vanishing."""
    if not have_ast_grep():
        return
    ctx.write_file("a.c", C_SOURCE)
    out = run(ctx, {"mode": "callers", "name": "helper", "lang": "c"})
    assert out.startswith("2 calls to helper and 6 other mentions in 1 file\n"
                          "a.c\n"), out
    body = out.split("\n", 1)[1]
    assert body == (
        "a.c\n"
        "  3 other: #define TWICE(x) helper(x) + helper(x)\n"
        "  9 other: int helper(int x);\n"
        "  15 other: int helper(int x) {\n"
        "  16 other: /* helper is cited in a comment */\n"
        "  17 other: printf(\"helper\\n\");\n"
        "  19 call in helper: return square(x) + helper(x - 1);\n"
        "  23 other: int (*fp)(int) = helper;\n"
        "  24 call in main: return helper(MAX) + fp(1) + TWICE(2);\n"), body


def test_a_c_call_pattern_with_one_argument_still_matches(ctx):
    """On its own `square($A)` parses as a macro type and matches nothing;
    the tool parses it as a call."""
    if not have_ast_grep():
        return
    ctx.write_file("a.c", C_SOURCE)
    out = run(ctx, {"mode": "pattern", "lang": "c", "pattern": "square($A)"})
    assert out.startswith("1 match in 1 file\n"), out
    assert "  19: return square(x) + helper(x - 1);" in out, out


def test_a_pattern_that_does_not_parse_shows_how_it_parsed(ctx):
    if not have_ast_grep():
        return
    ctx.write_file("a.c", C_SOURCE)
    out = run(ctx, {"mode": "pattern", "lang": "c", "pattern": "x +* y"})
    assert out.startswith("0 matches\n"
                          "The pattern does not parse cleanly as c:\n"
                          "translation_unit"), out
    assert "ERROR" in out, out
    assert "Debug AST" not in out, out


def test_a_pattern_ast_grep_refuses_shows_how_it_parsed(ctx):
    """ast-grep answers `foo(` with an error about rules needing a kind;
    the parse tree says what is wrong with the pattern."""
    if not have_ast_grep():
        return
    ctx.write_file("a.c", C_SOURCE)
    out = run(ctx, {"mode": "pattern", "lang": "c", "pattern": "foo("})
    assert out.startswith("ERROR: The pattern does not parse cleanly as c:\n"
                          "translation_unit"), out
    assert "ERROR (" in out, out
    assert "kind" not in out, out


def test_a_pattern_that_parses_but_matches_nothing_shows_no_tree(ctx):
    """The tool searches a one-argument C call as a call, so the tree of
    the bare pattern, a macro type, would mislead."""
    if not have_ast_grep():
        return
    ctx.write_file("a.c", C_SOURCE)
    out = run(ctx, {"mode": "pattern", "lang": "c",
                    "pattern": "nonexistent_fn($A)"})
    assert out == "0 matches\n", out


def test_a_rule_object_passes_through(ctx):
    if not have_ast_grep():
        return
    ctx.write_file("a.c", C_SOURCE)
    out = run(ctx, {"mode": "rule", "lang": "c", "rule": {
        "kind": "return_statement",
        "has": {"stopBy": "end", "kind": "call_expression",
                "has": {"field": "function", "regex": "^square$"}}}})
    assert out.startswith("1 match in 1 file\n"), out
    assert "  19: return square(x)" in out, out


def test_a_project_sgconfig_does_not_change_what_is_parsed(ctx):
    """An sgconfig.yml in the project could map other files to a language
    or load grammars; the tool reads none of it."""
    if not have_ast_grep():
        return
    ctx.write_file("sgconfig.yml", 'ruleDirs: []\nlanguageGlobs:\n  c: ["*.txt"]\n')
    ctx.write_file("notes.txt", "int f(void) { return square(1); }\n")
    out = run(ctx, {"mode": "pattern", "lang": "c", "pattern": "square($A)"})
    assert out.startswith("0 matches"), out


def test_a_long_result_pages_and_says_how_to_continue(ctx):
    if not have_ast_grep():
        return
    ctx.write_file("a.c", "".join(f"int f{i}(void) {{ return {i}; }}\n"
                                  for i in range(30)))
    first = {"mode": "outline", "path": "a.c", "limit": 10}
    second = {"mode": "outline", "path": "a.c", "offset": 11, "limit": 10}
    ctx.scenario("tool=code_search:" + json.dumps(first) + ",tool=code_search:"
                 + json.dumps(second) + ",final_text=searched")
    s = ctx.spawn(**ON)
    s.submit("search")
    s.wait_text("searched")
    s.wait_turn_done()
    s.close()
    out, out2 = ctx.mock.tool_results()[-2:]
    assert out.startswith("30 definitions in 1 file\na.c\n"), out
    assert "  1 fn f0\n" in out and "  10 fn f9\n" in out, out
    assert "  11 fn f10" not in out, out
    assert ('[continue with code_search {"mode":"outline","path":"a.c",'
            '"offset":11,"limit":10}]') in out, out
    assert "[full output: " in out, out
    spill = out.split("[full output: ")[1].split(",")[0]
    assert out2.split("\n")[1:3] == ["a.c", "  11 fn f10"], out2
    assert f"[full output: {spill}," in out2, "one spill file serves every page"


def test_a_page_that_starts_inside_a_file_names_the_file(ctx):
    if not have_ast_grep():
        return
    for name in ("a.c", "b.c"):
        ctx.write_file(name, "".join(f"int {name[0]}{i}(void) {{ return 0; }}\n"
                                     for i in range(5)))
    out = run(ctx, {"mode": "outline", "offset": 7, "limit": 2})
    assert out.split("\n")[:4] == ["10 definitions in 2 files", "b.c",
                                   "  2 fn b1", "  3 fn b2"], out


def test_two_calls_on_one_line_show_one_row_with_their_count(ctx):
    if not have_ast_grep():
        return
    ctx.write_file("a.c", "int f(int);\n"
                          "int g(void) { return f(1) + f(2); }\n")
    out = run(ctx, {"mode": "callers", "name": "f"})
    assert out == ("2 calls to f and 1 other mention in 1 file\n"
                   "a.c\n"
                   "  1 other: int f(int);\n"
                   "  2 call in g (2 calls): int g(void) { return f(1) + f(2); }"
                   "\n"), out
    out = run(ctx, {"mode": "pattern", "lang": "c", "pattern": "f($A)"})
    assert out == ("2 matches in 1 file\n"
                   "a.c\n"
                   "  2 (2 matches): int g(void) { return f(1) + f(2); }\n"), out


def test_a_macro_definition_ends_on_its_own_line(ctx):
    """tree-sitter ends a #define at column 0 of the next line, so ast-grep
    sends that line too."""
    if not have_ast_grep():
        return
    ctx.write_file("a.c", C_SOURCE)
    out = run(ctx, {"mode": "definition", "name": "MAX"})
    assert out == ("1 definition of MAX\n"
                   "a.c:2 macro MAX\n"
                   "#define MAX 4\n"), out


def test_an_empty_result_names_a_missing_language(ctx):
    if not have_ast_grep():
        return
    ctx.write_file("notes.md", "# helper\n")
    ctx.write_file("a.py", "def helper(): pass\n")
    out = run(ctx, {"mode": "outline", "path": "notes.md"})
    assert out == ("0 definitions\n"
                   "[no c, cpp, csharp, python, rust, go, javascript, "
                   "typescript, tsx file at notes.md; pattern and rule take "
                   "other languages]\n"), out
    out = run(ctx, {"mode": "definition", "name": "helper", "lang": "c"})
    assert out == "0 definitions of helper\n[no c file at .]\n", out
    out = run(ctx, {"mode": "callers", "name": "absent", "lang": "py"})
    assert out == "0 calls to absent and 0 other mentions\n", out


def test_mentions_are_found_in_a_file_too_big_for_one_json_line(ctx):
    """ast-grep would send the whole file through one JSON line; the tool
    reads the file itself instead."""
    if not have_ast_grep():
        return
    pad = "// padding line for a large source file\n" * 150_000
    ctx.write_file("big.c", pad + "int g(void) { return helper(1); }\n"
                   "/* helper */\n")
    out = run(ctx, {"mode": "callers", "name": "helper", "path": "big.c"})
    assert out.startswith("1 call to helper and 1 other mention in 1 file\n"),\
        out
    assert "  150002 other: /* helper */" in out, out
    assert "not listed" not in out, out


def test_a_file_ast_grep_skips_is_named_and_read_for_mentions(ctx):
    """ast-grep skips a file over 3 MB and 200000 lines without a word."""
    if not have_ast_grep():
        return
    pad = "// padding line for a source file over sixteen megabytes\n"
    ctx.write_file("huge.c", pad * 300_000
                   + "int g(void) { return helper(1); }\n")
    ctx.write_file("a.c", "int helper(int x) { return x; }\n")
    out = run(ctx, {"mode": "callers", "name": "helper", "path": "huge.c"})
    assert out.startswith(
        "0 calls to helper and 1 other mention in 1 file\n"
        "[huge.c is over 3 MB and 200000 lines, too large for ast-grep; its "
        "calls are listed as other]\n"
        "huge.c\n"
        "  300001 other: int g(void) { return helper(1); }\n"), out
    out = run(ctx, {"mode": "outline"})
    assert out.startswith("1 definition in 1 file\n"
                          "[not searched: huge.c is over 3 MB and 200000 "
                          "lines]\n"), out


def test_a_file_that_is_not_utf8_is_counted(ctx):
    if not have_ast_grep():
        return
    ctx.write_file("a.c", "int f(void) { return 0; }\n")
    (ctx.work / "b.c").write_bytes(b"/* caf\xe9 */ int g(void) { return 0; }\n")
    (ctx.work / "empty.c").write_bytes(b"")
    out = run(ctx, {"mode": "outline"})
    assert out.startswith("1 definition in 1 file\n"
                          "[not searched: 1 file that is not UTF-8]\n"), out


def test_a_definition_too_big_for_one_json_line_is_still_listed(ctx):
    """Its source is over 4 MB as JSON, so the range and name are taken
    from either side of the big strings."""
    if not have_ast_grep():
        return
    ctx.write_file("a.c", "int big(void) {\n" + "    x = 1;\n" * 250_000
                   + "}\nint small(void) { return 0; }\n")
    out = run(ctx, {"mode": "outline"})
    assert out == ("2 definitions in 1 file\n"
                   "a.c\n"
                   "  1-250002 fn big\n"
                   "  250003 fn small\n"), out
    out = run(ctx, {"mode": "definition", "name": "big"})
    assert out == ("1 definition of big\n"
                   "a.c:1-250002 fn big\n"
                   "[too long to show; read these lines]\n"), out


def test_a_definition_with_a_line_too_long_for_a_page_still_advances(ctx):
    if not have_ast_grep():
        return
    long_line = "    int t[] = {" + ", ".join(["1"] * 4000) + "};\n"
    ctx.write_file("a.c", "int f(void) {\n" + long_line + "    return 0;\n}\n")
    out = run(ctx, {"mode": "definition", "name": "f"})
    assert '"offset":3' in out, out
    out = run(ctx, {"mode": "definition", "name": "f", "offset": 3})
    assert "int t[] = {1, 1, 1" in out, out
    assert " [line clipped]\n    return 0;\n}\n" in out, out
    assert "[full output: " in out, out


def test_an_empty_result_adds_no_note(ctx):
    if not have_ast_grep():
        return
    ctx.write_file("a.c", C_SOURCE)
    assert run(ctx, {"mode": "definition", "name": "absent"}) == \
        "0 definitions of absent\n"


CS_SOURCE = """\
namespace App.Core {
public interface IShape { double Area(); }
public record Point(int X, int Y);
public class Circle : IShape {
    public double Radius { get; set; }
    public Circle(double r) { Radius = r; }
    public double Area() { return Helper(Radius) * 3.14; }
    static double Helper(double x) { return x * x; }
}
static class Program {
    static void Main() { var c = new Circle(2); c.Area(); }
}
}
"""


def test_csharp_has_outline_definition_and_callers(ctx):
    if not have_ast_grep():
        return
    ctx.write_file("a.cs", CS_SOURCE)
    out = run(ctx, {"mode": "outline", "path": "a.cs"})
    for line in ("  1-13 namespace App.Core", "  2 interface IShape",
                 "  3 record Point", "  4-9 class Circle",
                 "  5 property Radius", "  6 ctor Circle", "  7 method Area",
                 "  8 method Helper", "  10-12 class Program",
                 "  11 method Main"):
        assert line + "\n" in out, (line, out)
    out = run(ctx, {"mode": "definition", "name": "Helper", "lang": "cs"})
    assert "a.cs:8 method Helper\n" in out, out
    out = run(ctx, {"mode": "callers", "name": "Circle", "lang": "csharp"})
    assert "  11 call in Main: static void Main()" in out, out
    out = run(ctx, {"mode": "callers", "name": "Area"})
    assert "  11 call in Main" in out, out


def test_pattern_takes_any_language_ast_grep_knows(ctx):
    if not have_ast_grep():
        return
    ctx.write_file("A.java", "class A { void m() { foo(1); } }\n")
    ctx.write_file("a.py", "foo(1)\n")
    out = run(ctx, {"mode": "pattern", "lang": "java", "pattern": "foo($X)"})
    assert out.startswith("1 match in 1 file\nA.java\n"), out
    out = run(ctx, {"mode": "pattern", "lang": "Py", "pattern": "foo($X)"})
    assert out.startswith("1 match in 1 file\na.py\n"), out


def test_outside_the_project_asks_first(ctx):
    if not have_ast_grep():
        return
    outside = ctx.tmp / "elsewhere"
    outside.mkdir()
    (outside / "b.c").write_text("int g(void) { return 1; }\n")
    ctx.scenario("tool=code_search:" + json.dumps(
        {"mode": "outline", "path": str(outside)}) + ",final_text=searched")
    s = ctx.spawn(**ON, ARQAN_PERMISSIONS="ask")
    s.submit("search")
    s.wait_status("allow read outside the project?")
    s.close()


def test_the_transcript_names_the_mode_the_name_and_the_path(ctx):
    if not have_ast_grep():
        return
    ctx.write_file("src/a.c", C_SOURCE)
    ctx.scenario('tool=code_search:{"mode":"callers","name":"square",'
                 '"path":"src"},final_text=searched')
    s = ctx.spawn(**ON)
    s.submit("search")
    s.wait_text("searched")
    s.wait_turn_done()
    text = s.text()
    assert "\u25c6  code_search callers square in src" in text, text
    assert "1 call to square and 1 other mention in 1 file" in text, text


def test_a_call_without_a_path_shows_no_json(ctx):
    if not have_ast_grep():
        return
    ctx.write_file("a.c", C_SOURCE)
    ctx.scenario('tool=code_search:{"mode":"outline"},final_text=searched')
    s = ctx.spawn(**ON)
    s.submit("search")
    s.wait_text("searched")
    s.wait_turn_done()
    text = s.text()
    assert "\u25c6  code_search outline" in text, text
    assert '{"mode"' not in text, text


def test_the_prompt_suggests_code_search_only_when_offered(ctx):
    def prompt(**env):
        ctx.scenario("final_text=done")
        s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None, **env)
        s.submit("hello")
        s.wait_turn_done()
        s.close()
        return ctx.mock.requests[-1]["messages"][0]["content"]

    assert "code_search" not in prompt()
    if not have_ast_grep():
        return
    assert "prefer code_search to reading whole files" in prompt(**ON)
