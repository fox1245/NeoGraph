#!/usr/bin/env bash
# Sweep the concurrent-bench matrix and append JSON lines to results.jsonl.
#
# Usage (from repo root):
#   bash benchmarks/concurrent/run_matrix.sh [output_file]
#
# Produces results.jsonl with one JSON object per (profile, concurrency,
# engine) cell. A cell is "ok" only when the harness exited 0 and its own
# record says status "ok" (every run returned the right answer, no failures).
# Anything else is "error": a harness record with failures, a crash, a timeout,
# or a container that Docker reports as OOM-killed (read from
# `docker inspect`, not guessed from exit status 137).

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

log "Building Haystack image..."
docker build -q -t hs-concurrent -f benchmarks/concurrent/Dockerfile.haystack . >&2

log "Building pydantic-graph image..."
docker build -q -t pg-concurrent -f benchmarks/concurrent/Dockerfile.pydantic_graph . >&2

log "Building LlamaIndex image..."
docker build -q -t li-concurrent -f benchmarks/concurrent/Dockerfile.llamaindex . >&2

log "Building AutoGen image..."
docker build -q -t ag-concurrent -f benchmarks/concurrent/Dockerfile.autogen . >&2

# ── Matrix ────────────────────────────────────────────────────────────
# profile = "cpus:mem"
PROFILES=("2:1g" "1:512m")
CONCURRENCIES=(10 100 1000 10000)

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

    # Named container, no --rm: the OOM flag is read before the container is removed.
    local name="ngbench-$$-${tag}-${profile//:/_}-${concurrency}"
    local output
    output=$(timeout "$timeout_s" docker run --name "$name" \
        --cpus="$cpus" \
        --memory="$mem" \
        --memory-swap="$mem" \
        "$image" $args "$concurrency" 2>&1)
    local rc=$?
    local oom
    oom=$(docker inspect -f '{{.State.OOMKilled}}' "$name" 2>/dev/null || echo unknown)
    docker rm -f "$name" >/dev/null 2>&1

    # Extract the last line that parses as JSON (bench image prints one).
    local json_line
    json_line=$(printf '%s\n' "$output" | tail -n 50 | grep -E '^\{' | tail -n 1)

    if [[ -z "$json_line" || "$oom" == "true" ]]; then
        # No record, or the kernel killed the container: report why.
        local reason="crash"
        if [[ "$oom" == "true" ]]; then
            reason="oom_killed"
        else
            case $rc in
                124) reason="timeout" ;;
                143) reason="sigterm" ;;
            esac
        fi
        json_line=$(printf '{"schema":2,"engine":"%s","mode":"%s","profile":"%s","concurrency":%d,"status":"error","reason":"%s","exit":%d,"oom_killed":"%s"}' \
            "${tag%%-*}" "${tag#*-}" "$profile" "$concurrency" "$reason" "$rc" "$oom")
    else
        # Add the profile and the exit status; the status is the harness's own
        # unless the process exited non-zero, which makes the cell an error.
        json_line=$(printf '%s' "$json_line" | python3 -c '
import json, sys
record = json.loads(sys.stdin.read())
profile, rc = sys.argv[1], int(sys.argv[2])
if rc != 0 or record.get("status") != "ok":
    record["status"] = "error"
record["profile"] = profile
record["exit"] = rc
print(json.dumps(record))' "$profile" "$rc") || json_line=$(printf \
            '{"schema":2,"engine":"%s","mode":"%s","profile":"%s","concurrency":%d,"status":"error","reason":"unreadable_record","exit":%d}' \
            "${tag%%-*}" "${tag#*-}" "$profile" "$concurrency" "$rc")
    fi

    printf '%s\n' "$json_line" | tee -a "$OUT" >&2
}

for profile in "${PROFILES[@]}"; do
    log ""
    log "=== profile $profile ==="
    for c in "${CONCURRENCIES[@]}"; do
        # Timeout scales with concurrency so 10k has room to breathe.
        # Larger for slow frameworks (LlamaIndex/AutoGen at N=10k = many minutes).
        timeout_s=$(( 120 + c / 20 ))

        run_cell "$profile" "$c" "ng-concurrent" ""       "neograph-threadpool" "$timeout_s"
        run_cell "$profile" "$c" "lg-concurrent" "async"  "langgraph-asyncio"   "$timeout_s"
        run_cell "$profile" "$c" "lg-concurrent" "mp"     "langgraph-mp"        "$timeout_s"
        run_cell "$profile" "$c" "hs-concurrent" "async"  "haystack-asyncio"    "$timeout_s"
        run_cell "$profile" "$c" "hs-concurrent" "mp"     "haystack-mp"         "$timeout_s"
        run_cell "$profile" "$c" "pg-concurrent" "async"  "pydantic-asyncio"    "$timeout_s"
        run_cell "$profile" "$c" "pg-concurrent" "mp"     "pydantic-mp"         "$timeout_s"
        run_cell "$profile" "$c" "li-concurrent" "async"  "llamaindex-asyncio"  "$timeout_s"
        run_cell "$profile" "$c" "li-concurrent" "mp"     "llamaindex-mp"       "$timeout_s"
        run_cell "$profile" "$c" "ag-concurrent" "async"  "autogen-asyncio"     "$timeout_s"
        run_cell "$profile" "$c" "ag-concurrent" "mp"     "autogen-mp"          "$timeout_s"
    done
done

log ""
log "Done. Results → $OUT"
wc -l "$OUT" >&2
