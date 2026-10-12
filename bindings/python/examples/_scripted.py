"""Scripted offline model for the Python examples.

A small local HTTP server that speaks the OpenAI Chat (``/v1/chat/completions``)
and Responses (``/v1/responses``) protocols and answers from a script written in
the example. The example then talks to it through the real ``SchemaProvider``,
so request building, SSE parsing, tool calls and outcome handling are the SDK's
own code; only the model is replaced. No API key, no network beyond 127.0.0.1.

Not a model: nothing here understands the prompt. The script decides.

    from _scripted import ScriptedModel, say, call_tool

    with ScriptedModel([
        call_tool("calc", expression="2 + 3"),   # call 1: the model asks for a tool
        say("The answer is 5."),                 # call 2: the final answer
    ]) as model:
        provider = model.provider()              # a SchemaProvider pointing at the local server
        ...
        assert model.call_count == 2

Most examples use ``_common.example_provider(script)`` instead, which returns this
provider unless ``--live`` is given (see ``_common.py``).

Rules:
  * A sequence script is consumed in call order. With parallel provider calls the
    order is not defined; pass a function ``(turn) -> Reply`` and key on
    ``turn.last_user_text`` instead.
  * When a sequence runs out the server answers HTTP 500 with a message naming the
    call number, so an unexpected extra call fails loudly.
  * ``family="chat"`` (default) or ``"responses"`` selects the protocol served.
    Streaming requests get SSE with one text delta per word.
  * ``with_usage()`` reports token counts; otherwise the reply carries no usage.
"""

from __future__ import annotations

import json
import threading
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Callable, Optional


@dataclass
class Reply:
    """What the scripted model answers on one call."""

    text: str = ""
    tool_calls: list = field(default_factory=list)   # [(id, name, arguments dict)]
    http_status: int = 200                           # not 200: the call fails with this status
    error_message: str = "scripted failure"
    usage: Optional[tuple] = None                    # (input_tokens, output_tokens)

    def with_text(self, text: str) -> "Reply":
        return Reply(text, list(self.tool_calls), self.http_status, self.error_message, self.usage)

    def with_call(self, name: str, id: str = "", **arguments) -> "Reply":
        calls = list(self.tool_calls) + [(id, name, arguments)]
        return Reply(self.text, calls, self.http_status, self.error_message, self.usage)

    def with_usage(self, input_tokens: int, output_tokens: int) -> "Reply":
        return Reply(self.text, list(self.tool_calls), self.http_status, self.error_message,
                     (input_tokens, output_tokens))


def say(text: str) -> Reply:
    """A plain text answer."""
    return Reply(text=text)


def call_tool(name: str, id: str = "", **arguments) -> Reply:
    """A reply that asks the caller to run a tool. Chain ``.with_call()`` for more."""
    return Reply(tool_calls=[(id, name, arguments)])


def fail(http_status: int = 500, message: str = "scripted failure") -> Reply:
    """A reply that fails with an HTTP error (the SDK reports a typed provider failure)."""
    return Reply(http_status=http_status, error_message=message)


@dataclass
class Turn:
    """Read-only view of the request the scripted model is answering."""

    index: int            # zero-based call number
    streaming: bool
    messages: list        # [{"role", "text", "tool_calls", "tool_call_id"}], oldest first

    @property
    def system_text(self) -> str:
        return "".join(m["text"] for m in self.messages if m["role"] in ("system", "developer"))

    @property
    def last_user_text(self) -> str:
        for message in reversed(self.messages):
            if message["role"] == "user":
                return message["text"]
        return ""

    @property
    def tool_result_count(self) -> int:
        return sum(1 for m in self.messages if m["role"] == "tool")

    def has_tool_result(self, tool_call_id: Optional[str] = None) -> bool:
        return any(m["role"] == "tool" and tool_call_id in (None, m["tool_call_id"])
                   for m in self.messages)

    @property
    def last_tool_result(self) -> str:
        results = [m["text"] for m in self.messages if m["role"] == "tool"]
        return results[-1] if results else ""


def _text(content) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "".join(part.get("text", "") for part in content if isinstance(part, dict))
    return ""


def _normalize_chat(body: dict) -> list:
    messages = []
    for message in body.get("messages", []):
        calls = [call.get("function", {}).get("name", "") for call in message.get("tool_calls") or []]
        messages.append({"role": message.get("role", ""), "text": _text(message.get("content")),
                         "tool_calls": calls, "tool_call_id": message.get("tool_call_id", "")})
    return messages


def _normalize_responses(body: dict) -> list:
    items = body.get("input", [])
    if isinstance(items, str):
        items = [{"role": "user", "content": items}]
    messages = []
    for item in items:
        kind = item.get("type", "message")
        if kind == "function_call_output":
            messages.append({"role": "tool", "text": _text(item.get("output")),
                             "tool_calls": [], "tool_call_id": item.get("call_id", "")})
        elif kind == "function_call":
            messages.append({"role": "assistant", "text": "", "tool_calls": [item.get("name", "")],
                             "tool_call_id": ""})
        else:
            messages.append({"role": item.get("role", ""), "text": _text(item.get("content")),
                             "tool_calls": [], "tool_call_id": ""})
    instructions = body.get("instructions")
    if instructions:
        messages.insert(0, {"role": "system", "text": str(instructions), "tool_calls": [],
                            "tool_call_id": ""})
    return messages


def _words(text: str):
    """Split into word chunks that concatenate back to the original text."""
    at = 0
    while at < len(text):
        space = text.find(" ", at)
        end = len(text) if space < 0 else space + 1
        yield text[at:end]
        at = end


class ScriptedModel:
    """A local OpenAI-protocol server driven by a script. Use as a context manager."""

    def __init__(self, script, *, family: str = "chat", model_name: str = "scripted-model"):
        if family not in ("chat", "responses"):
            raise ValueError("family must be 'chat' or 'responses'")
        self._responder: Optional[Callable[[Turn], Reply]] = script if callable(script) else None
        self._script = [] if callable(script) else list(script)
        self.family = family
        self.model_name = model_name
        self.requests: list = []     # decoded request bodies, in arrival order
        self.turns: list = []        # Turn objects, in arrival order
        self._lock = threading.Lock()
        self._next_id = 0
        self._server: Optional[ThreadingHTTPServer] = None
        self._thread: Optional[threading.Thread] = None

    # ---- lifecycle ----
    def start(self) -> "ScriptedModel":
        outer = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args):
                pass

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(length) or b"{}")
                status, content_type, payload = outer._answer(self.path, body)
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                try:
                    self.wfile.write(payload)
                except (BrokenPipeError, ConnectionResetError):
                    pass  # The client gave up (for example it was cancelled).

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._server.daemon_threads = True
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()
        return self

    def stop(self) -> None:
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
            self._thread.join()
            self._server = None

    def __enter__(self) -> "ScriptedModel":
        return self.start()

    def __exit__(self, *_exc) -> None:
        self.stop()

    @property
    def base_url(self) -> str:
        return f"http://127.0.0.1:{self._server.server_port}"

    def provider(self):
        """A real ``SchemaProvider`` that talks to this server."""
        from _common import schema_provider
        return schema_provider("openai_responses" if self.family == "responses" else "openai",
                               base_url=self.base_url)

    # ---- what the example asserts on ----
    @property
    def call_count(self) -> int:
        return len(self.turns)

    # ---- request handling ----
    def _tool_id(self) -> str:
        self._next_id += 1
        return f"call_{self._next_id:03d}"

    def _answer(self, path: str, body: dict):
        expected = "/v1/responses" if self.family == "responses" else "/v1/chat/completions"
        if path != expected:
            return self._error(404, f"scripted {self.family} server has no route {path}")
        streaming = bool(body.get("stream"))
        messages = _normalize_responses(body) if self.family == "responses" else _normalize_chat(body)
        with self._lock:
            index = len(self.turns)
            turn = Turn(index, streaming, messages)
            self.requests.append(body)
            self.turns.append(turn)
        try:
            if self._responder is not None:
                reply = self._responder(turn)
            elif index < len(self._script):
                reply = self._script[index]
            else:
                reply = fail(500, f"scripted model has no reply for call {index + 1} "
                                  f"(script holds {len(self._script)})")
        except Exception as error:  # a broken responder is an example bug: say so on the wire
            reply = fail(500, f"scripted responder raised {type(error).__name__}: {error}")
        if reply.http_status != 200:
            return self._error(reply.http_status, reply.error_message)
        with self._lock:
            calls = [(call_id or self._tool_id(), name, arguments)
                     for call_id, name, arguments in reply.tool_calls]
        if self.family == "responses":
            return self._responses(body, reply, calls, streaming)
        return self._chat(body, reply, calls, streaming)

    @staticmethod
    def _error(status: int, message: str):
        error = {"message": message, "type": "scripted_error"}
        if status == 429:
            error.update(type="rate_limit_error", code="rate_limit_exceeded")
        payload = json.dumps({"error": error}).encode()
        return status, "application/json", payload

    # ---- Chat Completions ----
    def _chat(self, body, reply, calls, streaming):
        finish = "tool_calls" if calls else "stop"
        usage = None
        if reply.usage:
            usage = {"prompt_tokens": reply.usage[0], "completion_tokens": reply.usage[1],
                     "total_tokens": sum(reply.usage)}
        model = body.get("model", self.model_name)
        if not streaming:
            message = {"role": "assistant", "content": reply.text or None}
            if calls:
                message["tool_calls"] = [
                    {"id": call_id, "type": "function",
                     "function": {"name": name, "arguments": json.dumps(arguments)}}
                    for call_id, name, arguments in calls]
            document = {"id": "scripted", "object": "chat.completion", "created": 1, "model": model,
                        "choices": [{"index": 0, "message": message, "finish_reason": finish}]}
            if usage:
                document["usage"] = usage
            return 200, "application/json", json.dumps(document).encode()

        def chunk(delta, finish_reason=None, **extra):
            return {"id": "scripted", "object": "chat.completion.chunk", "created": 1, "model": model,
                    "choices": [{"index": 0, "delta": delta, "finish_reason": finish_reason}], **extra}

        frames = [chunk({"role": "assistant"})]
        frames += [chunk({"content": word}) for word in _words(reply.text)]
        for position, (call_id, name, arguments) in enumerate(calls):
            frames.append(chunk({"tool_calls": [{"index": position, "id": call_id, "type": "function",
                                                 "function": {"name": name, "arguments": ""}}]}))
            frames.append(chunk({"tool_calls": [{"index": position,
                                                 "function": {"arguments": json.dumps(arguments)}}]}))
        frames.append(chunk({}, finish))
        if usage:
            frames.append({"id": "scripted", "object": "chat.completion.chunk", "created": 1,
                           "model": model, "choices": [], "usage": usage})
        payload = "".join("data: " + json.dumps(frame) + "\n\n" for frame in frames) + "data: [DONE]\n\n"
        return 200, "text/event-stream", payload.encode()

    # ---- Responses ----
    def _responses(self, body, reply, calls, streaming):
        output = []
        if reply.text:
            output.append({"id": "msg_scripted", "type": "message", "status": "completed",
                           "role": "assistant",
                           "content": [{"type": "output_text", "text": reply.text, "annotations": []}]})
        for call_id, name, arguments in calls:
            output.append({"id": "fc_" + call_id, "type": "function_call", "status": "completed",
                           "call_id": call_id, "name": name, "arguments": json.dumps(arguments)})
        usage = None
        if reply.usage:
            usage = {"input_tokens": reply.usage[0], "output_tokens": reply.usage[1],
                     "total_tokens": sum(reply.usage)}

        def envelope(items, status="completed"):
            return {"id": "resp_scripted", "object": "response", "created_at": 1,
                    "model": body.get("model", self.model_name), "status": status, "output": items,
                    "usage": usage if status != "in_progress" else None,
                    "incomplete_details": None, "error": None}

        if not streaming:
            return 200, "application/json", json.dumps(envelope(output)).encode()

        frames = []

        def frame(kind, **fields):
            payload = {"type": kind, "sequence_number": len(frames), **fields}
            frames.append(f"event: {kind}\ndata: {json.dumps(payload)}\n\n")

        frame("response.created", response=envelope([], "in_progress"))
        for index, item in enumerate(output):
            if item["type"] == "message":
                initial = {**item, "status": "in_progress", "content": []}
            else:
                initial = {**item, "status": "in_progress", "arguments": ""}
            frame("response.output_item.added", output_index=index, item=initial)
            owner = {"item_id": item["id"], "output_index": index}
            if item["type"] == "message":
                part = item["content"][0]
                part_owner = {**owner, "content_index": 0}
                frame("response.content_part.added", **part_owner, part={**part, "text": ""})
                for word in _words(part["text"]):
                    frame("response.output_text.delta", **part_owner, delta=word)
                frame("response.output_text.done", **part_owner, text=part["text"])
                frame("response.content_part.done", **part_owner, part=part)
            else:
                frame("response.function_call_arguments.delta", **owner, delta=item["arguments"])
                frame("response.function_call_arguments.done", **owner, arguments=item["arguments"])
            frame("response.output_item.done", output_index=index, item=item)
        frame("response.completed", response=envelope(output))
        return 200, "text/event-stream", "".join(frames).encode()
