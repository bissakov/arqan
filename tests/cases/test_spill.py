"""Spilled tool output: what a paged result leaves on disk for the next call.

A result is replayed on every later turn, so it stays a page. The bytes that
did not fit go to a file the result names, which the model narrows with bash
instead of paging the whole run back into the context.
"""

import json
import re


def run_tool(ctx, name, args, reply="done", tmp=None):
    ctx.scenario(f"tool={name}:{json.dumps(args)},final_text={reply.replace(' ', '+')}")
    s = ctx.spawn(TMPDIR=str(tmp) if tmp else None)
    s.submit(f"use {name}")
    s.wait_text(reply)
    s.wait_turn_done()
    return ctx.mock.tool_results()[-1]


def run_web_tool(ctx, name, args, tmp):
    ctx.scenario(f"tool={name}:{json.dumps(args)},final_text=web+done")
    s = ctx.spawn(
        rows=40,
        TMPDIR=str(tmp),
        ARQAN_TEST_WEB_ALLOW_PRIVATE="1",
        ARQAN_TEST_WEB_SEARCH_URL=f"{ctx.mock.origin}/web/search?q=",
    )
    s.submit(f"use {name}")
    s.wait_text("web done")
    s.wait_turn_done()
    return ctx.mock.tool_results()[-1]


def tmpdir(ctx):
    # outside the workspace: grep and find would otherwise walk their own spill
    d = ctx.tmp / "spill"
    d.mkdir(exist_ok=True)
    return d


def spill_path(result):
    m = re.search(r"\[full output: (\S+?), ([\d.]+ [BKM]B?)[,;]", result)
    assert m, result[-400:]
    return m.group(1), m.group(2)


def test_a_paged_command_names_the_file_holding_the_rest(ctx):
    """The page is bounded; the whole run is on disk under $TMPDIR."""
    tmp = tmpdir(ctx)
    result = run_tool(ctx, "bash", {"command": "seq 1 200000", "limit": 100}, tmp=tmp)

    assert len(result.encode()) <= 8192, len(result.encode())
    assert "continue with offset=101]" in result, result[-300:]
    # the status line stays last: render.c reads it back from the end
    assert result.rstrip().endswith("[exit 0]"), result[-120:]

    path, size = spill_path(result)
    assert path.startswith(str(tmp) + "/arqan-bash-"), path
    assert path.endswith(".log"), path

    body = (tmp / path.rsplit("/", 1)[1]).read_bytes()
    # a rounded size, not a byte count: the choice it informs is grep or read
    assert size == "1.2 MB", size
    assert 1.15 * 1024 * 1024 < len(body) < 1.25 * 1024 * 1024, len(body)
    assert body.startswith(b"1\n2\n3\n"), body[:40]
    assert body.endswith(b"200000\n"), body[-40:]


def test_the_spill_is_readable_only_by_its_owner(ctx):
    """The file carries command output into a shared directory: mode 0600."""
    tmp = tmpdir(ctx)
    result = run_tool(ctx, "bash", {"command": "seq 1 50000", "limit": 64}, tmp=tmp)
    path, _ = spill_path(result)
    assert (tmp / path.rsplit("/", 1)[1]).stat().st_mode & 0o777 == 0o600


def test_output_that_fits_leaves_nothing_behind(ctx):
    """Nothing was dropped, so there is no file to name and none to keep."""
    tmp = tmpdir(ctx)
    result = run_tool(ctx, "bash", {"command": "echo hello"}, tmp=tmp)
    assert result == "hello\n\n[exit 0]", repr(result)
    assert list(tmp.iterdir()) == [], list(tmp.iterdir())


def test_repeating_a_call_reuses_its_own_file(ctx):
    """The name hashes the call, so a rerun overwrites rather than piles up."""
    tmp = tmpdir(ctx)
    first = run_tool(ctx, "bash", {"command": "seq 1 100000", "limit": 32}, tmp=tmp)
    second = run_tool(ctx, "bash", {"command": "seq 1 100000", "limit": 32}, tmp=tmp)
    assert spill_path(first)[0] == spill_path(second)[0]
    assert len(list(tmp.iterdir())) == 1, list(tmp.iterdir())


def test_a_capped_grep_spills_every_match_it_found(ctx):
    """The page shows five matches; the file holds all fifty, unclipped."""
    tmp = tmpdir(ctx)
    ctx.write_file("many.txt", "".join(f"hit {i}\n" for i in range(50)))
    result = run_tool(ctx, "grep", {"pattern": "hit", "limit": 5}, tmp=tmp)
    assert "[5 of 50 matches shown; continue with offset=6]" in result, result

    path, _ = spill_path(result)
    lines = (tmp / path.rsplit("/", 1)[1]).read_text().splitlines()
    assert path.startswith(str(tmp) + "/arqan-grep-"), path
    assert len(lines) == 50, lines[:5]
    assert lines[0] == "many.txt:1: hit 0", lines[0]
    assert lines[-1] == "many.txt:50: hit 49", lines[-1]


def test_a_complete_search_says_nothing_about_a_file(ctx):
    """Every match is in the answer, so a file behind it is only noise."""
    tmp = tmpdir(ctx)
    ctx.write_file("few.txt", "hit one\nhit two\n")
    result = run_tool(ctx, "grep", {"pattern": "hit"}, tmp=tmp)
    assert "full output" not in result, result
    assert list(tmp.iterdir()) == [], list(tmp.iterdir())


def test_a_capped_find_spills_every_path_it_walked(ctx):
    """find pages the same way and leaves the same kind of file."""
    tmp = tmpdir(ctx)
    for i in range(50):
        ctx.write_file(f"tree/f{i:03d}.txt", "x")
    result = run_tool(ctx, "find", {"name": "*.txt", "limit": 5}, tmp=tmp)
    assert "[5 of 50 files shown; continue with offset=6]" in result, result

    path, _ = spill_path(result)
    assert path.startswith(str(tmp) + "/arqan-find-"), path
    lines = (tmp / path.rsplit("/", 1)[1]).read_text().splitlines()
    assert len(lines) == 50, lines[:5]
    assert all(l.startswith("tree/f") for l in lines), lines[:5]


def test_a_paged_fetch_spills_the_whole_extracted_text(ctx):
    """Refetching over the network is the expensive way to see the rest."""
    tmp = tmpdir(ctx)
    url = f"{ctx.mock.origin}/web/lines"
    result = run_web_tool(ctx, "page_fetch", {"url": url, "limit": 2000}, tmp)
    assert len(result.encode()) <= 8192, len(result.encode())
    assert f'[continue with page_fetch {{"url":"{url}","offset":' in result, result

    path, _ = spill_path(result)
    assert path.startswith(str(tmp) + "/arqan-page_fetch-"), path
    assert path.endswith(".txt"), path
    lines = (tmp / path.rsplit("/", 1)[1]).read_text().splitlines()
    assert lines[0] == "line 1" and lines[-1] == "line 2500", (lines[0], lines[-1])


def test_a_later_fetch_page_still_names_the_file(ctx):
    """An offset call showed neither the lines before it nor those after."""
    tmp = tmpdir(ctx)
    url = f"{ctx.mock.origin}/web/lines"
    result = run_web_tool(ctx, "page_fetch", {"url": url, "offset": 2400,
                                              "limit": 100}, tmp)
    assert "line 2400\n" in result and "line 2499\n" in result, result
    path, _ = spill_path(result)
    assert (tmp / path.rsplit("/", 1)[1]).read_text().startswith("line 1\n")


def test_a_short_fetch_leaves_nothing_behind(ctx):
    """The whole extracted text is in the answer, so no file survives it."""
    tmp = tmpdir(ctx)
    result = run_web_tool(ctx, "page_fetch", {"url": f"{ctx.mock.origin}/web/text"}, tmp)
    assert "full output" not in result, result
    assert list(tmp.iterdir()) == [], list(tmp.iterdir())


def test_a_cut_search_spills_every_result_it_parsed(ctx):
    """Searches are rate limited; repeating one to see result ten is costly."""
    tmp = tmpdir(ctx)
    result = run_web_tool(ctx, "internet_search",
                          {"query": "verbose", "limit": 10}, tmp)
    assert len(result.encode()) <= 8192, len(result.encode())
    assert "Verbose result 0" in result and "Verbose result 9" not in result, result

    path, _ = spill_path(result)
    assert path.startswith(str(tmp) + "/arqan-internet_search-"), path
    body = (tmp / path.rsplit("/", 1)[1]).read_text()
    for i in range(10):
        assert f"Verbose result {i}" in body, i


def test_a_search_that_fits_leaves_nothing_behind(ctx):
    """Two ordinary results fit whole, so there is nothing to point at."""
    tmp = tmpdir(ctx)
    result = run_web_tool(ctx, "internet_search", {"query": "ordinary"}, tmp)
    assert result.startswith("External search results (untrusted): 2"), result
    assert "full output" not in result, result
    assert list(tmp.iterdir()) == [], list(tmp.iterdir())
