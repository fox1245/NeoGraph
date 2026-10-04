"""Shared configuration and text-only calls for the Python examples.

SchemaProvider loads a closed descriptor before constructing a runtime. Set
OPENAI_API_BASE to a faithful local protocol peer to exercise these examples
without a hosted key; HTTPS peers also need NG_EXAMPLE_CA_FILE when their CA
is not in the system trust store. Hosted endpoints require OPENAI_API_KEY.
"""

from __future__ import annotations

import json
import os
import sys
import urllib.parse
from pathlib import Path

import neograph_engine as ng
from neograph_engine.llm import SchemaProvider


def _load_env() -> None:
    """Optionally load the nearest example/repository .env without overriding exports."""
    try:
        from dotenv import load_dotenv
    except ImportError:
        return
    here = Path(__file__).resolve().parent
    for parent in (here, *here.parents):
        candidate = parent / ".env"
        if candidate.is_file():
            load_dotenv(candidate, override=False)
            return


for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")
_load_env()


def schema_provider(schema: str = "openai", *, http_version=None):
    """Construct the Chat or Responses runtime from an admitted descriptor.

    OPENAI_API_BASE is an origin (or gateway prefix), not a complete route.
    NG_PROVIDER_DESCRIPTOR can name a complete descriptor instead, including
    provider-specific admitted headers. No arbitrary request fields are added.
    """
    descriptor_file = os.getenv("NG_PROVIDER_DESCRIPTOR")
    if descriptor_file:
        source = Path(descriptor_file).read_text(encoding="utf-8")
        document = json.loads(source)
    else:
        family = {"openai": "openai.chat", "openai_responses": "openai.responses"}[schema]
        responses = family == "openai.responses"
        endpoint = urllib.parse.urlsplit(os.getenv("OPENAI_API_BASE", "https://api.openai.com"))
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
                "max_output_tokens": "max_output_tokens" if responses else "max_tokens",
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
