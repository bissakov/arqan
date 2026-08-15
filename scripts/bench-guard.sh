#!/bin/sh
# Measure a reference commit and this working tree on one machine, then fail
# on a regression between them.
#
#   scripts/bench-guard.sh [ref] [extra bench arguments...]
#
# Arguments reach both runs, so they must be options the reference's own
# bench/run.py understands: a case filter, not a flag added since. Thresholds
# are set through TOLERANCE and MEM_TOLERANCE, which reach only the run being
# judged, so an old reference can serve as one.
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
regressed=$root/bench-regressed.txt

cleanup() {
    git worktree remove --force "$tree" >/dev/null 2>&1 || :
    rm -rf "$tree"
}
trap cleanup EXIT INT TERM
cleanup

jobs=$(command -v nproc >/dev/null 2>&1 && nproc || echo 2)

gate=""
if [ -n "${TOLERANCE:-}" ]; then gate="--tolerance $TOLERANCE"; fi
if [ -n "${MEM_TOLERANCE:-}" ]; then gate="$gate --mem-tolerance $MEM_TOLERANCE"; fi

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

measure_base() {  # report path, then bench arguments
    out=$1
    shift
    # The reference measures itself with its own bench/, so a case this change
    # adds is simply absent from the baseline rather than a phantom regression.
    ( cd "$tree" && python3 -m bench.run --no-budgets --json "$out" "$@" ) || :
}

measure_base "$base_json" "$@"

make -j"$jobs" all >/dev/null
flush "$root"
# $gate is deliberately unquoted: it carries zero, one or two option pairs.
rm -f "$regressed"
if python3 -m bench.run --json "$head_json" --baseline "$base_json" \
        --regressed "$regressed" $gate "$@"; then
    exit 0
fi

# A step that runs its operation once carries a fat tail: a single sample can
# clear any tolerance on its own. Measure the faulted cases again on both
# sides and report only what repeats. An empty list means the run failed on a
# budget, a stress check or a throw, which is not a matter of sampling.
[ -s "$regressed" ] || exit 1
cases=$(cat "$regressed")
printf '\nconfirming %s\n' "$cases"
measure_base "$root/bench-base-confirm.json" -k "$cases"
flush "$root"
python3 -m bench.run --json "$root/bench-head-confirm.json" \
    --baseline "$root/bench-base-confirm.json" $gate -k "$cases"
