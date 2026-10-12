"""Shared configuration and text-only calls for the Python examples.

Offline by default. An example that calls a model builds its provider with
``example_provider(script)``: unless ``--live`` is given (or NG_EXAMPLE_LIVE=1, or
OPENAI_API_BASE / NG_PROVIDER_DESCRIPTOR names an endpoint), the replies come from
``script`` through ``_scripted.ScriptedModel``, a local server that speaks the real
protocol, so no key and no network are involved.

``--live`` uses ``schema_provider()``: SchemaProvider loads a closed descriptor before
constructing a runtime. Set OPENAI_API_BASE to a faithful local protocol peer to
exercise an example against your own endpoint; HTTPS peers also need
NG_EXAMPLE_CA_FILE when their CA is not in the system trust store. Hosted endpoints
require OPENAI_API_KEY, which is read from the process environment only.
"""

from __future__ import annotations

import contextlib
import json
import os
import sys
import urllib.parse
from collections import namedtuple
from pathlib import Path

import neograph_engine as ng
from neograph_engine.llm import SchemaProvider


for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")


def schema_provider(schema: str = "openai", *, http_version=None, base_url=None):
    """Construct the Chat or Responses runtime from an admitted descriptor.

    OPENAI_API_BASE is an origin (or gateway prefix), not a complete route.
    NG_PROVIDER_DESCRIPTOR can name a complete descriptor instead, including
    provider-specific admitted headers. No arbitrary request fields are added.
    ``base_url`` (used by the scripted offline model) takes precedence over both.
    """
    descriptor_file = None if base_url else os.getenv("NG_PROVIDER_DESCRIPTOR")
    if descriptor_file:
        source = Path(descriptor_file).read_text(encoding="utf-8")
        document = json.loads(source)
    else:
        family = {"openai": "openai.chat", "openai_responses": "openai.responses"}[schema]
        responses = family == "openai.responses"
        endpoint = urllib.parse.urlsplit(base_url or os.getenv("OPENAI_API_BASE", "https://api.openai.com"))
        origin = urllib.parse.urlunsplit((endpoint.scheme, endpoint.netloc, "", "", ""))
        prefix = endpoint.path.rstrip("/")
        route = "/v1/responses" if responses else "/v1/chat/completions"
        route = prefix + route
        document = {
            "descriptor_version": 1,
            "revision": 1,
            "id": "python-example-responses" if responses else "python-example-chat",
            "family": family,
            "connection": {
                "base_url": origin,
                "paths": {"buffered": route, "streaming": route},
            },
            "bindings": {
                "model": "model", "messages": "input" if responses else "messages",
                "stream": "stream",
                # OpenAI Chat rejects max_tokens for reasoning models (gpt-5.x, o-series);
                # max_completion_tokens is accepted by every current Chat model.
                "max_output_tokens": "max_output_tokens" if responses else "max_completion_tokens",
                "usage": ["usage"],
            },
            "stop_reasons": ({"completed": "EndTurn", "max_output_tokens": "MaxTokens",
                              "content_filter": "ContentFilter"}
                             if responses else {"stop": "EndTurn", "length": "MaxTokens",
                                                "tool_calls": "ToolUse",
                                                "content_filter": "ContentFilter"}),
        }
        source = json.dumps(document)
    hostname = urllib.parse.urlsplit(document["connection"]["base_url"]).hostname
    key = os.getenv("OPENAI_API_KEY", "")
    if hostname not in {"localhost", "127.0.0.1", "::1"} and not key:
        raise SystemExit("Hosted examples require OPENAI_API_KEY. For no-key verification, "
                         "set OPENAI_API_BASE to a local Chat/Responses protocol peer.")
    options = ng.ProviderRuntimeOptions(
        api_key=key,
        ca_file=os.getenv("NG_EXAMPLE_CA_FILE", ""),
        default_timeout_ms=int(os.getenv("NG_EXAMPLE_TIMEOUT_SECONDS", "180")) * 1000,
    )
    if http_version is not None:
        options.http_version = http_version
    return SchemaProvider(ng.load_provider_descriptor(source), options=options)


def live_requested(argv=None) -> bool:
    """True when the run should use a configured endpoint instead of the scripted model.

    ``--live`` or NG_EXAMPLE_LIVE=1 selects the hosted endpoint (OPENAI_API_KEY). An explicit
    OPENAI_API_BASE or NG_PROVIDER_DESCRIPTOR also selects the configured endpoint, so the
    protocol-peer workflow in the README keeps working.
    """
    argv = sys.argv if argv is None else argv
    return ("--live" in argv or os.getenv("NG_EXAMPLE_LIVE") in ("1", "true")
            or bool(os.getenv("OPENAI_API_BASE")) or bool(os.getenv("NG_PROVIDER_DESCRIPTOR")))


ChosenProvider = namedtuple("ChosenProvider", "provider scripted live")


@contextlib.contextmanager
def example_provider(script, schema: str = "openai"):
    """Yield ``ChosenProvider(provider, scripted, live)``.

    ``script`` is a list of ``_scripted`` replies (``say``, ``call_tool``, ``fail``) or a function
    ``(turn) -> Reply``. Offline, ``scripted`` is the running ``ScriptedModel`` (read
    ``scripted.call_count`` / ``scripted.turns`` for assertions); with ``--live`` it is None.
    ``schema`` is "openai" (Chat) or "openai_responses".
    """
    if live_requested():
        print("provider: live (OPENAI_API_KEY from the environment; the endpoint bills per request)")
        yield ChosenProvider(schema_provider(schema), None, True)
        return
    from _scripted import ScriptedModel
    print("provider: scripted (offline; pass --live to call a real model)")
    family = "responses" if schema == "openai_responses" else "chat"
    with ScriptedModel(script, family=family) as model:
        yield ChosenProvider(model.provider(), model, False)


def example_model() -> str:
    """Keep the explicit request/llm_call model selection in one place."""
    return os.getenv("OPENAI_MODEL", "gpt-4.1-mini")


def provider_messages(messages):
    """Translate text-only graph messages; rich provider parts stay typed."""
    roles = {"system": ng.ProviderRole.System, "developer": ng.ProviderRole.Developer,
             "user": ng.ProviderRole.User, "assistant": ng.ProviderRole.Assistant,
             "tool": ng.ProviderRole.Tool}
    result = []
    for message in messages:
        if isinstance(message, ng.ProviderMessage):
            result.append(message)
            continue
        role = message.get("role") if isinstance(message, dict) else message.role
        content = message.get("content", "") if isinstance(message, dict) else message.content
        if not isinstance(content, str):
            raise TypeError("ask_text accepts text messages; pass rich ProviderMessage parts explicitly")
        result.append(ng.ProviderMessage(role=roles[role], parts=[ng.Text(content)]))
    return result


def ask_text(provider, messages, *, temperature=None, max_output_tokens=None, model=None):
    """Invoke the typed provider and extract visible text, without inventing outcomes.

    These custom-node demos use only text. Tool-enabled graphs use llm_call,
    which keeps the complete provider messages/outcomes in the run evidence.
    """
    controls = ng.ProviderControls()
    if temperature is not None:
        controls.temperature = temperature
    controls.max_output_tokens = (max_output_tokens if max_output_tokens is not None
                                  else int(os.getenv("NG_EXAMPLE_MAX_TOKENS", "1600")))
    request = ng.make_provider_request(
        provider, model if model is not None else example_model(),
        provider_messages(messages), controls=controls,
    )
    outcome = provider.invoke(request)
    if outcome.failure is not None:
        raise RuntimeError(f"Provider request failed: {outcome.failure.error.safe_message}")
    completion = outcome.completion
    if completion is None:
        raise RuntimeError("Provider returned neither a completion nor a failure")
    text = "".join(part.value for message in completion.messages
                   for part in message.parts if isinstance(part, ng.Text))
    if not text.strip():
        raise RuntimeError("Provider completion contains no visible text; inspect typed outcome parts")
    return text
