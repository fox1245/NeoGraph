#!/bin/bash
# Spin up 4 member servers, send a bill to the speaker, tear down.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Find the binaries. In-tree (NeoGraph repo) build lives at
# <neograph-root>/build-pybind or <neograph-root>/build; standalone
# build at $ROOT/build. Try each in order.
NG_ROOT="$(cd "$ROOT/../../.." && pwd 2>/dev/null)" || NG_ROOT=""
if [ -n "$NG_ROOT" ] && [ -x "$NG_ROOT/build-pybind/cookbook_ai_assembly_member" ]; then
    BUILD="$NG_ROOT/build-pybind"
elif [ -n "$NG_ROOT" ] && [ -x "$NG_ROOT/build/cookbook_ai_assembly_member" ]; then
    BUILD="$NG_ROOT/build"
else
    BUILD="$ROOT/build"
fi

BILL="${1:-$ROOT/bills/basic_income.txt}"

if [ ! -x "$BUILD/cookbook_ai_assembly_member" ] || [ ! -x "$BUILD/cookbook_ai_assembly_speaker" ]; then
    echo "Build first. From NeoGraph root:"
    echo "  cmake --build build-pybind --target cookbook_ai_assembly_member cookbook_ai_assembly_speaker -j4"
    echo "or standalone (sibling repo):"
    echo "  cmake -S '$ROOT' -B '$ROOT/build' && cmake --build '$ROOT/build' -j4"
    exit 1
fi

# Load OPENAI_API_KEY from .env (cwd or NeoGraph root) if present.
for envf in "$ROOT/.env" "$NG_ROOT/.env"; do
    if [ -n "$envf" ] && [ -f "$envf" ]; then set -a; . "$envf"; set +a; break; fi
done
if [ -z "$OPENAI_API_KEY" ]; then
    echo "OPENAI_API_KEY not set"; exit 2
fi

PIDS=()
cleanup() {
    echo
    echo "[shutdown] stopping member servers..."
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null
}
trap cleanup EXIT

start_member() {
    local port=$1; local name=$2; local party=$3; local prompt=$4
    "$BUILD/cookbook_ai_assembly_member" "$port" "$name" "$party" "$ROOT/prompts/$prompt" &
    PIDS+=($!)
}

start_member 8101 김진보 진보당 jinbo.txt
start_member 8102 박보수 보수당 bosu.txt
start_member 8103 정중도 중도당 jungdo.txt
start_member 8104 나녹색 녹색당 nokdang.txt

# Wait until all members are reachable on their well-known card path.
for port in 8101 8102 8103 8104; do
    for i in $(seq 1 50); do
        if curl -sf -o /dev/null --max-time 1 "http://127.0.0.1:$port/.well-known/agent-card.json"; then
            echo "  ✓ port $port ready"
            break
        fi
        sleep 0.1
    done
done

echo
"$BUILD/cookbook_ai_assembly_speaker" "$BILL" \
    "http://127.0.0.1:8101" \
    "http://127.0.0.1:8102" \
    "http://127.0.0.1:8103" \
    "http://127.0.0.1:8104"
