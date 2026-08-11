"""Process cost, sampled from /proc, and statistics over repeated samples.

Every number here describes the `arqan` child, never the harness. The harness
spends most of a benchmark asleep waiting for a quiet screen, so wall time on
its own says almost nothing; on-CPU time is what a slow path shows up in.

CPU comes from /proc/pid/schedstat when the kernel offers it, because a
keystroke costs single-digit milliseconds and the 10ms tick behind
/proc/pid/stat cannot see one at all. Memory is Private_Dirty from
smaps_rollup: the pages this process alone has written, which is the only
figure that distinguishes a mapped 11 MiB buffer from a resident one.
"""

from __future__ import annotations

import os
import time
from dataclasses import dataclass, replace
from pathlib import Path

HZ = os.sysconf("SC_CLK_TCK")


def have_proc() -> bool:
    """Whether this kernel exposes everything the samplers below need."""
    return Path("/proc/self/smaps_rollup").exists()


def _read(path: str) -> str:
    try:
        return Path(path).read_text()
    except OSError:
        return ""


def cpu_seconds(pid: int) -> float:
    """On-CPU seconds, at nanosecond resolution where the kernel offers it."""
    text = _read(f"/proc/{pid}/schedstat")
    if text:
        try:
            return int(text.split()[0]) / 1e9
        except (IndexError, ValueError):
            pass
    text = _read(f"/proc/{pid}/stat")
    if not text:
        return 0.0
    try:
        # After the "comm) " split, field 0 is state; utime/stime are 11 and 12.
        fields = text.rsplit(") ", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / HZ
    except (IndexError, ValueError):
        return 0.0


def private_dirty_kb(pid: int) -> int:
    """Pages this process alone has written, in KB."""
    for line in _read(f"/proc/{pid}/smaps_rollup").splitlines():
        if line.startswith("Private_Dirty:"):
            try:
                return int(line.split()[1])
            except (IndexError, ValueError):
                return 0
    return 0


def _status(pid: int) -> dict[str, int]:
    out: dict[str, int] = {}
    for line in _read(f"/proc/{pid}/status").splitlines():
        key, _, rest = line.partition(":")
        parts = rest.split()
        if parts and parts[0].isdigit():
            out[key] = int(parts[0])
    return out


def _faults(pid: int) -> tuple[int, int]:
    text = _read(f"/proc/{pid}/stat")
    if not text:
        return (0, 0)
    try:
        fields = text.rsplit(") ", 1)[1].split()
        return (int(fields[7]), int(fields[9]))
    except (IndexError, ValueError):
        return (0, 0)


def _io_kb(pid: int) -> tuple[int, int]:
    """Bytes the process asked the kernel for, in KB; (0, 0) when unreadable."""
    read = write = 0
    for line in _read(f"/proc/{pid}/io").splitlines():
        key, _, value = line.partition(":")
        value = value.strip()
        if not value.isdigit():
            continue
        if key == "rchar":
            read = int(value) // 1024
        elif key == "wchar":
            write = int(value) // 1024
    return (read, write)


def open_fds(pid: int) -> int:
    try:
        return len(os.listdir(f"/proc/{pid}/fd"))
    except OSError:
        return 0


@dataclass(frozen=True)
class Counters:
    """One instant of a process's cost. Deltas come from `Probe.since`."""

    wall: float = 0.0
    cpu: float = 0.0
    priv_kb: int = 0
    rss_kb: int = 0
    peak_kb: int = 0
    minflt: int = 0
    majflt: int = 0
    read_kb: int = 0
    write_kb: int = 0
    threads: int = 0
    fds: int = 0


@dataclass(frozen=True)
class Delta:
    """What a measured window cost, plus the state the process ended in.

    Counters that only grow (cpu, faults, io) are differences; the ones that
    describe a level (private dirty, peak rss, threads) are the value after
    the window, since the difference of a high-water mark is meaningless.
    """

    wall: float = 0.0
    cpu: float = 0.0
    minflt: int = 0
    majflt: int = 0
    read_kb: int = 0
    write_kb: int = 0
    priv_kb: int = 0
    priv_growth_kb: int = 0
    rss_kb: int = 0
    peak_kb: int = 0
    threads: int = 0
    fds: int = 0

    @property
    def wall_ms(self) -> float:
        return self.wall * 1000.0

    @property
    def cpu_ms(self) -> float:
        return self.cpu * 1000.0

    @property
    def busy(self) -> float:
        """Fraction of the window the process was actually on a CPU."""
        return self.cpu / self.wall if self.wall > 0 else 0.0

    def per(self, units: int) -> "Delta":
        """The same window divided by `units` operations."""
        if units <= 1:
            return self
        return replace(self, wall=self.wall / units, cpu=self.cpu / units)


class Probe:
    """Samples one pid. Every reader tolerates the process having exited."""

    def __init__(self, pid: int):
        self.pid = pid

    def alive(self) -> bool:
        return Path(f"/proc/{self.pid}").exists()

    def read(self) -> Counters:
        status = _status(self.pid)
        minflt, majflt = _faults(self.pid)
        read_kb, write_kb = _io_kb(self.pid)
        return Counters(
            wall=time.perf_counter(),
            cpu=cpu_seconds(self.pid),
            priv_kb=private_dirty_kb(self.pid),
            rss_kb=status.get("VmRSS", 0),
            peak_kb=status.get("VmHWM", 0),
            minflt=minflt,
            majflt=majflt,
            read_kb=read_kb,
            write_kb=write_kb,
            threads=status.get("Threads", 0),
            fds=open_fds(self.pid),
        )

    def since(self, start: Counters) -> Delta:
        now = self.read()
        return Delta(
            wall=now.wall - start.wall,
            cpu=max(0.0, now.cpu - start.cpu),
            minflt=max(0, now.minflt - start.minflt),
            majflt=max(0, now.majflt - start.majflt),
            read_kb=max(0, now.read_kb - start.read_kb),
            write_kb=max(0, now.write_kb - start.write_kb),
            priv_kb=now.priv_kb,
            priv_growth_kb=now.priv_kb - start.priv_kb,
            rss_kb=now.rss_kb,
            peak_kb=now.peak_kb,
            threads=now.threads,
            fds=now.fds,
        )


class Stat:
    """Summary of repeated per-operation samples, in milliseconds.

    A mean hides the frame that stalled, so the worst sample and p95 are kept
    beside it: an editor is judged by its slowest keystroke.
    """

    def __init__(self, values, unit: str = "ms"):
        self.values = sorted(float(v) for v in values)
        self.unit = unit

    def __len__(self) -> int:
        return len(self.values)

    def _pick(self, q: float) -> float:
        if not self.values:
            return 0.0
        i = min(len(self.values) - 1, int(round(q * (len(self.values) - 1))))
        return self.values[i]

    @property
    def total(self) -> float:
        return sum(self.values)

    @property
    def mean(self) -> float:
        return self.total / len(self.values) if self.values else 0.0

    @property
    def p50(self) -> float:
        return self._pick(0.50)

    @property
    def p95(self) -> float:
        return self._pick(0.95)

    @property
    def worst(self) -> float:
        return self.values[-1] if self.values else 0.0

    def as_dict(self) -> dict:
        return {
            "n": len(self.values),
            "mean": round(self.mean, 3),
            "p50": round(self.p50, 3),
            "p95": round(self.p95, 3),
            "worst": round(self.worst, 3),
        }

    def __str__(self) -> str:
        return (
            f"n={len(self)} mean={self.mean:.2f}{self.unit} "
            f"p50={self.p50:.2f} p95={self.p95:.2f} worst={self.worst:.2f}"
        )
