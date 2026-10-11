"""Scenario 8: a Program whose graph calls a model through OpenRouter.

Default (offline): the identical code path runs against a loopback Chat
Completions peer started by the test, so the wiring (descriptor, ZDR routing,
output cap, typed outcome, usage) is verified with no key and no paid call.

Live (opt-in): NEOGRAPH_E2E_LIVE=1 and OPENROUTER_API_KEY=... run the same
Program against https://openrouter.ai with ~deepseek/deepseek-v4-flash-latest
(override with OPENROUTER_MODEL), max_output_tokens=64 and provider.zdr=true.
The key is read from the environment only.

The model call lives in a Python node, not the built-in `llm_call` node:
Python cannot bind a Provider into a Program yet (#366), see
test_03_failures.py.
"""

import http.server
import json
import os
import threading
from urllib.parse import urlsplit

import pytest

import neograph_engine as ng

from .support import admit, budget, ceiling, describe, digest, make_host

LIVE = os.environ.get("NEOGRAPH_E2E_LIVE") == "1" and bool(os.environ.get("OPENROUTER_API_KEY"))
MODEL = os.environ.get("OPENROUTER_MODEL", "~deepseek/deepseek-v4-flash-latest")
MAX_OUTPUT_TOKENS = 64

needs_llm = pytest.mark.skipif(not getattr(ng, "_HAVE_LLM", False),
                               reason="neograph_engine was built without the LLM target")


def openrouter_provider(base_url, api_key):
    """SchemaProvider for OpenRouter Chat Completions, as in
    examples/cookbook/openrouter-provider/via_openai_compat.py."""
    from neograph_engine.llm import SchemaProvider

    endpoint = urlsplit(base_url.rstrip("/"))
    origin = f"{endpoint.scheme}://{endpoint.netloc}"
    path = endpoint.path.rstrip("/") + "/chat/completions"
    policy = None
    if endpoint.hostname in {"127.0.0.1", "::1"}:
        # Declare the loopback peer as an OpenRouter origin through the closed
        # policy loader, exactly like the cookbook's no-cost smoke.
        families = json.loads(ng.provider_policy_json())
        for family in families["families"]:
            if family["family"] == "openai.chat":
                family["openrouter_origins"].append(origin)
        policy = ng.load_provider_policy(json.dumps(families), ng.provider_codec_defaults_json())
    descriptor = ng.load_provider_descriptor(json.dumps({
        "descriptor_version": 1,
        "revision": 1,
        "id": "e2e-openrouter-chat",
        "family": "openai.chat",
        "connection": {"base_url": origin, "paths": {"buffered": path, "streaming": path}},
    }), policy)
    routing = ng.OpenRouterRouting()
    routing.zdr = True
    defaults = ng.SchemaProviderDefaults()
    defaults.provider = routing
    options = ng.ProviderRuntimeOptions(api_key=api_key, default_timeout_ms=120_000)
    return SchemaProvider(descriptor, options, defaults), routing


class ModelAnswer(ng.GraphNode):
    """Python node that owns one typed provider call and records its usage."""

    def __init__(self, name, provider, model, routing):
        super().__init__()
        self._name = name
        self._provider = provider
        self._model = model
        self._routing = routing

    def get_name(self):
        return self._name

    def run(self, input):
        controls = ng.ProviderControls()
        controls.max_output_tokens = MAX_OUTPUT_TOKENS
        controls.provider = self._routing
        messages = [
            ng.ProviderMessage(role=ng.ProviderRole.System,
                               parts=[ng.Text("Answer in one short sentence.")]),
            ng.ProviderMessage(role=ng.ProviderRole.User,
                               parts=[ng.Text(input.state.get("question") or "")]),
        ]
        request = ng.make_provider_request(self._provider, self._model, messages, controls=controls)
        request.cancel_token = input.ctx.cancel_token
        outcome = self._provider.invoke(request)
        if outcome.failure is not None:
            raise RuntimeError(f"provider failed: {outcome.failure.error.safe_message}")
        completion = outcome.completion
        usage = completion.usage

        def count(value):
            return None if value is None else value.value

        return [
            ng.ChannelWrite("reply", outcome.text),
            ng.ChannelWrite("usage", {"input": count(usage.input_total),
                                      "output": count(usage.output_total)}),
        ]


CHAT_PROGRAM = r"""
export function define() {
  return ng.graph("chat")
    .channel("question", {reducer: "overwrite", initial: ""})
    .channel("reply", {reducer: "overwrite", initial: ""})
    .channel("usage", {reducer: "overwrite", initial: null})
    .node("answer", {type: "e2e.model_answer"})
    .entry("answer")
    .exit("answer");
}

export function* main(input) {
  const turn = yield ng.callCore("chat", {question: input.question}, "chat:answer");
  return {text: turn.reply, usage: turn.usage};
}
"""


def run_chat_program(provider, routing, question):
    ng.NodeFactory.register_type(
        "e2e.model_answer", lambda name, config, ctx: ModelAnswer(name, provider, MODEL, routing))
    builder = ng.ProgramRegistryBuilder()
    builder.add_registered_node("e2e.model_answer", "1.0.0", digest("node:e2e.model_answer"))
    builder.add_registered_reducer("overwrite", "1.0.0", digest("reducer:overwrite"))
    host = make_host("tenant:llm", registry=builder.build(),
                     budget_ceiling=ceiling(wall_time_ms=180_000), scheduler_threads=2)
    run_budget = budget(wall_time_ms=150_000, max_core_steps=5, max_program_operations=4)
    version = admit(host, CHAT_PROGRAM, run_budget, "chat.js")
    return host.run(version, {"question": question}, run_budget)


class _ChatPeer(http.server.BaseHTTPRequestHandler):
    requests = []

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
        type(self).requests.append({"path": self.path, "body": body,
                                    "auth": self.headers.get("Authorization")})
        reply = json.dumps({
            "id": "chatcmpl-e2e", "object": "chat.completion", "created": 1,
            "model": body.get("model"),
            "choices": [{"index": 0, "finish_reason": "stop",
                         "message": {"role": "assistant", "content": "Paris."}}],
            "usage": {"prompt_tokens": 21, "completion_tokens": 3, "total_tokens": 24},
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)

    def log_message(self, *args):
        pass


@pytest.fixture
def chat_peer():
    _ChatPeer.requests = []
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _ChatPeer)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_address[1]}/api/v1", _ChatPeer.requests
    finally:
        server.shutdown()
        server.server_close()


@needs_llm
def test_program_model_call_wiring_against_a_loopback_peer(chat_peer):
    base_url, requests = chat_peer
    provider, routing = openrouter_provider(base_url, "local-smoke")

    result = run_chat_program(provider, routing, "What is the capital of France?")

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.output["text"] == "Paris."
    assert result.output["usage"] == {"input": 21, "output": 3}
    assert result.execution_trace == ["answer"]
    [sent] = requests
    assert sent["path"] == "/api/v1/chat/completions"
    assert sent["auth"] == "Bearer local-smoke"
    body = sent["body"]
    assert body["model"] == MODEL
    assert body.get("provider", {}).get("zdr") is True, body
    caps = [body[key] for key in ("max_tokens", "max_completion_tokens", "max_output_tokens") if key in body]
    assert caps == [MAX_OUTPUT_TOKENS], body
    assert [m["role"] for m in body["messages"]] == ["system", "user"]


@needs_llm
@pytest.mark.live
@pytest.mark.skipif(not LIVE, reason="live: set NEOGRAPH_E2E_LIVE=1 and OPENROUTER_API_KEY")
def test_live_openrouter_model_call_inside_a_program():
    base_url = os.environ.get("OPENROUTER_BASE_URL", "https://openrouter.ai/api/v1")
    provider, routing = openrouter_provider(base_url, os.environ["OPENROUTER_API_KEY"])

    result = run_chat_program(provider, routing, "What is the capital of France? One word.")

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.output["text"].strip(), result.output
    usage = result.output["usage"]
    assert usage["input"] and usage["input"] > 0, usage
    assert usage["output"] and 0 < usage["output"] <= MAX_OUTPUT_TOKENS, usage
