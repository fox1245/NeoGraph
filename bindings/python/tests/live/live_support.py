"""Shared helpers of the live end-to-end suite. See tests/live/README.md.

Keys come from the environment only; nothing here prints or stores them.
"""

import base64
from collections.abc import Mapping
import json
import os
from pathlib import Path
import struct
import zlib

import pytest

import neograph_engine as ng
from neograph_engine.llm import SchemaProvider

LIVE = os.environ.get("NEOGRAPH_E2E_LIVE") == "1"
KEY_VARIABLES = ("OPENAI_API_KEY", "GEMINI_API_KEY", "ANTHROPIC_API_KEY", "OPENROUTER_API_KEY")
MANIFEST_PATH = Path(__file__).resolve().parents[4] / "tests" / "live" / "manifest.json"


def model_env_var(role):
    """Return the environment variable name that overrides the model for `role`."""
    return f"NG_LIVE_MODEL_{role.upper().replace('.', '_').replace('-', '_')}"


def live_model(role):
    """Return the configured model id for `role`, or its environment variable override."""
    models = json.loads(MANIFEST_PATH.read_text(encoding="utf-8")).get("models")
    if not isinstance(models, dict) or role not in models or not isinstance(models[role], str) or not models[role].strip():
        raise KeyError(f"missing live model role {role!r} in {MANIFEST_PATH}")
    variable = model_env_var(role)
    if variable in os.environ:
        override = os.environ[variable].strip()
        if not override:
            raise ValueError(f"blank live model override {variable} for role {role!r}")
        return override
    return models[role].strip()


def case_model_id(role, strip_date=False):
    """Return the pytest case id for `role`, preserving overrides without hiding the model."""
    model = live_model(role)
    if strip_date and model_env_var(role) not in os.environ:
        head, sep, tail = model.rpartition("-")
        if sep and len(tail) == 8 and tail.isdigit():
            return head
    return model


GOOGLE = "https://generativelanguage.googleapis.com"
# preset -> (family, origin, buffered path, streaming path, key variable)
ROUTES = {
    "openai.chat": ("openai.chat", "https://api.openai.com", "/v1/chat/completions", "/v1/chat/completions",
                    "OPENAI_API_KEY"),
    "openai.responses": ("openai.responses", "https://api.openai.com", "/v1/responses", "/v1/responses",
                         "OPENAI_API_KEY"),
    "anthropic.messages": ("anthropic.messages", "https://api.anthropic.com", "/v1/messages", "/v1/messages",
                           "ANTHROPIC_API_KEY"),
    "google.generate": ("google.generate", GOOGLE, "/v1beta/models/{model}:generateContent",
                        "/v1beta/models/{model}:streamGenerateContent?alt=sse", "GEMINI_API_KEY"),
    "google.interactions": ("google.interactions", GOOGLE, "/v1beta/interactions", "/v1beta/interactions",
                            "GEMINI_API_KEY"),
    "openrouter.chat": ("openai.chat", "https://openrouter.ai", "/api/v1/chat/completions",
                        "/api/v1/chat/completions", "OPENROUTER_API_KEY"),
    "openrouter.responses": ("openai.responses", "https://openrouter.ai", "/api/v1/responses", "/api/v1/responses",
                             "OPENROUTER_API_KEY"),
    "openrouter.messages": ("anthropic.messages", "https://openrouter.ai", "/api/v1/messages", "/api/v1/messages",
                            "OPENROUTER_API_KEY"),
}
# The cheapest model role of each preset that answers text, and the controls it needs. A role whose model reasons
# gets a larger cap so that its reasoning does not use up the answer.
TEXT_CONTROLS = {
    "openai.chat": {"reasoning_effort": "low", "max_output_tokens": 600},
    "openai.responses": {"reasoning_effort": "low", "max_output_tokens": 600},
    "anthropic.messages": {"max_output_tokens": 64},
    "google.generate": {"max_output_tokens": 64},
    "google.interactions": {"max_output_tokens": 64},
    "openrouter.chat": {"reasoning_effort": "low", "max_output_tokens": 600},
    "openrouter.responses": {"reasoning_effort": "low", "max_output_tokens": 600},
    "openrouter.messages": {"max_output_tokens": 64},
}


class _TextModels(Mapping):
    def __getitem__(self, preset):
        return live_model(preset), dict(TEXT_CONTROLS[preset])

    def __iter__(self):
        return iter(TEXT_CONTROLS)

    def __len__(self):
        return len(TEXT_CONTROLS)


TEXT_MODELS = _TextModels()


def needs(*variables):
    """Marks for a live case: skipped unless NEOGRAPH_E2E_LIVE=1 and every key variable is set."""
    marks = [pytest.mark.skipif(not LIVE, reason="live: set NEOGRAPH_E2E_LIVE=1")]
    for variable in variables:
        marks.append(pytest.mark.skipif(not os.environ.get(variable), reason=f"live: set {variable}"))
    return marks


def case(*values, keys, id, gap=None):
    """A pytest.param for a live case. `gap` names the issue of a known gap: the case must fail (strict xfail)."""
    marks = needs(*keys)
    if gap:
        marks.append(pytest.mark.xfail(strict=True, reason=f"known gap, {gap}"))
    return pytest.param(*values, marks=marks, id=id)


def provider(preset, model, timeout_ms=300_000):
    family, origin, buffered, streaming, variable = ROUTES[preset]
    document = {"descriptor_version": 1, "revision": 1, "id": "live-" + preset.replace(".", "-"), "family": family,
                "connection": {"base_url": origin, "paths": {"buffered": buffered.format(model=model),
                                                             "streaming": streaming.format(model=model)}}}
    options = ng.ProviderRuntimeOptions(api_key=os.environ.get(variable, ""), default_timeout_ms=timeout_ms)
    return SchemaProvider(ng.load_provider_descriptor(json.dumps(document)), options=options)


def controls(**values):
    result = ng.ProviderControls()
    for name, value in values.items():
        setattr(result, name, value)
    return result


def user(*parts):
    return ng.ProviderMessage(role=ng.ProviderRole.User,
                              parts=[ng.Text(p) if isinstance(p, str) else p for p in parts])


class Run:
    """One live call: the outcome plus, for a stream, every event."""

    def __init__(self, provider_, model, messages, stream=False, tools=(), **control_values):
        self.events = []
        mode = ng.ProviderMode.Stream if stream else ng.ProviderMode.Collect
        request = ng.make_provider_request(provider_, model, messages, tools=list(tools),
                                           controls=controls(**control_values), mode=mode)
        if stream:
            request.on_event = self.events.append
        self.request = request
        self.outcome = provider_.invoke(request)

    def ok(self):
        failure = self.outcome.failure
        assert failure is None, f"{failure.error.kind}: {failure.error.safe_message}"
        return self.outcome.completion

    def parts(self):
        return [p for m in self.ok().messages for p in m.parts]

    def text(self):
        return "".join(p.value for p in self.parts() if isinstance(p, ng.Text))

    def streamed_text(self):
        return "".join(e.value.bytes for e in self.events if e.kind == "PartDelta"
                       and str(e.value.kind).endswith("Text") and str(e.value.channel).endswith("Content"))


def red_png(size=64):
    raw = b"".join(b"\x00" + bytes((255, 0, 0)) * size for _ in range(size))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

    return base64.b64encode(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
                            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")).decode()


def weather_tool():
    return ng.ChatTool(name="get_weather", description="Get the current weather of a city.",
                       parameters={"type": "object", "properties": {"city": {"type": "string"}}, "required": ["city"]})
