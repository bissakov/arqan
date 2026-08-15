#!/bin/sh
# Measure a reference commit and this working tree on one machine, then fail
# on a regression between them.
#
#   scripts/bench-guard.sh [ref] [extra bench arguments...]
#
# A committed baseline would describe the machine that recorded it, so the
# reference is built and measured here, moments before the tree it judges.
# The reference run is measured only: its budgets and stress assertions belong
# to its own commit, and a failure there must not be reported as this change's.
#
# The reference is built beside the repository and never in TMPDIR: a binary
# mapped from tmpfs counts its text as private dirty, which would report
# roughly a megabyte of phantom memory against whichever side sat there.
set -eu

ref=${1:-HEAD}
if [ $# -gt 0 ]; then shift; fi

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

sha=$(git rev-parse --verify "${ref}^{commit}")
tree=$root/.bench-guard
base_json=$root/bench-base.json
head_json=$root/bench-head.json

cleanup() {
    git worktree remove --force "$tree" >/dev/null 2>&1 || :
    rm -rf "$tree"
}
trap cleanup EXIT INT TERM
cleanup

jobs=$(command -v nproc >/dev/null 2>&1 && nproc || echo 2)

# smaps counts a file page as private dirty while the page cache still holds
# it dirty, so a binary measured moments after it was linked charges its own
# text to the process. bench/run.py flushes what it measures; this covers a
# reference commit from before it did.
flush() {
    sync "$1/bin/arqan" "$1/bin/arqan-highlight" 2>/dev/null || sync
}

printf 'reference %s (%s)\n' "$(git rev-parse --short "$sha")" "$ref"
git worktree add --detach "$tree" "$sha" >/dev/null
make -C "$tree" -j"$jobs" all >/dev/null
flush "$tree"

# The reference measures itself with its own bench/, so a case this change
# adds is simply absent from the baseline rather than a phantom regression.
( cd "$tree" && python3 -m bench.run --no-budgets --json "$base_json" "$@" ) || :

make -j"$jobs" all >/dev/null
flush "$root"
python3 -m bench.run --json "$head_json" --baseline "$base_json" "$@"
