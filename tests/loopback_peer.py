"""Loopback-only TLS listener shared by the isolated typed-endpoint test peers."""
import http.server
import socket
import ssl
import threading


class _IPv6Loopback(http.server.ThreadingHTTPServer):
    address_family = socket.AF_INET6


class LoopbackPeer:
    """Serve one handler over TLS on 127.0.0.1 and, when the host has it, on ::1 at the same port.

    `localhost` resolves to ::1 first on Windows, and Windows takes about two seconds to refuse a
    loopback connection, which is longer than the deadlines these tests exercise. The peer
    therefore has to answer on whichever family the client dials first, without listening
    beyond loopback.
    """

    def __init__(self, handler: type, context: ssl.SSLContext):
        primary = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.port = primary.server_port
        self._servers = [primary]
        try:
            self._servers.append(_IPv6Loopback(("::1", self.port), handler))
        except OSError:
            pass  # No IPv6 loopback, or the port is taken there: localhost cannot resolve to it first.
        for server in self._servers:
            server.socket = context.wrap_socket(server.socket, server_side=True)
        self._threads = [threading.Thread(target=server.serve_forever, daemon=True)
                         for server in self._servers]

    def start(self) -> None:
        for thread in self._threads:
            thread.start()

    def stop(self) -> None:
        for server in self._servers:
            server.shutdown()
        for server in self._servers:
            server.server_close()
        for thread in self._threads:
            thread.join()
