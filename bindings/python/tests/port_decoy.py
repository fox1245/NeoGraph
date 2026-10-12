"""A listener that takes the port a test server was going to get.

For the tests of the port contract (server_ports.py): the decoy answers like a server of
its own and counts who comes, so a test can show that nobody talked to it.
"""

import socket
import threading

import server_ports


class Decoy:
    """A listener that answers like a server of its own and counts who comes."""

    def __init__(self, port=0):
        self.socket = socket.socket()
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.bind(("127.0.0.1", port))
        self.socket.listen(16)
        self.port = self.socket.getsockname()[1]
        self.connections = 0
        self.requests = 0
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        while True:
            try:
                peer, _ = self.socket.accept()
            except OSError:
                return
            self.connections += 1
            with peer:
                peer.settimeout(0.5)
                try:
                    if peer.recv(4096):
                        self.requests += 1
                        peer.sendall(b"HTTP/1.1 200 OK\r\nX-Identity: decoy\r\n"
                                     b"Content-Length: 5\r\nConnection: close\r\n\r\ndecoy")
                except OSError:
                    pass

    def close(self):
        self.socket.close()


def take_the_first_number(monkeypatch):
    """Make `reserve_port` hand out, as its first number, one a decoy holds at once:
    another process gets in between the choice of the number and the server's bind.
    Returns the list the decoy is appended to; the caller closes it."""
    real = server_ports.reserve_port
    decoys = []

    def reserve():
        port = real()
        if not decoys:
            decoys.append(Decoy(port))
        return port

    monkeypatch.setattr(server_ports, "reserve_port", reserve)
    return decoys
