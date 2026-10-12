"""Transparent stdio tap between an MCP client and an MCP server subprocess.

Usage: stdio_tap.py --log FILE [--rewrite-init-version V] [--drop-init-capabilities]
                    -- <server argv...>

Every newline-delimited frame is forwarded unchanged (unless a rewrite option is
given) and logged as {"dir": "c2s"|"s2c", "raw": line, "json": parsed|null}.
stderr of the server is passed through.  Closing the client's stdin closes the
server's stdin; the tap exits when the server exits, and records the signal or
exit status of both ends as {"dir": "meta", ...} lines so child-lifecycle
handling by the client can be inspected.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import threading
import time


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--log", required=True)
    p.add_argument("--rewrite-init-version")
    p.add_argument("cmd", nargs=argparse.REMAINDER)
    a = p.parse_args()
    cmd = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd
    lock = threading.Lock()
    init_ids: set = set()

    def log(entry: dict) -> None:
        entry["t"] = time.time()
        with lock:
            with open(a.log, "a", encoding="utf-8") as fh:
                fh.write(json.dumps(entry) + "\n")

    child = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
    log({"dir": "meta", "event": "spawned", "pid": child.pid, "cmd": cmd})

    def on_signal(signum, _frame):
        log({"dir": "meta", "event": "tap_signal", "signal": signum})
        try:
            child.send_signal(signum)
        except ProcessLookupError:
            pass

    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, on_signal)

    def pump_in() -> None:
        stdin = sys.stdin.buffer
        while True:
            line = stdin.readline()
            if not line:
                log({"dir": "meta", "event": "client_closed_stdin"})
                try:
                    child.stdin.close()
                except OSError:
                    pass
                return
            text = line.decode("utf-8", "replace")
            try:
                obj = json.loads(text)
            except ValueError:
                obj = None
            if isinstance(obj, dict) and obj.get("method") == "initialize":
                init_ids.add(json.dumps(obj.get("id")))
            log({"dir": "c2s", "raw": text, "json": obj})
            try:
                child.stdin.write(line)
                child.stdin.flush()
            except OSError:
                return

    def pump_out() -> None:
        out = sys.stdout.buffer
        while True:
            line = child.stdout.readline()
            if not line:
                return
            text = line.decode("utf-8", "replace")
            try:
                obj = json.loads(text)
            except ValueError:
                obj = None
            if (a.rewrite_init_version and isinstance(obj, dict)
                    and json.dumps(obj.get("id")) in init_ids and isinstance(obj.get("result"), dict)):
                obj["result"]["protocolVersion"] = a.rewrite_init_version
                line = (json.dumps(obj) + "\n").encode()
                text = line.decode()
                log({"dir": "meta", "event": "rewrote_init_version", "to": a.rewrite_init_version})
            log({"dir": "s2c", "raw": text, "json": obj})
            try:
                out.write(line)
                out.flush()
            except OSError:
                return

    threading.Thread(target=pump_in, daemon=True).start()
    t_out = threading.Thread(target=pump_out, daemon=True)
    t_out.start()
    rc = child.wait()
    t_out.join(timeout=2)
    log({"dir": "meta", "event": "server_exited", "returncode": rc})
    return 0


if __name__ == "__main__":
    sys.exit(main())
