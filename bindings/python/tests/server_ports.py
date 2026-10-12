"""The ports of the servers the tests start: one contract for every suite.

1. A server that can bind port 0 itself does, and reports the port it got by
   printing one line, `READY <port>`, on stdout once it listens
   (`start_server(..., reports_port=True)`). No number is chosen beforehand, so
   nothing can take it in between.
2. A server that has to be told its number gets one from `reserve_port()`. The
   number is only a candidate: another process can take it before the server
   binds. The server then fails to bind and exits, and the whole spawn is
   repeated with a fresh number (`reports_port=False`).
3. Nothing is talked to until the listener is proven to be the process that was
   started: the child is alive, and the socket that listens on the port is one
   of its own (on Linux, read from /proc). A port that merely accepts
   connections says nothing; another test run's server, or any other listener,
   would pass for ours. Where the owner cannot be read, the child has to be
   alive and accepting for half a second, which is weaker.

The same contract is available to callers that are not Python through the
command line:

    python -I server_ports.py [--reports-port] [--port-env NAME] [--timeout S] -- COMMAND...

starts COMMAND (every `{port}` in it replaced by the number), prints
`READY <port>` when the server listens, and stops the server on SIGTERM.

NEOGRAPH_TEST_PORT_RANGE="lo-hi" confines the numbers `reserve_port()` hands
out, for machines that only open some ports.
"""

from __future__ import annotations

import argparse
import collections
import contextlib
import dataclasses
import os
import random
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from typing import Callable, Dict, List, Optional, Sequence, Set

HOST = "127.0.0.1"
_READY = re.compile(r"^READY (\d+)\s*$")
# What a server says when the number it was given is taken (the C++ probes say
# "bind failed").
_ADDRESS_IN_USE = re.compile(
    r"address already in use|EADDRINUSE|Errno 98|Errno 48|Errno 10048|"
    r"only one usage of each socket address|bind failed", re.IGNORECASE)
# Where the owner of a listener cannot be read, the child must keep accepting
# connections for this long.
_SETTLE_WITHOUT_OWNER_CHECK = 0.5


class ServerStartError(RuntimeError):
    """The server did not come up; the message carries what it printed."""


@dataclasses.dataclass
class Launch:
    """How to start one server: argv, environment (added to ours), working directory."""
    argv: List[str]
    env: Dict[str, str] = dataclasses.field(default_factory=dict)
    cwd: Optional[str] = None
    inherit_env: bool = True  # False: `env` is the whole environment, not additions to ours


# ----------------------------------------------------------------------------
# Numbers

_chosen: Set[int] = set()
_chosen_lock = threading.Lock()


def _range():
    spec = os.environ.get("NEOGRAPH_TEST_PORT_RANGE")
    if not spec:
        return None
    low, high = (int(part) for part in spec.split("-"))
    return low, high


def _is_free(port: int) -> bool:
    with socket.socket() as probe:
        try:
            probe.bind((HOST, port))
        except OSError:
            return False
    return True


def reserve_port() -> int:
    """A loopback port nobody uses right now, for a server that must be told one.

    The number is released again at once, so it is only a candidate: pass it to
    `start_server(..., reports_port=False)`, which repeats the start when the
    number was taken meanwhile. Numbers already handed out are not handed out
    twice in one process.
    """
    bounds = _range()
    for _ in range(200):
        if bounds:
            port = random.randint(*bounds)
            if not _is_free(port):
                continue
        else:
            with socket.socket() as probe:
                probe.bind((HOST, 0))
                port = probe.getsockname()[1]
        with _chosen_lock:
            if port in _chosen:
                continue
            _chosen.add(port)
        return port
    raise RuntimeError("no free loopback port to hand out")


# ----------------------------------------------------------------------------
# Who listens on a port

def _listening_inodes(port: int) -> Optional[Set[int]]:
    """Inodes of the sockets that listen on `port` (IPv4 and IPv6), from /proc."""
    inodes: Set[int] = set()
    readable = False
    for table in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            with open(table, encoding="ascii") as handle:
                lines = handle.read().splitlines()[1:]
        except OSError:
            continue
        readable = True
        for line in lines:
            fields = line.split()
            if len(fields) > 9 and fields[3] == "0A" and int(fields[1].rsplit(":", 1)[1], 16) == port:
                inodes.add(int(fields[9]))
    return inodes if readable else None


def _socket_inodes(pid: int) -> Set[int]:
    found: Set[int] = set()
    directory = "/proc/%d/fd" % pid
    try:
        names = os.listdir(directory)
    except OSError:
        return found
    for name in names:
        try:
            target = os.readlink(os.path.join(directory, name))
        except OSError:
            continue
        if target.startswith("socket:["):
            found.add(int(target[8:-1]))
    return found


def process_tree(pid: int) -> Set[int]:
    """`pid` and its descendants (a server may run in a child of what was started)."""
    tree = {pid}
    pending = [pid]
    while pending:
        current = pending.pop()
        try:
            tasks = os.listdir("/proc/%d/task" % current)
        except OSError:
            continue
        for task in tasks:
            try:
                with open("/proc/%d/task/%s/children" % (current, task), encoding="ascii") as handle:
                    children = [int(child) for child in handle.read().split()]
            except OSError:
                continue
            for child in children:
                if child not in tree:
                    tree.add(child)
                    pending.append(child)
    return tree


def listener_state(port: int, pids: Set[int]) -> Optional[str]:
    """Who listens on `port`: "ours" when a process of `pids` does, "other" when only
    some other process does, "none", or None where this cannot be read (not Linux)."""
    if not sys.platform.startswith("linux"):
        return None
    inodes = _listening_inodes(port)
    if inodes is None:
        return None
    if not inodes:
        return "none"
    for pid in pids:
        if inodes & _socket_inodes(pid):
            return "ours"
    return "other"


def _accepts(port: int) -> bool:
    try:
        socket.create_connection((HOST, port), 0.2).close()
        return True
    except OSError:
        return False


# ----------------------------------------------------------------------------
# For the servers themselves: bind, keep the number, say so

def bind_listener(port: int = 0, host: str = HOST) -> socket.socket:
    """A listening socket on `port` (0: the OS picks), for the server that serves on it.

    Bound first and kept open, the number cannot be taken by anyone else: a
    server that must name its port in what it serves (a card, a resource URL)
    knows it before it starts. A taken number raises OSError "Address already
    in use", which the starting side recognises."""
    listener = socket.socket()
    if os.name != "nt":  # on Windows SO_REUSEADDR would let a second socket share the port
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((host, port))
    listener.listen(128)  # listening, the number is exclusive: a second socket cannot share it
    return listener


def announce(port: int) -> None:
    """Say, as the starting side expects, that the server listens on `port`."""
    print("READY %d" % port, flush=True)


def serve_uvicorn(app, listener: socket.socket, lifespan: str = "auto") -> None:
    """Serve an ASGI app on a listener from `bind_listener`; print READY once it listens."""
    import uvicorn

    class _Announcing(uvicorn.Server):
        async def startup(self, sockets=None):
            await super().startup(sockets=sockets)
            if self.servers:
                announce(listener.getsockname()[1])

    _Announcing(uvicorn.Config(app, log_level="warning", lifespan=lifespan)).run(sockets=[listener])


# ----------------------------------------------------------------------------
# A started server

class _ExitedEarly(Exception):
    def __init__(self, code: int):
        super().__init__(code)
        self.code = code


class _NotListening(Exception):
    pass


class _Spawn:
    """One server process with its output: stdout watched for READY, stderr in a file."""

    def __init__(self, launch: Launch, stderr_path: Optional[str]):
        self.own_stderr = stderr_path is None
        if stderr_path is None:
            handle, stderr_path = tempfile.mkstemp(prefix="neograph-server-", suffix=".stderr")
            os.close(handle)
        self.stderr_path = str(stderr_path)
        self.stderr_offset = os.path.getsize(self.stderr_path) if os.path.exists(self.stderr_path) else 0
        self.lines = collections.deque(maxlen=200)  # type: collections.deque
        self.reported: Optional[int] = None
        self.reported_event = threading.Event()
        with open(self.stderr_path, "ab") as stderr:
            self.proc = subprocess.Popen(
                launch.argv, env=({**os.environ, **launch.env} if launch.inherit_env else dict(launch.env)),
                cwd=launch.cwd,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=stderr,
                universal_newlines=True, errors="replace")
        self.reader = threading.Thread(target=self._read_stdout, daemon=True)
        self.reader.start()

    def _read_stdout(self) -> None:
        # Read to the end: a server that keeps printing must never block on a full pipe.
        for line in self.proc.stdout:
            line = line.rstrip("\r\n")
            self.lines.append(line)
            match = _READY.match(line)
            if match and self.reported is None:
                self.reported = int(match.group(1))
                self.reported_event.set()

    def pids(self) -> Set[int]:
        return process_tree(self.proc.pid) if sys.platform.startswith("linux") else {self.proc.pid}

    def output(self) -> str:
        try:
            with open(self.stderr_path, "rb") as handle:
                handle.seek(self.stderr_offset)
                err = handle.read().decode("utf-8", "replace")
        except OSError:
            err = ""
        out = "\n".join(self.lines)
        text = ""
        if out:
            text += "stdout:\n" + out[-3000:] + "\n"
        if err:
            text += "stderr:\n" + err[-3000:]
        return text

    def wait_listening(self, number: int, reports_port: bool, timeout: float) -> int:
        deadline = time.monotonic() + timeout
        accepting_since = None
        while True:
            code = self.proc.poll()
            if code is not None:
                raise _ExitedEarly(code)
            if reports_port:
                if self.reported_event.wait(0.02):
                    if number and self.reported != number:
                        raise ServerStartError("the server was given port %d but reports %d"
                                               % (number, self.reported))
                    number = self.reported
                    if listener_state(number, self.pids()) in ("ours", None):
                        return number
            else:
                state = listener_state(number, self.pids())
                if state == "ours":
                    return number
                if state is None:
                    if _accepts(number):
                        accepting_since = accepting_since or time.monotonic()
                        if time.monotonic() - accepting_since >= _SETTLE_WITHOUT_OWNER_CHECK:
                            return number
                    else:
                        accepting_since = None
            if time.monotonic() >= deadline:
                raise _NotListening()
            time.sleep(0.02)

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.reader.join(2)
        with contextlib.suppress(Exception):
            self.proc.stdout.close()

    def forget(self) -> None:
        if self.own_stderr:
            with contextlib.suppress(OSError):
                os.unlink(self.stderr_path)


class ServerProcess:
    """A server that was started and is known to listen on `port`."""

    def __init__(self, spawn: _Spawn, port: int, attempts: int):
        self._spawn = spawn
        self.proc = spawn.proc
        self.port = port
        self.attempts = attempts

    def url(self, path: str = "") -> str:
        return "http://%s:%d%s" % (HOST, self.port, path)

    def output(self) -> str:
        return self._spawn.output()

    def stop(self) -> None:
        self._spawn.stop()
        self._spawn.forget()

    def __enter__(self) -> "ServerProcess":
        return self

    def __exit__(self, *_exc) -> None:
        self.stop()


def start_server(launch_for_port: Callable[[int], Launch], *, reports_port: bool = False,
                 port: Optional[int] = None, stderr_path: Optional[str] = None,
                 timeout: float = 60.0, attempts: int = 8) -> ServerProcess:
    """Start a server and return it once it is proven to listen on its port.

    `launch_for_port(n)` says how to start the server on port n.

    reports_port=True: the server binds port 0 itself (n is 0) and prints
    `READY <port>` once it listens and serves; the port it reports is the
    result. With `port=` the server is given that number instead (a restart on
    the old port), and has to report the same one.

    reports_port=False: the server is told a number from `reserve_port()`. When
    the server exits because that number was taken meanwhile, the start is
    repeated with a fresh one, up to `attempts` times. A server that exits for
    another reason is not started again. `port=` fixes the number (a restart on
    the old port); the start is then not repeated.

    The server's stderr is appended to `stderr_path`, or kept in a temporary
    file that is read for the error message.
    """
    tries = attempts if (not reports_port and port is None) else 1
    history: List[str] = []
    for attempt in range(1, tries + 1):
        number = port if port is not None else (0 if reports_port else reserve_port())
        spawn = _Spawn(launch_for_port(number), stderr_path)
        label = "attempt %d of %d, port %s" % (attempt, tries, number if number else "0 (reported)")
        try:
            bound = spawn.wait_listening(number, reports_port, timeout)
        except _ExitedEarly as early:
            spawn.stop()
            text = spawn.output()
            spawn.forget()
            history.append("%s: exited with status %s\n%s" % (label, early.code, text))
            if number and (_ADDRESS_IN_USE.search(text) or not _is_free(number)):
                if port is None:
                    continue  # the number was taken meanwhile: a fresh one
                history.append("port %d is in use by another process" % number)
            raise ServerStartError("\n".join(history))
        except _NotListening:
            spawn.stop()
            history.append("%s: did not listen within %.1f s\n%s" % (label, timeout, spawn.output()))
            spawn.forget()
            raise ServerStartError("\n".join(history))
        except BaseException:
            spawn.stop()
            spawn.forget()
            raise
        return ServerProcess(spawn, bound, attempt)
    raise ServerStartError("every start found its port taken:\n" + "\n".join(history))


def wait_for_listener(proc: subprocess.Popen, port: int, timeout: float = 60.0) -> None:
    """Wait until `proc`, or a descendant of it, listens on `port`.

    For a process started elsewhere: raises when it exits first, and never
    takes another process's listener for it. (`start_server` does this itself.)"""
    deadline = time.monotonic() + timeout
    accepting_since = None
    while True:
        if proc.poll() is not None:
            raise ServerStartError("the server exited with status %s before it listened on %d"
                                   % (proc.returncode, port))
        pids = process_tree(proc.pid) if sys.platform.startswith("linux") else {proc.pid}
        state = listener_state(port, pids)
        if state == "ours":
            return
        if state is None and _accepts(port):
            accepting_since = accepting_since or time.monotonic()
            if time.monotonic() - accepting_since >= _SETTLE_WITHOUT_OWNER_CHECK:
                return
        elif state is None:
            accepting_since = None
        if time.monotonic() >= deadline:
            raise ServerStartError("the server did not listen on %d within %.1f s" % (port, timeout))
        time.sleep(0.02)


# ----------------------------------------------------------------------------
# Command line: the contract for callers that are not Python

def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--reports-port", action="store_true",
                        help="the command binds port 0 and prints READY <port> itself")
    parser.add_argument("--port-env", metavar="NAME",
                        help="also pass the number in this environment variable")
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("no command to start")

    def launch(port: int) -> Launch:
        env = {args.port_env: str(port)} if args.port_env else {}
        return Launch([part.replace("{port}", str(port)) for part in command], env)

    stopping = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stopping.set())
    signal.signal(signal.SIGINT, lambda *_: stopping.set())
    try:
        server = start_server(launch, reports_port=args.reports_port, timeout=args.timeout)
    except ServerStartError as error:
        print(error, file=sys.stderr, flush=True)
        return 1
    print("READY %d" % server.port, flush=True)
    try:
        while not stopping.is_set() and server.proc.poll() is None:
            stopping.wait(0.1)
        code = server.proc.poll()
    finally:
        server.stop()
    return code or 0


if __name__ == "__main__":
    sys.exit(main())
