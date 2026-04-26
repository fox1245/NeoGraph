#!/usr/bin/env bash
# Sweep mock bench across fan-out widths and LLM latencies.
# Each variant: warmup 5, iters 30 per side, alternating.
set -euo pipefail
cd "$(dirname "$0")"
PY=/tmp/dr-ui/bin/python

run() {
    local FANOUT=$1 LLM_MS=$2
    echo
    echo "########################################################################"
    echo "# FANOUT=$FANOUT  LLM_MOCK_MS=$LLM_MS"
    echo "########################################################################"
    LLM_MOCK_MS=$LLM_MS MOCK_SEARCH=1 USE_INMEMORY_CP=1 FANOUT=$FANOUT \
        $PY bench_mock.py --warmup 5 --iters 30 2>&1 | tail -10
}

# Pure engine — what's the floor?
run 5  0
run 20 0
run 50 0
# Realistic LLM latency — see how engine-overhead amortises
run 5  10
run 20 10
run 50 10
# Heavy LLM — engine overhead becomes invisible
run 5  100
run 20 100
