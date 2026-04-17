#!/usr/bin/env bash
# Sweep the concurrent-bench matrix and append JSON lines to results.jsonl.
#
# Usage (from repo root):
#   bash benchmarks/concurrent/run_matrix.sh [output_file]
#
# Produces results.jsonl with one JSON object per (profile, concurrency,
# engine) cell. Container exits with OOM are captured with an explicit
# error line so the chart can show where each engine dies.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

OUT="${1:-benchmarks/concurrent/results.jsonl}"
mkdir -p "$(dirname "$OUT")"
: > "$OUT"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }

# ── Build images (cached) ─────────────────────────────────────────────
log "Building NeoGraph image..."
docker build -q -t ng-concurrent -f benchmarks/concurrent/Dockerfile.neograph . >&2

log "Building LangGraph image..."
docker build -q -t lg-concurrent -f benchmarks/concurrent/Dockerfile.langgraph . >&2

# ── Matrix ────────────────────────────────────────────────────────────
# profile = "cpus:mem"
PROFILES=("2:1g" "1:512m")
CONCURRENCIES=(10 100 1000 5000 10000)

run_cell() {
    local profile="$1"
    local concurrency="$2"
    local image="$3"
    local args="$4"
    local tag="$5"
    local timeout_s="$6"

    local cpus="${profile%:*}"
    local mem="${profile#*:}"

    log "  [$tag] profile=$profile concurrency=$concurrency"

    local output
    output=$(timeout "$timeout_s" docker run --rm \
        --cpus="$cpus" \
        --memory="$mem" \
        --memory-swap="$mem" \
        "$image" $args "$concurrency" 2>&1)
    local rc=$?

    # Extract the last line that parses as JSON (bench image prints one).
    local json_line
    json_line=$(printf '%s\n' "$output" | tail -n 50 | grep -E '^\{' | tail -n 1)

    if [[ -z "$json_line" ]]; then
        # Detect OOM kill (exit 137), timeout (124), or other crash.
        local reason="crash"
        case $rc in
            124) reason="timeout" ;;
            137) reason="oom_killed" ;;
            143) reason="sigterm" ;;
        esac
        json_line=$(printf '{"engine":"%s","mode":"%s","profile":"%s","concurrency":%d,"status":"%s","exit":%d}' \
            "${tag%%-*}" "${tag#*-}" "$profile" "$concurrency" "$reason" "$rc")
    else
        # Augment the bench's JSON with profile + status.
        json_line=$(printf '%s' "$json_line" | sed \
            -e "s/^{/{\"profile\":\"$profile\",\"status\":\"ok\",/")
    fi

    printf '%s\n' "$json_line" | tee -a "$OUT" >&2
}

for profile in "${PROFILES[@]}"; do
    log ""
    log "=== profile $profile ==="
    for c in "${CONCURRENCIES[@]}"; do
        # Timeout scales with concurrency so 10k has room to breathe.
        timeout_s=$(( 60 + c / 200 ))

        run_cell "$profile" "$c" "ng-concurrent" ""       "neograph-threadpool" "$timeout_s"
        run_cell "$profile" "$c" "lg-concurrent" "async"  "langgraph-asyncio"   "$timeout_s"
        run_cell "$profile" "$c" "lg-concurrent" "mp"     "langgraph-mp"        "$timeout_s"
    done
done

log ""
log "Done. Results → $OUT"
wc -l "$OUT" >&2
