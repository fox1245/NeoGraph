"""One contract for the ports of the servers the tests start (server_ports.py).

A decoy listener takes the number a server was going to get, between the moment
the number was chosen and the moment the server starts. The helper must neither
talk to it nor take it for the server.
"""

import http.client
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

import port_decoy
import server_ports
from port_decoy import Decoy

STUB = str(Path(__file__).with_name("port_stub_server.py"))
LINUX = pytest.mark.skipif(
    sys.platform != "linux",
    reason="who listens on a port is read from /proc, which only Linux has")


def stub(*extra):
    """The launch of the stub server for a given port."""
    return lambda port: server_ports.Launch(
        [sys.executable, "-I", STUB, "--port", str(port), *extra])


def identity(port):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    try:
        connection.request("GET", "/")
        return connection.getresponse().getheader("X-Identity")
    finally:
        connection.close()


@pytest.fixture
def decoy_takes_the_first_number(monkeypatch):
    decoys = port_decoy.take_the_first_number(monkeypatch)
    yield decoys
    for decoy in decoys:
        decoy.close()


def test_a_server_that_reports_its_port_binds_port_zero_and_nothing_is_reserved(monkeypatch):
    monkeypatch.setattr(server_ports, "reserve_port",
                        lambda: pytest.fail("no number should be chosen for a server that reports its own"))
    with server_ports.start_server(stub(), reports_port=True) as server:
        assert server.port > 0 and server.attempts == 1
        assert identity(server.port) == "real"
    assert server.proc.poll() is not None  # the context manager stopped it


@LINUX
def test_a_decoy_on_the_chosen_number_is_not_talked_to(decoy_takes_the_first_number):
    with server_ports.start_server(stub(), reports_port=False) as server:
        decoy = decoy_takes_the_first_number[0]
        assert server.port != decoy.port
        assert server.attempts == 2  # the spawn was repeated with a fresh number
        assert identity(server.port) == "real"
        assert (decoy.connections, decoy.requests) == (0, 0)


@LINUX
def test_a_decoy_is_not_taken_for_a_server_that_is_slow_to_start(decoy_takes_the_first_number):
    # The decoy accepts connections for the whole second the server needs
    # before it binds; a wait that only asks whether the port accepts would
    # return on the first of them.
    started = time.monotonic()
    with server_ports.start_server(stub("--delay", "1"), reports_port=False) as server:
        decoy = decoy_takes_the_first_number[0]
        assert time.monotonic() - started >= 1.0
        assert server.port != decoy.port
        assert identity(server.port) == "real"
        assert (decoy.connections, decoy.requests) == (0, 0)


@LINUX
def test_without_the_ownership_check_the_decoy_would_be_taken_for_the_server(
        decoy_takes_the_first_number, monkeypatch):
    # The control of the two tests above: the decoy does answer, so it is the
    # check of who listens that keeps the helper away from it.
    monkeypatch.setattr(server_ports, "listener_state", lambda port, pids: "ours")
    with server_ports.start_server(stub("--delay", "1"), reports_port=False) as server:
        assert server.port == decoy_takes_the_first_number[0].port
        assert identity(server.port) == "decoy"


@LINUX
def test_a_fixed_number_that_is_taken_fails_instead_of_talking_to_its_holder():
    decoy = Decoy()
    try:
        with pytest.raises(server_ports.ServerStartError, match="in use"):
            server_ports.start_server(stub(), reports_port=False, port=decoy.port)
        assert (decoy.connections, decoy.requests) == (0, 0)
    finally:
        decoy.close()


def test_a_server_that_fails_for_another_reason_is_not_started_again():
    with pytest.raises(server_ports.ServerStartError, match="reason of its own") as failure:
        server_ports.start_server(stub("--die", "3"), reports_port=False)
    assert "attempt 1 of" in str(failure.value) and "attempt 2 of" not in str(failure.value)


def test_a_server_that_never_listens_times_out_with_its_output():
    with pytest.raises(server_ports.ServerStartError, match="did not listen"):
        server_ports.start_server(stub("--delay", "30"), reports_port=False, timeout=1.0)


def test_the_launcher_starts_a_server_for_a_caller_that_is_not_python():
    # The TypeScript interop test starts its servers through this command.
    # Closing the launcher's standard input is the stop signal that works on every
    # platform: a hard kill of the launcher on Windows would leave the server running.
    launcher = subprocess.Popen(
        [sys.executable, "-I", server_ports.__file__, "--stop-on-stdin-eof",
         "--", sys.executable, "-I", STUB, "--port", "{port}"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        line = launcher.stdout.readline().split()
        assert line[0] == "READY"
        assert identity(int(line[1])) == "real"
    finally:
        launcher.stdin.close()
        assert launcher.wait(timeout=10) is not None
        launcher.stdout.close()
    with pytest.raises(OSError):
        socket.create_connection(("127.0.0.1", int(line[1])), 1).close()


def test_an_environment_variable_can_carry_the_number():
    # Servers such as the Harness example take their port from the environment.
    launch = lambda port: server_ports.Launch(
        [sys.executable, "-I", "-c",
         "import os, runpy, sys; sys.argv = [sys.argv[0], '--port', os.environ['STUB_PORT']];"
         f"runpy.run_path({STUB!r}, run_name='__main__')"],
        env={"STUB_PORT": str(port)})
    with server_ports.start_server(launch, reports_port=False) as server:
        assert identity(server.port) == "real"


def test_a_bound_listener_keeps_its_number_from_everyone_else():
    listener = server_ports.bind_listener()
    try:
        with pytest.raises(OSError):
            server_ports.bind_listener(listener.getsockname()[1])
    finally:
        listener.close()


def test_a_uvicorn_server_binds_port_zero_and_reports_it():
    pytest.importorskip("uvicorn")
    code = (
        "import sys; sys.path.insert(0, %r); import server_ports\n"
        "async def app(scope, receive, send):\n"
        "    if scope['type'] == 'http':\n"
        "        await send({'type': 'http.response.start', 'status': 200,\n"
        "                    'headers': [(b'x-identity', b'asgi'), (b'content-length', b'0')]})\n"
        "        await send({'type': 'http.response.body', 'body': b''})\n"
        "    elif scope['type'] == 'lifespan':\n"
        "        while True:\n"
        "            message = await receive()\n"
        "            await send({'type': message['type'] + '.complete'})\n"
        "            if message['type'] == 'lifespan.shutdown':\n"
        "                return\n"
        "server_ports.serve_uvicorn(app, server_ports.bind_listener(int(sys.argv[1])))\n"
    ) % str(Path(__file__).resolve().parent)
    launch = lambda port: server_ports.Launch([sys.executable, "-I", "-c", code, str(port)])
    with server_ports.start_server(launch, reports_port=True) as server:
        assert identity(server.port) == "asgi"


@LINUX
def test_a_server_that_reports_can_be_restarted_on_its_old_number():
    with server_ports.start_server(stub(), reports_port=True) as first:
        port = first.port
    with server_ports.start_server(stub(), reports_port=True, port=port) as again:
        assert again.port == port and identity(port) == "real"
    decoy = Decoy()
    try:
        with pytest.raises(server_ports.ServerStartError, match="in use"):
            server_ports.start_server(stub(), reports_port=True, port=decoy.port)
        assert (decoy.connections, decoy.requests) == (0, 0)
    finally:
        decoy.close()


def test_reserved_numbers_are_not_handed_out_twice():
    numbers = [server_ports.reserve_port() for _ in range(50)]
    assert len(set(numbers)) == len(numbers)


def test_a_port_range_confines_the_numbers(monkeypatch):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        low = probe.getsockname()[1]
    monkeypatch.setenv("NEOGRAPH_TEST_PORT_RANGE", "%d-%d" % (low, low + 3))
    assert low <= server_ports.reserve_port() <= low + 3
