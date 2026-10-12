"""Transparent stdio recorder: `stdio_tee.py LOG -- server argv...`.

Spawns the server, forwards stdin/stdout byte-for-byte, and appends one JSONL
record per newline-delimited frame to LOG: {"t", "dir": "c2s"|"s2c", "raw"}.
The server's stderr is passed through unchanged (MCP allows logs there).
Used as the command an official MCP client launches, so the wire is captured
without touching either peer.
"""

import json
import os
import subprocess
import sys
import threading
import time


def main() -> int:
    if len(sys.argv) < 4 or sys.argv[2] != "--":
        print("usage: stdio_tee.py LOG -- argv...", file=sys.stderr)
        return 2
    log_path = sys.argv[1]
    argv = sys.argv[3:]
    lock = threading.Lock()
    log = open(log_path, "a", encoding="utf-8")

    def record(direction: str, line: bytes) -> None:
        with lock:
            log.write(json.dumps({"t": time.time(), "dir": direction,
                                  "raw": line.decode("utf-8", "replace")}) + "\n")
            log.flush()

    proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def pump_in() -> None:
        stdin = sys.stdin.buffer
        try:
            while True:
                line = stdin.readline()
                if not line:
                    break
                record("c2s", line.rstrip(b"\r\n"))
                proc.stdin.write(line)
                proc.stdin.flush()
        except (BrokenPipeError, OSError):
            pass
        finally:
            try:
                proc.stdin.close()
            except OSError:
                pass

    def pump_out() -> None:
        out = sys.stdout.buffer
        try:
            while True:
                line = proc.stdout.readline()
                if not line:
                    break
                record("s2c", line.rstrip(b"\r\n"))
                out.write(line)
                out.flush()
        except (BrokenPipeError, OSError):
            pass

    t_in = threading.Thread(target=pump_in, daemon=True)
    t_out = threading.Thread(target=pump_out, daemon=True)
    t_in.start()
    t_out.start()
    t_out.join()
    code = proc.wait()
    with lock:
        log.close()
    return code


if __name__ == "__main__":
    sys.exit(main())
