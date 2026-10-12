#!/usr/bin/env python3
"""Transparent stdio tap for ACP agents (stdlib only).

    python3 -I acp_stdio_tap.py <transcript.jsonl> -- <agent> [args...]

Spawns the agent, relays the client's stdin to the agent and the agent's
stdout back, and appends every newline-delimited message to the transcript as
{"dir": "c2a"|"a2c", "t": <seconds>, "raw": <line>}. A line the agent writes
that is not a JSON object is still relayed but is marked "invalid": true, so a
stdout-hygiene violation shows up in the transcript. The agent's stderr goes
to this process's stderr.
"""

import json
import os
import subprocess
import sys
import threading
import time


def main() -> int:
    if len(sys.argv) < 4 or sys.argv[2] != "--":
        print(__doc__, file=sys.stderr)
        return 2
    transcript_path = sys.argv[1]
    command = sys.argv[3:]
    lock = threading.Lock()
    started = time.monotonic()
    transcript = open(transcript_path, "a", encoding="utf-8")

    def record(direction: str, raw: bytes) -> None:
        text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        entry = {"dir": direction, "t": round(time.monotonic() - started, 4), "raw": text}
        try:
            parsed = json.loads(text)
            if not isinstance(parsed, (dict, list)):
                entry["invalid"] = True
        except ValueError:
            entry["invalid"] = True
        try:
            raw.decode("utf-8")
        except UnicodeDecodeError:
            entry["invalid_utf8"] = True
        with lock:
            transcript.write(json.dumps(entry) + "\n")
            transcript.flush()

    agent = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=None)

    def pump_in() -> None:
        try:
            for line in iter(sys.stdin.buffer.readline, b""):
                record("c2a", line)
                agent.stdin.write(line)
                agent.stdin.flush()
        except (BrokenPipeError, ValueError):
            pass
        finally:
            try:
                agent.stdin.close()
            except Exception:
                pass

    def pump_out() -> None:
        out = sys.stdout.buffer
        try:
            for line in iter(agent.stdout.readline, b""):
                record("a2c", line)
                out.write(line)
                out.flush()
        except (BrokenPipeError, ValueError):
            pass

    t_in = threading.Thread(target=pump_in, daemon=True)
    t_out = threading.Thread(target=pump_out, daemon=True)
    t_in.start()
    t_out.start()
    code = agent.wait()
    t_out.join(timeout=2)
    transcript.close()
    try:
        sys.stdout.flush()
    except Exception:
        pass
    os._exit(code if code >= 0 else 128 - code)


if __name__ == "__main__":
    sys.exit(main())
