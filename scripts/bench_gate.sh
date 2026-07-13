#!/usr/bin/env bash
#
# Performance gate — usable standalone or as `git bisect run scripts/bench_gate.sh`.
#
# Builds the CURRENT commit in a throwaway worktree, runs bench_neograph under
# CPU pinning + FIFO scheduling, and compares the median seq per-iter time
# against a baseline.
#
#   exit 0   — within threshold (commit is "good")
#   exit 1   — regression beyond threshold (commit is "bad")
#   exit 125 — build failed (tell git bisect to skip this commit)
#
# Two rules this script exists to enforce, both learned the hard way:
#
#   1. Never bench in a reused build tree. An accumulated build/ has made the
#      same source measure ~8% slower — a fake regression signal that cost a
#      whole bisect. Every run gets a fresh worktree + fresh configure.
#
#   2. Never bench unpinned on WSL2. Host scheduler interrupts inject ~10%
#      jitter, and that noise is not Gaussian, so "more reps + median" does not
#      wash it out. taskset + chrt FIFO brings stddev to ~3%, which is what
#      makes microsecond-scale regressions detectable at all.
#
# Usage:
#   BASELINE_US=5.185 scripts/bench_gate.sh
#   BASELINE_US=5.185 THRESHOLD_PCT=5 REPS=5 git bisect run scripts/bench_gate.sh
#
set -uo pipefail

BASELINE_US="${BASELINE_US:?set BASELINE_US to the known-good seq µs/iter}"
THRESHOLD_PCT="${THRESHOLD_PCT:-5}"
REPS="${REPS:-9}"      # 9 reps: median was stable to +-1% across batches (4.8/4.8/4.9)
WARMUP="${WARMUP:-3}"  # discarded — see below
CPU="${BENCH_CPU:-3}"

repo_root=$(git rev-parse --show-toplevel)
rev=$(git rev-parse --short HEAD)
wt="/tmp/ng-gate-${rev}"

cleanup() { git -C "$repo_root" worktree remove --force "$wt" >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "── bench gate @ ${rev} (baseline ${BASELINE_US} µs, threshold +${THRESHOLD_PCT}%)"

cleanup
git -C "$repo_root" worktree add --detach "$wt" HEAD >/dev/null 2>&1 || exit 125

# NEOGRAPH_BUILD_BENCHMARKS defaults to OFF — without it the target does not
# exist and every commit would silently exit 125, i.e. bisect would skip the
# entire range and report nothing. Examples are off: they dominate build time
# and the bench does not link them.
cmake -S "$wt" -B "$wt/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DNEOGRAPH_BUILD_BENCHMARKS=ON \
    -DNEOGRAPH_BUILD_EXAMPLES=OFF >/dev/null 2>&1 || exit 125
cmake --build "$wt/build" --target bench_neograph -j"$(nproc)" >/dev/null 2>&1 || exit 125

bench=$(find "$wt/build" -name bench_neograph -type f -perm -u+x | head -1)
[[ -n "$bench" ]] || exit 125

# Refuse to measure on a busy box, and note that the build above just made it
# busy: a -j$(nproc) compile leaves the 1-minute load average elevated for a
# while after it exits, so a gate that benches immediately is measuring its own
# build. Observed: a bench run at load 8.5 (8 cores) reported a 25.8% regression
# on a commit that does not touch the benchmarked path at all, with samples
# spread 5.2-8.3 us against a quiet-box spread of 4.8-5.0.
#
# The false positive is the harmless direction — a human investigates and finds
# nothing. The same defect runs the other way: baseline taken while loaded and
# candidate taken while quiet lets a real regression through. And `git bisect
# run` calls this script dozens of times unattended, so a stray build landing
# mid-run pins the blame on an innocent commit.
#
# A missing number is better than a wrong one: if the box will not settle, skip.
wait_for_quiet() {
    local limit="${LOAD_LIMIT:-1.5}" waited=0
    while (( waited < ${LOAD_WAIT_MAX:-300} )); do
        local load
        load=$(cut -d' ' -f1 /proc/loadavg)
        if awk -v l="$load" -v m="$limit" 'BEGIN{exit !(l <= m)}'; then
            [[ $waited -gt 0 ]] && echo "   load settled to ${load} after ${waited}s"
            return 0
        fi
        sleep 10
        waited=$((waited + 10))
    done
    echo "   load still $(cut -d' ' -f1 /proc/loadavg) after ${waited}s — refusing to measure"
    return 1
}

wait_for_quiet || exit 125

# Warm-up runs are DISCARDED, not averaged in. Measured on this box: the first
# few runs after a build land ~10% high (median 5.3 µs) while steady state is
# 4.8-4.9 µs. Folding those into the baseline inflates it, and an inflated
# baseline is worse than no gate at all — it silently absorbs a real regression
# and still reports "ok".
#
# bench_neograph prints TSV: "seq\t<iters>\t<total_ms>\t<per_iter_us>"
run_once() {
    taskset -c "$CPU" chrt -f 99 "$bench" 2>/dev/null | awk -F'\t' '$1=="seq"{print $4}'
}

for _ in $(seq "$WARMUP"); do run_once >/dev/null; done

samples=()
for _ in $(seq "$REPS"); do
    v=$(run_once)
    [[ -n "$v" ]] && samples+=("$v")
done

if (( ${#samples[@]} == 0 )); then
    echo "   no samples parsed — treating as skip"
    exit 125
fi

median=$(printf '%s\n' "${samples[@]}" | sort -n | awk '{a[NR]=$1} END {print (NR%2) ? a[(NR+1)/2] : (a[NR/2]+a[NR/2+1])/2}')
limit=$(awk -v b="$BASELINE_US" -v t="$THRESHOLD_PCT" 'BEGIN{printf "%.4f", b*(1+t/100)}')
delta=$(awk -v m="$median" -v b="$BASELINE_US" 'BEGIN{printf "%+.1f", (m-b)/b*100}')

echo "   samples: ${samples[*]}"
echo "   median ${median} µs  vs limit ${limit} µs  (${delta}%)"

if awk -v m="$median" -v l="$limit" 'BEGIN{exit !(m > l)}'; then
    echo "   REGRESSION"
    exit 1
fi
echo "   ok"
exit 0
