"""Resident footprint: the bulk buffers must stay out of the cleared block.

The TUI's scrollback, composer, draft and per-row selection mirrors total
about 12 MiB of static storage. Each is read only up to a counter in
TuiState, so tui_start clears the counters and leaves the bytes alone, and
the pages behind them stay untouched until something is written there.
Moving one of those arrays back into TuiState would put all of it into the
startup memset and make it permanently resident, which is what this measures.
"""

from pathlib import Path

from tests.context import BIN


def unmeasurable() -> bool:
    """Whether this build or platform cannot answer the question.

    A sanitizer surrounds every global with a redzone and keeps a shadow map
    it writes as it goes, so it changes both the size of the storage under
    test and the pages behind it. The footprint it reports is its own.
    Fil-C answers differently for the same reason: its runtime carries
    capability metadata beside every object and a garbage collector that
    touches pages on its own schedule.
    """
    if not Path("/proc/self/smaps_rollup").exists():
        return True   # not Linux; there is nothing to read
    blob = BIN.read_bytes()
    return b"__asan_init" in blob or b"libpizlo" in blob


def private_dirty_kb(pid: int) -> int:
    """The pages this process alone has written: its true memory cost."""
    total = 0
    for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
        if line.startswith("Private_Dirty:"):
            total += int(line.split()[1])
    return total


def maps_libcurl(pid: int) -> bool:
    """Whether libcurl and its dependency tree are mapped into the process."""
    text = Path(f"/proc/{pid}/maps").read_text()
    return "libcurl.so" in text


def test_idle_footprint_excludes_bulk_buffers(ctx):
    """A session that wrote nothing has not paid for the scrollback."""
    if unmeasurable():
        return
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.settle()

    kb = private_dirty_kb(s.proc.pid)
    # The bulk buffers are 12 MiB. Clearing them would land here; the control
    # block plus libcurl's own allocations sit far below it.
    assert kb < 8192, f"idle private footprint {kb} KB: bulk buffers resident?"


def test_libcurl_is_absent_until_the_first_request(ctx):
    """The loader does no TLS or resolver work for a session that never asks.

    libcurl pulls in a dependency tree that costs more to map than the rest
    of startup does, so it is opened at the first request rather than named
    in the binary. Linking it again would paint the first frame behind that
    load, which is what this measures.
    """
    if not Path("/proc/self/maps").exists():
        return   # not Linux; there is nothing to read
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.settle()

    assert not maps_libcurl(s.proc.pid), "libcurl mapped before any request"

    s.submit("say something")
    s.wait_turn_done()
    s.settle()
    assert maps_libcurl(s.proc.pid), "the turn did not load libcurl"


def test_footprint_tracks_use_not_capacity(ctx):
    """A short turn grows the footprint by its own bytes, not the cap."""
    if unmeasurable():
        return
    ctx.scenario("words=200,chunk=8")
    s = ctx.spawn()
    s.settle()
    before = private_dirty_kb(s.proc.pid)

    s.submit("write something")
    s.wait_turn_done()
    s.settle()
    after = private_dirty_kb(s.proc.pid)

    # A couple of hundred words touches its own transcript pages and the rows
    # it painted, nowhere near the 8 MiB the scrollback could hold.
    assert after - before < 4096, f"one turn added {after - before} KB"
    assert after < 8192, f"private footprint {after} KB after one turn"
