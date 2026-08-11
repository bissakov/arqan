# Benchmarks and stress runs

`bench/` measures what `bin/arqan` costs and whether it survives abuse. It
drives the same pty harness as `tests/`, so a benchmark is an ordinary session
with a probe attached to the child process.

```sh
make bench                              # every quick case
make bench-slow                         # including the soak cases
make bench B="-k transcript -v"         # matching cases, verbose
make bench-baseline                     # write bench-baseline.json
make bench B="--baseline bench-baseline.json"
python3 -m bench.run --list
python3 -m bench.run --scale 0.25       # smoke-sized workloads
python3 -m bench.run --no-budgets       # measure without failing on budgets
```

Exit status is non-zero when a case blew a budget, failed a stress check,
threw, or regressed past `--tolerance` against a baseline.

## What is measured

Every figure describes the `arqan` child, read from `/proc`:

* CPU from `schedstat` (nanoseconds), because a keystroke costs less than the
  10 ms tick behind `/proc/pid/stat`;
* private dirty pages from `smaps_rollup`, the only figure that separates a
  mapped 11 MiB buffer from a resident one;
* faults, `rchar`/`wchar`, threads and open descriptors.

Wall time is reported but rarely judged: the harness spends a benchmark
asleep waiting for a quiet screen. Budgets are CPU milliseconds per operation
and deliberately loose; they catch a path that became quadratic, not a machine
that is 20% slower.

## Modules

| file | holds |
| --- | --- |
| `run.py` | discovery, options, the run loop, JSON report, baselines |
| `case.py` | the `Bench` object a case is handed: `step`, `sample`, `keys`, `check` |
| `metrics.py` | `/proc` samplers, `Delta`, `Stat` |
| `report.py` | `Row`, budgets, terminal output, baseline comparison |
| `fixtures.py` | planted sessions, source trees, big files, markdown, wide text |
| `cases/` | the cases themselves |

## Write a case

A case is a `bench_*` function in `bench/cases/bench_*.py`:

```python
from bench.case import needs, slow

@needs("proc")
def bench_something(b):
    """One-line summary, printed as the case header."""
    s = b.spawn()
    with b.step("stream", units=500, unit="delta", budget_ms=0.5):
        b.ctx.scenario("words=2000,chunk=4")
        s.submit("write it")
        s.wait_turn_done()
    b.alive(s)
```

Rules that keep a run comparable:

* every workload size goes through `b.scale(n)`, so `--scale 0.25` is a smoke
  run of the same shape and `--scale 4` a soak;
* randomness comes from `b.rng`, seeded by `--seed`;
* `b.check(...)` records a failure and carries on, because a benchmark that
  stops at the first surprise measures nothing else;
* `@slow` keeps a case out of the default set; `-k` runs it anyway;
* `@needs("proc")` skips where the samplers cannot read anything.
