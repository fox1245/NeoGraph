"""Map OpenRouter HTTP requests and replies inside a NeoGraph custom node.

Requires the current typed-provider neograph-engine release and ``httpx``.
This example keeps direct HTTP ownership: OpenRouterHttpNode sends JSON and
writes ordinary graph messages/usage. It does not fabricate ProviderOutcome
or SchemaProvider prepared-request authority.

Run ``python via_http.py``. Hosted calls require OPENROUTER_API_KEY and
NG_ALLOW_HOSTED_CALLS=1. For a no-cost smoke, set
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 and OPENROUTER_MODEL=fixture-model.
Loopback requests use a dummy credential, never the hosted key.

The peer accepts POST /v1/chat/completions with model, system/user messages,
temperature=0.7, and provider={"zdr": true}. Return JSON with one assistant
choice, text content, finish_reason="stop", and optional usage. The graph's
messages append channel contains the user then assistant; http_usage contains
the server usage dictionary or None. HTTP errors fail the node, not a success
shaped fallback. The httpx client is closed when the run finishes.
"""

import os
import sys
from pathlib import Path
from urllib.parse import urlsplit

import neograph_engine as ng


MODEL = "~deepseek/deepseek-v4-flash-latest"


def _load_env_if_present() -> None:
    for p in (Path(".env"), Path(__file__).parent / ".env",
              Path(__file__).resolve().parents[3] / ".env"):
        if p.exists():
            for line in p.read_text().splitlines():
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, value = line.partition("=")
                os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))
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


class OpenRouterHttpNode(ng.GraphNode):
    """Send a buffered chat request with the supplied httpx client."""

    def __init__(self, name, client, api_key, base_url, model=MODEL):
        super().__init__()
        self._name = name
        self.client = client
        self.api_key = api_key
        self.url = base_url + "/chat/completions"
        self.model = model

    def get_name(self):
        return self._name

    def run(self, input):
        messages = [{"role": "system", "content": "Be concise. One short sentence per answer."}]
        messages.extend(input.state.get("messages") or [])
        response = self.client.post(
            self.url,
            headers={"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json"},
            json={
                "model": self.model,
                "messages": messages,
                "temperature": 0.7,
                "provider": {"zdr": True},
            },
        )
        response.raise_for_status()
        body = response.json()
        message = body["choices"][0]["message"]
        if message.get("tool_calls"):
            raise RuntimeError("This text-only HTTP node cannot execute tool calls")
        return [
            ng.ChannelWrite("messages", [{"role": message.get("role", "assistant"), "content": message.get("content") or ""}]),
            ng.ChannelWrite("http_usage", body.get("usage")),
        ]


def main() -> int:
    _load_env_if_present()
    try:
        base_url, api_key = _endpoint_and_key()
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 2

    import httpx

    with httpx.Client(timeout=120) as client:
        node = OpenRouterHttpNode("answer", client, api_key, base_url,
                                  os.environ.get("OPENROUTER_MODEL", MODEL))
        registry = ng.GraphRegistry()
        registry.register_type("openrouter_http", lambda name, config, ctx: node)
        graph_def = {
            "name": "openrouter-http",
            "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
            "channels": {"messages": {"reducer": "append"}, "http_usage": {"reducer": "overwrite"}},
            "nodes": {"answer": {"type": "openrouter_http"}},
            "edges": [
                {"from": ng.START_NODE, "to": "answer"},
                {"from": "answer", "to": ng.END_NODE},
            ],
        }
        engine = ng.GraphEngine.compile(graph_def, ng.NodeContext(), registry=registry)
        user_q = "What's 17 * 23?"
        print(f"[user] {user_q}")
        result = engine.run(ng.RunConfig(
            thread_id="openrouter-http",
            input={"messages": [{"role": "user", "content": user_q}]},
        ))
        for message in result.output["channels"]["messages"]["value"]:
            if message.get("content"):
                print(f"  {message['role']:>9}: {message['content']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
