"""Run OpenRouter's Chat Completions family through a real SchemaProvider.

Requires the current typed-provider neograph-engine wheel with LLM bindings.
A closed descriptor selects the origin and request paths. SchemaProvider uses
the native SDK's libcurl transport, typed requests, and immutable outcomes;
there is no OpenAIProvider adapter or Python-created prepared authority.

Run ``python via_openai_compat.py``. Hosted calls require OPENROUTER_API_KEY
and NG_ALLOW_HOSTED_CALLS=1. For a no-cost smoke, set
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 and OPENROUTER_MODEL=fixture-model.
Loopback calls use a dummy key. The example re-admits a full descriptor policy
with the loopback origin declared for OpenRouter routing; it does not disable
routing validation. OPENROUTER_CA_FILE may point to a local TLS peer's CA.

The peer accepts POST /v1/chat/completions with a system and user message,
model=fixture-model and provider={"zdr": true}. Return a standard buffered Chat
Completion with one assistant choice, finish_reason="stop", and optional usage.
Expected result: one real successful provider outcome, user+assistant typed
provider_messages, and the assistant text in graph state. Missing usage counts
remain None; HTTP/protocol failures are not printed as successful completions.
"""

import json
import os
import sys
from urllib.parse import urlsplit

import neograph_engine as ng

from via_http import MODEL, _endpoint_and_key, _load_env_if_present


def _descriptor(base_url):
    endpoint = urlsplit(base_url)
    origin = f"{endpoint.scheme}://{endpoint.netloc}"
    path = endpoint.path.rstrip("/") + "/chat/completions"
    policy = None
    if endpoint.hostname in {"127.0.0.1", "::1"}:
        # Declare the fixture origin as reviewed policy data, then pass the
        # entire policy through the SDK's closed policy loader.
        families = json.loads(ng.provider_policy_json())
        for family in families["families"]:
            if family["family"] == "openai.chat":
                family["openrouter_origins"].append(origin)
        policy = ng.load_provider_policy(json.dumps(families), ng.provider_codec_defaults_json())
    return ng.load_provider_descriptor(json.dumps({
        "descriptor_version": 1,
        "revision": 1,
        "id": "openrouter-chat-cookbook",
        "family": "openai.chat",
        "connection": {
            "base_url": origin,
            "paths": {"buffered": path, "streaming": path},
        },
    }), policy)


def main() -> int:
    _load_env_if_present()
    try:
        base_url, api_key = _endpoint_and_key()
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 2

    routing = ng.OpenRouterRouting()
    routing.zdr = True
    defaults = ng.SchemaProviderDefaults()
    defaults.provider = routing
    options = ng.ProviderRuntimeOptions(
        api_key=api_key,
        default_timeout_ms=120_000,
        ca_file=os.environ.get("OPENROUTER_CA_FILE", ""),
    )
    provider = ng.SchemaProvider(_descriptor(base_url), options, defaults)
    context = ng.NodeContext(
        provider=provider,
        model=os.environ.get("OPENROUTER_MODEL", MODEL),
        instructions="Be concise. One short sentence per answer.",
    )
    graph_def = {
        "name": "openrouter-compat",
        "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"answer": {"type": "llm_call"}},
        "edges": [
            {"from": ng.START_NODE, "to": "answer"},
            {"from": "answer", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(graph_def, context)
    user_q = "What's the capital of France?"
    print(f"[user] {user_q}")
    config = ng.RunConfig(thread_id="openrouter-compat")
    config.provider_messages = [ng.ProviderMessage(ng.ProviderRole.User, [ng.Text(user_q)])]
    result = engine.run(config)
    for outcome in result.provider_outcomes:
        if outcome.failure is not None:
            print(outcome.failure.error.safe_message, file=sys.stderr)
            return 1
        completion = outcome.completion
        print(f"[assistant] {outcome.text}")
        usage = completion.usage
        print("[usage] input=", usage.input_total.value if usage.input_total is not None else None,
              "output=", usage.output_total.value if usage.output_total is not None else None)
    return 0


if __name__ == "__main__":
    sys.exit(main())
