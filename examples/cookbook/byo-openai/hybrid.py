"""Run an official OpenAI SDK client inside a NeoGraph custom node.

Requires the current typed-provider neograph-engine release and ``openai``.
The SDK owns HTTP, retries, and client configuration; the node writes ordinary
message/usage channel data, not SchemaProvider outcomes or native replay state.

Run ``python hybrid.py`` with OPENROUTER_API_KEY and NG_ALLOW_HOSTED_CALLS=1
only when you intend to pay for a hosted call. Without that opt-in, hosted
endpoints are rejected before any request. For a no-cost protocol smoke, set
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 and OPENROUTER_MODEL=fixture-model;
loopback requests use a dummy credential, never the hosted key.

The peer must accept POST /v1/chat/completions with model, system/user messages,
temperature=0.7, and provider={"zdr": true}. Return a standard buffered Chat
Completion with one assistant choice, finish_reason="stop", and optional usage.
The messages append channel ends with the original user and assistant reply;
sdk_usage is the returned usage dictionary, or None when usage is absent.
"""

import os
import sys
from pathlib import Path
from urllib.parse import urlsplit

import neograph_engine as ng
from openai import OpenAI


MODEL = "~deepseek/deepseek-v4-flash-latest"


def _load_env_if_present():
    for p in (Path(".env"), Path(__file__).parent / ".env",
              Path(__file__).resolve().parents[3] / ".env"):
        if p.exists():
            for line in p.read_text().splitlines():
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, _, v = line.partition("=")
                os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))
            return


def _endpoint_and_key():
    base_url = os.environ.get("OPENROUTER_BASE_URL", "https://openrouter.ai/api/v1").rstrip("/")
    endpoint = urlsplit(base_url)
    local = endpoint.hostname in {"127.0.0.1", "::1"}
    if endpoint.scheme not in {"http", "https"} or not endpoint.hostname:
        raise ValueError("OPENROUTER_BASE_URL must be an HTTP(S) API base URL")
    if endpoint.username or endpoint.password or endpoint.query or endpoint.fragment:
        raise ValueError("OPENROUTER_BASE_URL must not contain credentials, query, or fragment")
    if local:
        return base_url, "local-smoke"
    if os.environ.get("NG_ALLOW_HOSTED_CALLS") != "1":
        raise ValueError("Hosted calls require NG_ALLOW_HOSTED_CALLS=1; use a loopback peer for smoke")
    if endpoint.scheme != "https":
        raise ValueError("Hosted calls require HTTPS")
    key = os.environ.get("OPENROUTER_API_KEY")
    if not key:
        raise ValueError("OPENROUTER_API_KEY not set")
    return base_url, key


class OpenAISdkNode(ng.GraphNode):
    """Call the supplied SDK client and append its text reply to graph state."""

    def __init__(self, name, client, model):
        super().__init__()
        self._name = name
        self.client = client
        self.model = model
        self.calls = 0

    def get_name(self):
        return self._name

    def run(self, input):
        messages = [{"role": "system", "content": "You are a concise assistant. Each turn must fit in 1-2 sentences."}]
        messages.extend(input.state.get("messages") or [])
        self.calls += 1
        print(f"[sdk] call #{self.calls} ({len(messages)} msgs)", file=sys.stderr)
        response = self.client.chat.completions.create(
            model=self.model,
            messages=messages,
            temperature=0.7,
            extra_body={"provider": {"zdr": True}},
        )
        message = response.choices[0].message
        if message.tool_calls:
            raise RuntimeError("This text-only node cannot execute tool calls; use hybrid_with_tools.py")
        return [
            ng.ChannelWrite("messages", [{"role": "assistant", "content": message.content or ""}]),
            ng.ChannelWrite("sdk_usage", response.usage.model_dump() if response.usage else None),
        ]


def main() -> int:
    _load_env_if_present()
    try:
        base_url, api_key = _endpoint_and_key()
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 2

    with OpenAI(api_key=api_key, base_url=base_url, timeout=120) as client:
        node = OpenAISdkNode("answer", client, os.environ.get("OPENROUTER_MODEL", MODEL))
        registry = ng.GraphRegistry()
        registry.register_type("openai_sdk", lambda name, config, ctx: node)
        graph_def = {
            "name": "byo-openai-demo",
            "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
            "channels": {"messages": {"reducer": "append"}, "sdk_usage": {"reducer": "overwrite"}},
            "nodes": {"answer": {"type": "openai_sdk"}},
            "edges": [
                {"from": ng.START_NODE, "to": "answer"},
                {"from": "answer", "to": ng.END_NODE},
            ],
        }
        engine = ng.GraphEngine.compile(graph_def, ng.NodeContext(), registry=registry)
        result = engine.run(ng.RunConfig(
            thread_id="hybrid-demo",
            input={"messages": [{"role": "user", "content": "How do I make my Python script run a graph through an LLM call?"}]},
        ))
        for message in result.output["channels"]["messages"]["value"]:
            if message.get("content"):
                print(f"  {message['role']:>9}: {message['content']}")
        print(f"[hybrid] {node.calls} call through the official OpenAI SDK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
