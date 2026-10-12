"""Opt-in live checks of the typed provider path against hosted models.

Nothing here runs by default. Set NEOGRAPH_E2E_LIVE=1 and the key variable of each provider you want to exercise
(OPENAI_API_KEY, GEMINI_API_KEY, ANTHROPIC_API_KEY, OPENROUTER_API_KEY); a provider without its key is skipped. Keys are
read from the environment only. A model can be changed with NG_LIVE_<PROVIDER>_MODEL (OPENAI, GEMINI, ANTHROPIC,
OPENROUTER). Every call uses a small output cap; a full run costs well under one US cent.

Covered through SchemaProvider: a plain completion with usage, a stream whose deltas equal the final text, a tool round
trip, an invalid key (typed Authentication failure, no key material in the failure), a stream cancelled mid-flight, and a
deadline that has already passed.
"""

import json
import os
import threading
import time

import pytest

import neograph_engine as ng
from neograph_engine.llm import SchemaProvider

LIVE = os.environ.get("NEOGRAPH_E2E_LIVE") == "1"

# provider -> (key variable, SDK family, origin, buffered path, streaming path, default model)
ROUTES = {
    "openai": ("OPENAI_API_KEY", "openai.chat", "https://api.openai.com", "/v1/chat/completions",
               "/v1/chat/completions", "gpt-6-luna"),
    "gemini": ("GEMINI_API_KEY", "google.generate", "https://generativelanguage.googleapis.com",
               "/v1beta/models/{model}:generateContent", "/v1beta/models/{model}:streamGenerateContent?alt=sse",
               "gemini-3.8-flash"),
    "anthropic": ("ANTHROPIC_API_KEY", "anthropic.messages", "https://api.anthropic.com", "/v1/messages",
                  "/v1/messages", "claude-haiku-5-5"),
    "openrouter": ("OPENROUTER_API_KEY", "openai.chat", "https://openrouter.ai", "/api/v1/chat/completions",
                   "/api/v1/chat/completions", "z-ai/glm-5.3-flash"),
}
DUMMY_KEY = "dummy-invalid-credential-0123456789"


def _model(name):
    return os.environ.get(f"NG_LIVE_{name.upper()}_MODEL", ROUTES[name][5])


def _provider(name, api_key=None, timeout_ms=90_000):
    key_variable, family, origin, buffered, streaming, _default = ROUTES[name]
    model = _model(name)
    document = {
        "descriptor_version": 1, "revision": 1, "id": f"live-{name}", "family": family,
        "connection": {"base_url": origin,
                       "paths": {"buffered": buffered.format(model=model), "streaming": streaming.format(model=model)}},
    }
    if name == "openai":
        # reasoning models on Chat Completions take max_completion_tokens, not max_tokens
        document["bindings"] = {"model": "model", "messages": "messages", "stream": "stream",
                                "max_output_tokens": "max_completion_tokens", "usage": ["usage"]}
        document["stop_reasons"] = {"stop": "EndTurn", "length": "MaxTokens", "tool_calls": "ToolUse",
                                    "content_filter": "ContentFilter"}
    defaults = ng.SchemaProviderDefaults()
    if name == "openrouter":
        routing = ng.OpenRouterRouting()
        routing.zdr = True
        defaults.provider = routing
    options = ng.ProviderRuntimeOptions(
        api_key=os.environ.get(key_variable, "") if api_key is None else api_key, default_timeout_ms=timeout_ms)
    return SchemaProvider(ng.load_provider_descriptor(json.dumps(document)), options, defaults), model


def _controls(cap=1024):
    controls = ng.ProviderControls()
    controls.max_output_tokens = cap
    return controls


def _user(text):
    return ng.ProviderMessage(role=ng.ProviderRole.User, parts=[ng.Text(text)])


def _request(provider, model, messages, **kw):
    return ng.make_provider_request(provider, model, messages, controls=kw.pop("controls", _controls()), **kw)


def _count(value):
    return None if value is None else value.value


def _route(name):
    key_variable = ROUTES[name][0]
    return pytest.param(name, marks=[
        pytest.mark.skipif(not LIVE, reason="live: set NEOGRAPH_E2E_LIVE=1"),
        pytest.mark.skipif(not os.environ.get(key_variable), reason=f"live: set {key_variable}"),
    ], id=name)


ALL_ROUTES = [_route(name) for name in ROUTES]


@pytest.mark.parametrize("name", ALL_ROUTES)
def test_live_plain_completion_reports_usage(name):
    provider, model = _provider(name)
    outcome = provider.invoke(_request(provider, model, [_user("What is the capital of France? One word.")]))
    assert outcome.failure is None, outcome.failure.error.kind
    assert "paris" in outcome.text.lower()
    usage = outcome.completion.usage
    assert _count(usage.input_total) and _count(usage.output_total)
    assert str(outcome.completion.stop.kind).endswith("EndTurn")


@pytest.mark.parametrize("name", ALL_ROUTES)
def test_live_stream_deltas_equal_the_final_text(name):
    provider, model = _provider(name)
    events = []
    request = _request(provider, model, [_user("Write the numbers from 1 to 12 in English words, comma separated.")],
                       mode=ng.ProviderMode.Stream)
    request.on_event = events.append
    outcome = provider.invoke(request)
    assert outcome.failure is None, outcome.failure.error.kind
    joined = "".join(e.value.bytes for e in events if e.kind == "PartDelta"
                     and str(e.value.kind).endswith("Text") and str(e.value.channel).endswith("Content"))
    final = "".join(p.value for m in outcome.completion.messages for p in m.parts if isinstance(p, ng.Text))
    assert joined == final and final
    assert any(e.kind == "UsageUpdate" for e in events)
    assert _count(outcome.completion.usage.output_total)


@pytest.mark.parametrize("name", ALL_ROUTES)
def test_live_tool_round_trip(name):
    provider, model = _provider(name)
    tool = ng.ChatTool(name="get_weather", description="Get the current weather of a city. Returns JSON.",
                       parameters={"type": "object", "properties": {"city": {"type": "string"}}, "required": ["city"]})
    controls = _controls()
    if name == "openai":
        controls.reasoning_effort = "none"  # Chat Completions refuses function tools together with reasoning
    first = _request(provider, model, [_user("What is the weather in Seoul? Call get_weather, then give the temperature "
                                             "in Celsius.")], tools=[tool], controls=controls)
    turn = provider.invoke(first)
    assert turn.failure is None, turn.failure.error.kind
    calls = [p for m in turn.completion.messages for p in m.parts if isinstance(p, ng.ProviderToolCall)]
    assert calls and calls[0].name == "get_weather"
    result = ng.ProviderMessage(role=ng.ProviderRole.Tool,
                                parts=[ng.ProviderToolResult(calls[0].id, json.dumps({"temp_c": 17.9}))])
    second = _request(provider, model, list(first.messages) + list(turn.completion.messages) + [result],
                      tools=[tool], controls=controls)
    final = provider.invoke(second)
    assert final.failure is None, final.failure.error.kind
    assert "17.9" in final.text


@pytest.mark.parametrize("name", ALL_ROUTES)
def test_live_invalid_key_is_a_typed_authentication_failure_without_key_material(name):
    provider, model = _provider(name, api_key=DUMMY_KEY)
    outcome = provider.invoke(_request(provider, model, [_user("hi")], controls=_controls(16)))
    assert outcome.completion is None
    error = outcome.failure.error
    assert error.kind == ng.ProviderErrorKind.Authentication
    assert DUMMY_KEY not in repr((error.safe_message, error.vendor_code))
    for variable in ("OPENAI_API_KEY", "GEMINI_API_KEY", "ANTHROPIC_API_KEY", "OPENROUTER_API_KEY"):
        secret = os.environ.get(variable)
        if secret:
            assert secret not in repr((error.safe_message, error.vendor_code))


@pytest.mark.parametrize("name", ALL_ROUTES)
def test_live_cancel_in_the_middle_of_a_stream_returns_promptly(name):
    provider, model = _provider(name)
    token = ng.CancelToken()
    seen = {"deltas": 0}
    first = threading.Event()

    def on_event(event):
        if event.kind == "PartDelta" and event.value.bytes:
            seen["deltas"] += 1
            if seen["deltas"] >= 3:
                first.set()

    request = _request(provider, model, [_user("Write a long story, at least 1500 words, about a lighthouse keeper. "
                                               "Start writing the story immediately.")],
                       controls=_controls(3000), mode=ng.ProviderMode.Stream)
    request.on_event = on_event
    request.cancel_token = token
    marks = {}

    def canceller():
        first.wait(10)  # a model that thinks silently first is cancelled anyway
        time.sleep(0.2)
        marks["at"] = time.monotonic()
        token.cancel()

    thread = threading.Thread(target=canceller, daemon=True)
    thread.start()
    outcome = provider.invoke(request)
    returned = time.monotonic()
    thread.join(5)
    assert outcome.completion is None
    assert outcome.failure.error.kind == ng.ProviderErrorKind.Cancelled
    assert returned - marks["at"] < 3.0


@pytest.mark.parametrize("name", ALL_ROUTES)
def test_live_expired_deadline_is_deadline_exceeded(name):
    provider, model = _provider(name)
    request = _request(provider, model, [_user("hi")], controls=_controls(16))
    request.timeout_ms = 30
    outcome = provider.invoke(request)
    assert outcome.completion is None
    assert outcome.failure.error.kind == ng.ProviderErrorKind.DeadlineExceeded
