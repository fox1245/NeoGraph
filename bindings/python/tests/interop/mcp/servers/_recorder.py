"""ASGI middleware that records HTTP exchanges as JSON lines (no SDK imports)."""

from __future__ import annotations

import json
import threading
import time
from typing import Any


class Recorder:
    """ASGI middleware recording every HTTP exchange as one JSON line."""

    def __init__(self, app, path: str | None):
        self.app = app
        self.path = path
        self.lock = threading.Lock()

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http" or not self.path:
            await self.app(scope, receive, send)
            return
        req_body = bytearray()
        resp: dict[str, Any] = {"status": None, "headers": [], "body": bytearray()}

        async def rx():
            message = await receive()
            if message["type"] == "http.request":
                req_body.extend(message.get("body", b""))
            return message

        async def tx(message):
            if message["type"] == "http.response.start":
                resp["status"] = message["status"]
                resp["headers"] = [(k.decode("latin1"), v.decode("latin1"))
                                   for k, v in message.get("headers", [])]
            elif message["type"] == "http.response.body":
                resp["body"].extend(message.get("body", b""))
            await send(message)

        started = time.time()
        try:
            await self.app(scope, rx, tx)
        finally:
            entry = {
                "t": started,
                "transport": "http",
                "method": scope["method"],
                "path": scope["path"],
                "query": scope.get("query_string", b"").decode("latin1"),
                "request_headers": [(k.decode("latin1"), v.decode("latin1"))
                                    for k, v in scope.get("headers", [])],
                "request_body": req_body.decode("utf-8", "replace"),
                "status": resp["status"],
                "response_headers": resp["headers"],
                "response_body": resp["body"].decode("utf-8", "replace"),
            }
            with self.lock:
                with open(self.path, "a", encoding="utf-8") as fh:
                    fh.write(json.dumps(entry) + "\n")


