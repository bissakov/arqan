"""grep and find: the two read-only tools that locate code without a shell."""

import json


def run_tool(ctx, name, args, reply="found it"):
    """Drive one tool call and hand back what it fed the model."""
    ctx.scenario(f"tool={name}:{json.dumps(args)},final_text={reply.replace(' ', '+')}")
    s = ctx.spawn()
    s.submit(f"use {name}")
    s.wait_text(reply)
    s.wait_turn_done()
    return ctx.mock.tool_results()[-1]


def a_small_tree(ctx):
    ctx.write_file("src/one.c", "int alpha(void);\nint beta(void);\n")
    ctx.write_file("src/deep/two.c", "int alpha(int);\n")
    ctx.write_file("notes.txt", "alpha is a letter\n")
    ctx.write_file(".hidden/three.c", "int alpha(char);\n")


def test_grep_reports_path_line_and_text(ctx):
    """A match reads as path:line: text, and nothing else is sent."""
    a_small_tree(ctx)
    result = run_tool(ctx, "grep", {"pattern": "alpha"})
    assert "notes.txt:1: alpha is a letter" in result, result
    assert "src/one.c:1: int alpha(void);" in result, result
    assert "src/deep/two.c:1: int alpha(int);" in result, result
    assert "beta" not in result, result


def test_grep_is_ordered_the_same_way_every_run(ctx):
    """readdir order is the filesystem's, so the walk sorts what it lists."""
    a_small_tree(ctx)
    result = run_tool(ctx, "grep", {"pattern": "alpha"})
    paths = [line.split(":")[0] for line in result.splitlines() if line]
    assert paths == sorted(paths), paths


def test_grep_skips_hidden_directories(ctx):
    """.git alone would be most of a walk, and none of it is what was asked."""
    a_small_tree(ctx)
    result = run_tool(ctx, "grep", {"pattern": "alpha"})
    assert ".hidden" not in result, result


def test_grep_honours_the_glob_and_the_search_root(ctx):
    """A glob narrows by name, a path narrows by subtree."""
    a_small_tree(ctx)
    result = run_tool(ctx, "grep", {"pattern": "alpha", "glob": "*.txt"})
    assert result.startswith("notes.txt:1:"), result
    assert ".c:" not in result, result

    result = run_tool(ctx, "grep", {"pattern": "alpha", "path": "src/deep"})
    assert "src/deep/two.c:1:" in result, result
    assert "one.c" not in result, result


def test_grep_matches_case_sensitively_unless_told_otherwise(ctx):
    """The default is the literal string; ignore_case is asked for."""
    ctx.write_file("shout.txt", "ALPHA\n")
    assert run_tool(ctx, "grep", {"pattern": "alpha"}).startswith("no matches")
    hit = run_tool(ctx, "grep", {"pattern": "alpha", "ignore_case": True})
    assert "shout.txt:1: ALPHA" in hit, hit


def test_grep_caps_its_results_and_says_so(ctx):
    """A wide pattern costs a page, not the repository."""
    ctx.write_file("many.txt", "".join(f"hit {i}\n" for i in range(50)))
    result = run_tool(ctx, "grep", {"pattern": "hit", "limit": 5})
    assert len([l for l in result.splitlines() if l.startswith("many.txt")]) == 5, result
    assert "[5 of 50 matches shown; continue with offset=6]" in result, result


def test_grep_leaves_binary_files_alone(ctx):
    """A match inside a binary is a line of noise nobody can read."""
    (ctx.work / "blob.bin").write_bytes(b"alpha\x00\x01\x02alpha\n")
    ctx.write_file("plain.txt", "alpha\n")
    result = run_tool(ctx, "grep", {"pattern": "alpha"})
    assert "plain.txt:1:" in result, result
    assert "blob.bin" not in result, result


def test_grep_without_a_pattern_is_refused(ctx):
    """An empty search would return the whole tree; it is a mistake, not a query."""
    result = run_tool(ctx, "grep", {"path": "."}, reply="no pattern")
    assert result.startswith("ERROR:"), result
    assert "missing pattern" in result, result


def test_grep_names_a_root_that_does_not_exist(ctx):
    """A wrong root is answered, not walked into silence."""
    result = run_tool(ctx, "grep", {"pattern": "x", "path": "nope"}, reply="no such dir")
    assert "nope does not exist" in result, result


def test_find_lists_paths_matching_a_glob(ctx):
    """find answers with paths and nothing else."""
    a_small_tree(ctx)
    result = run_tool(ctx, "find", {"name": "*.c"})
    assert result.splitlines() == ["src/deep/two.c", "src/one.c"], result


def test_find_matches_the_whole_path_when_the_glob_has_a_slash(ctx):
    """'src/*.c' names a path; '*.c' names a file."""
    a_small_tree(ctx)
    result = run_tool(ctx, "find", {"name": "src/*.c"})
    assert result.splitlines() == ["src/one.c"], result


def test_find_says_when_nothing_matches(ctx):
    """An empty answer is still an answer."""
    a_small_tree(ctx)
    assert run_tool(ctx, "find", {"name": "*.rs"}).startswith("no files")


def test_search_tools_are_offered_in_plan_mode(ctx):
    """Plan mode reads the project, so it keeps every read-only tool."""
    ctx.scenario("text=planning")
    s = ctx.spawn()
    s.key("shift-tab")
    s.wait_for(lambda t: s.status_field(2) == "plan", "plan mode")
    s.submit("what would you do?")
    s.wait_text("planning")
    s.wait_turn_done()
    names = sorted(t["function"]["name"] for t in ctx.mock.requests[-1]["tools"])
    assert names == [
        "ask_user", "find", "grep", "internet_search", "page_fetch",
        "read", "submit_plan",
    ], names


def test_a_search_reads_as_what_it_looked_for(ctx):
    """The header carries the query, and no JSON reaches the transcript."""
    a_small_tree(ctx)
    ctx.scenario(
        'tool=grep:{"pattern":"alpha","path":"src"},final_text=three+hits'
    )
    s = ctx.spawn()
    s.submit("find alpha")
    s.wait_text("three hits")
    s.wait_turn_done()

    text = s.text()
    assert "\u25c6  grep alpha" in text, text
    assert "{" not in text, text
    assert "src/one.c:1: int alpha(void);" in text, text


def test_grep_searches_a_path_naming_one_file(ctx):
    """Narrowing a search to the file it is about is a smaller root, not an error."""
    a_small_tree(ctx)
    result = run_tool(ctx, "grep", {"pattern": "alpha", "path": "src/one.c"})
    assert result.splitlines() == ["src/one.c:1: int alpha(void);"], result


def test_find_matches_a_path_naming_one_file(ctx):
    """find against a single file answers with it, or with nothing."""
    a_small_tree(ctx)
    assert run_tool(ctx, "find", {"name": "*.c", "path": "src/one.c"}) == "src/one.c\n"
    assert run_tool(ctx, "find", {"name": "*.rs", "path": "src/one.c"}).startswith(
        "no files"
    )


def test_grep_offset_pages_through_results(ctx):
    """offset skips matches so a wide search is read in pages, not whole."""
    ctx.write_file("many.txt", "".join(f"hit {i}\n" for i in range(30)))
    page1 = run_tool(ctx, "grep", {"pattern": "hit", "limit": 10})
    assert "hit 0" in page1 and "hit 9" in page1, page1
    assert "hit 10" not in page1, page1
    assert "[10 of 30 matches shown; continue with offset=11]" in page1, page1

    page2 = run_tool(ctx, "grep", {"pattern": "hit", "limit": 10, "offset": 11})
    assert "hit 10" in page2 and "hit 19" in page2, page2
    assert "[10 of 30 matches shown; continue with offset=21]" in page2, page2

    page3 = run_tool(ctx, "grep", {"pattern": "hit", "limit": 10, "offset": 21})
    assert "hit 20" in page3 and "hit 29" in page3, page3
    # last page: no "continue" since there is nothing more
    assert "continue with offset" not in page3, page3
    assert "[10 of 30 matches shown]" in page3, page3


def test_grep_offset_past_the_end_says_so(ctx):
    """An offset beyond the last match is answered, not left empty."""
    ctx.write_file("few.txt", "hit 0\nhit 1\nhit 2\n")
    result = run_tool(ctx, "grep", {"pattern": "hit", "offset": 10})
    assert "offset 10 is past the last" in result, result
    assert "3 matches" in result, result


def test_grep_limit_above_the_cap_is_refused(ctx):
    """limit is a hard cap: the default is the ceiling, not a floor."""
    ctx.write_file("many.txt", "".join(f"hit {i}\n" for i in range(200)))
    result = run_tool(ctx, "grep", {"pattern": "hit", "limit": 200},
                      reply="too wide")
    assert result.startswith("ERROR:"), result
    assert "limit must be a whole number in 1..100" in result, result


def test_find_caps_its_results_and_says_so(ctx):
    """A wide glob costs a page, not the whole tree."""
    for i in range(50):
        ctx.write_file(f"f{i:02d}.txt", "")
    result = run_tool(ctx, "find", {"name": "*.txt", "limit": 5})
    lines = [l for l in result.splitlines() if l.startswith("f")]
    assert len(lines) == 5, result
    assert "[5 of 50 files shown; continue with offset=6]" in result, result


def test_find_offset_pages_through_results(ctx):
    """offset skips paths so a wide list is read in pages, not whole."""
    for i in range(30):
        ctx.write_file(f"f{i:02d}.txt", "")
    page1 = run_tool(ctx, "find", {"name": "*.txt", "limit": 10})
    assert "[10 of 30 files shown; continue with offset=11]" in page1, page1

    page2 = run_tool(ctx, "find", {"name": "*.txt", "limit": 10, "offset": 11})
    assert "[10 of 30 files shown; continue with offset=21]" in page2, page2

    page3 = run_tool(ctx, "find", {"name": "*.txt", "limit": 10, "offset": 21})
    assert "continue with offset" not in page3, page3
    assert "[10 of 30 files shown]" in page3, page3


def test_find_limit_above_the_cap_is_refused(ctx):
    """limit is a hard cap for find too."""
    for i in range(10):
        ctx.write_file(f"f{i:02d}.txt", "")
    result = run_tool(ctx, "find", {"name": "*.txt", "limit": 300},
                      reply="too wide")
    assert result.startswith("ERROR:"), result
    assert "limit must be a whole number in 1..200" in result, result
