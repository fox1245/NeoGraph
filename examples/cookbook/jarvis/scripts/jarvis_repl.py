#!/usr/bin/env python3
"""REPL wrapper for Korean text input to JARVIS.

cookbook_jarvis reads raw stdin, which can break Hangul line editing (backspace,
cursor movement) or drop characters in a terminal. This wrapper reads a full
line via GNU readline and forwards it to JARVIS stdin — readline handles Hangul
editing, ↑↓ history, and UTF-8.

Usage:
    python3 scripts/jarvis_repl.py                    # default config-demo/real-tools
    python3 scripts/jarvis_repl.py config-demo/mock   # forwards args to run_jarvis.sh

Exit: Ctrl+D or Ctrl+C (sends EOF to JARVIS → clean __shutdown__ exit path)
"""
import atexit
import os
import readline  # noqa: F401 — importing attaches line editing to input()
import subprocess
import sys
import threading

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))          # .../jarvis/scripts
JARVIS_DIR = os.path.dirname(SCRIPT_DIR)                          # .../jarvis
NEOGRAPH_ROOT = os.path.abspath(os.path.join(JARVIS_DIR, "..", "..", ".."))

# ── History (reuse prior utterances with ↑↓) ─────────────────────────────────
HIST_FILE = os.path.expanduser("~/.jarvis_history")
try:
    readline.read_history_file(HIST_FILE)
except OSError:
    pass
atexit.register(lambda: readline.write_history_file(HIST_FILE))

# ── Auto-load OPENROUTER_API_KEY from NeoGraph root .env if unset ────────────
if not os.environ.get("OPENROUTER_API_KEY"):
    env_path = os.path.join(NEOGRAPH_ROOT, ".env")
    if os.path.exists(env_path):
        with open(env_path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, _, v = line.partition("=")
                os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))

# ── Launch JARVIS (via run_jarvis.sh — reuses LD_LIBRARY_PATH and cwd setup) ─
proc = subprocess.Popen(
    ["bash", os.path.join(SCRIPT_DIR, "run_jarvis.sh"), *sys.argv[1:]],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,   # piped to detect turn completion, echoed below
    stderr=None,              # debug/logs go straight to the terminal
    text=True,
    bufsize=1,
)

# ── stdout pump — echo lines while watching for turn-completion markers ──────
turn_done = threading.Event()


def pump() -> None:
    assert proc.stdout is not None
    for line in proc.stdout:
        print(line, end="", flush=True)
        # Unlock the prompt once startup prints the Korean "online" marker or a turn finishes (TTS line)
        if line.startswith("[jarvis:tts]") or "온라인" in line:
            turn_done.set()
    turn_done.set()  # unblock waiting if the process exits


threading.Thread(target=pump, daemon=True).start()

# Wait for model loading (whisper ~10s) before showing the first prompt
turn_done.wait(timeout=180)

try:
    while proc.poll() is None:
        try:
            line = input("토니 ▸ ")
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not line.strip():
            continue
        turn_done.clear()
        try:
            assert proc.stdin is not None
            proc.stdin.write(line + "\n")
            proc.stdin.flush()
        except BrokenPipeError:
            break
        # Wait for turn completion — timeout fallback for paths with no output (e.g. empty final_text)
        turn_done.wait(timeout=180)
finally:
    try:
        if proc.stdin:
            proc.stdin.close()   # EOF → clean JARVIS __shutdown__ exit
    except Exception:
        pass
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.terminate()
