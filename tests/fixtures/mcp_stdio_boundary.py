"""Finite subprocess fixtures for MCP launch and ownership boundaries."""
import ctypes
import json
import os
import pathlib
import signal
import subprocess
import sys
import threading
import time


def watchdog():
    time.sleep(5)
    os._exit(0)


threading.Thread(target=watchdog, daemon=True).start()
mode = sys.argv[1]
if mode == "descendant":
    if os.name != "nt":
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
    print("ready", flush=True)
    time.sleep(1)
    pathlib.Path(sys.argv[2]).write_text("survived", encoding="utf-8")
    os._exit(0)

for line in sys.stdin:
    request = json.loads(line)
    if "id" not in request:
        continue
    method = request["method"]
    if method == "initialize":
        if mode == "startup":
            time.sleep(0.6)
        if mode == "stderr":
            os.write(2, b"x" * 8192)
            time.sleep(0.2)
        result = {"protocolVersion": "2025-11-25", "capabilities": {"tools": {}},
                  "serverInfo": {"name": "boundary", "version": "1"}}
    elif method == "tools/list":
        result = {"tools": [{"name": "probe", "description": "probe process state",
                              "inputSchema": {"type": "object"}}]}
    elif mode in ("tree", "tree-blocked"):
        child = subprocess.Popen([sys.executable, __file__, "descendant", sys.argv[2]],
                                 stdout=subprocess.PIPE, text=True)
        assert child.stdout.readline().strip() == "ready"
        if mode == "tree-blocked":
            pathlib.Path(sys.argv[2] + ".ready").write_text("entered", encoding="utf-8")
            time.sleep(2)
        result = {"content": [], "childPid": child.pid}
    elif mode == "blocked":
        pathlib.Path(sys.argv[2]).write_text("entered", encoding="utf-8")
        time.sleep(2)
        result = {"content": []}
    else:
        inherited = False
        if os.name == "nt" and len(sys.argv) > 2:
            flags = ctypes.c_ulong()
            inherited = bool(ctypes.windll.kernel32.GetHandleInformation(
                ctypes.c_void_p(int(sys.argv[2])), ctypes.byref(flags)))
        result = {"content": [], "cwd": os.getcwd(), "environment": dict(os.environ),
                  "inheritedHandle": inherited, "argv": sys.argv[3:]}
    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)
    if mode == "tree" and method == "tools/call":
        os._exit(0)
