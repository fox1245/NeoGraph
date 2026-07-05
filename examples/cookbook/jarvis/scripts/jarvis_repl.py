#!/usr/bin/env python3
"""자비스 한글 입력용 REPL 래퍼.

cookbook_jarvis 는 stdin 을 raw 로 읽어서 터미널에서 한글 편집(백스페이스,
커서 이동)이 깨지거나 입력이 씹힌다. 이 래퍼가 GNU readline 으로 한 줄을
완성해 받아 자비스 stdin 에 넘긴다 — 한글 편집, ↑↓ 히스토리, UTF-8 전부
readline 이 처리.

사용:
    python3 scripts/jarvis_repl.py                    # 기본 config-demo/real-tools
    python3 scripts/jarvis_repl.py config-demo/mock   # run_jarvis.sh 인자 그대로 전달

종료: Ctrl+D 또는 Ctrl+C (자비스에 EOF 전달 → __shutdown__ 정상 종료 경로)
"""
import atexit
import os
import readline  # noqa: F401 — import 자체가 input() 에 라인 편집을 붙인다
import subprocess
import sys
import threading

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))          # .../jarvis/scripts
JARVIS_DIR = os.path.dirname(SCRIPT_DIR)                          # .../jarvis
NEOGRAPH_ROOT = os.path.abspath(os.path.join(JARVIS_DIR, "..", "..", ".."))

# ── 히스토리 (↑↓ 로 이전 발화 재사용) ────────────────────────────────────────
HIST_FILE = os.path.expanduser("~/.jarvis_history")
try:
    readline.read_history_file(HIST_FILE)
except OSError:
    pass
atexit.register(lambda: readline.write_history_file(HIST_FILE))

# ── OPENAI_API_KEY 자동 로드 — 없으면 NeoGraph 루트 .env 에서 ───────────────
if not os.environ.get("OPENAI_API_KEY"):
    env_path = os.path.join(NEOGRAPH_ROOT, ".env")
    if os.path.exists(env_path):
        with open(env_path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, _, v = line.partition("=")
                os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))

# ── 자비스 기동 (run_jarvis.sh 경유 — LD_LIBRARY_PATH·cwd 처리 재사용) ──────
proc = subprocess.Popen(
    ["bash", os.path.join(SCRIPT_DIR, "run_jarvis.sh"), *sys.argv[1:]],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,   # 턴 완료 감지를 위해 파이프, 아래서 그대로 에코
    stderr=None,              # 디버그/로그는 터미널 직행
    text=True,
    bufsize=1,
)

# ── stdout 펌프 — 그대로 에코하면서 턴 완료 마커 감지 ───────────────────────
turn_done = threading.Event()


def pump() -> None:
    assert proc.stdout is not None
    for line in proc.stdout:
        print(line, end="", flush=True)
        # 기동 완료("온라인") 또는 턴 완료(TTS 생성 라인)에서 프롬프트 개방
        if line.startswith("[jarvis:tts]") or "온라인" in line:
            turn_done.set()
    turn_done.set()  # 프로세스 종료 시에도 대기 해제


threading.Thread(target=pump, daemon=True).start()

# 모델 로드(whisper ~10초) 대기 후 첫 프롬프트
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
        # 턴 완료 대기 — 응답 없는 경로(빈 final_text 등)를 위해 타임아웃 폴백
        turn_done.wait(timeout=180)
finally:
    try:
        if proc.stdin:
            proc.stdin.close()   # EOF → 자비스 __shutdown__ 정상 종료
    except Exception:
        pass
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.terminate()
