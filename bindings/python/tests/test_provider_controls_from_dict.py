"""ProviderControls family nests from Python: the binding, strict from_dict, and the bytes they encode."""

import json

import pytest
import neograph_engine as ng
from neograph_engine import llm as nglm
from neograph_engine.provider_controls import FIELDS

# Flat ProviderControls members kept for one release; from_dict takes the family nests instead.
DEPRECATED = {
    "chat_reasoning", "include_reasoning", "usage_include", "models", "previous_response_id",
    "previous_response_history", "parallel_tool_calls", "verbosity", "truncation", "responses_include",
    "thinking_mode", "output_effort", "cache_control", "messages_tool_choice", "gemini_history_mode",
    "gemini_thinking_level", "safety_settings", "gemini_tool_choice",
}

ROUTES = {
    "google.generate": ("https://generativelanguage.googleapis.com",
                        "/v1beta/models/fixture-model:generateContent",
                        "/v1beta/models/fixture-model:streamGenerateContent?alt=sse"),
    "openai.responses": ("https://api.openai.com", "/v1/responses", "/v1/responses"),
    "anthropic.messages": ("https://api.anthropic.com", "/v1/messages", "/v1/messages"),
    "openai.chat": ("https://api.openai.com", "/v1/chat/completions", "/v1/chat/completions"),
    "google.interactions": ("https://generativelanguage.googleapis.com", "/v1beta/interactions",
                            "/v1beta/interactions"),
}


def _class(name):
    target = ng
    for part in name.split("."):
        target = getattr(target, part)
    return target


def _properties(cls):
    return {name for name in dir(cls) if not name.startswith("_") and isinstance(getattr(cls, name), property)}


def _body(family, controls):
    origin, buffered, streaming = ROUTES[family]
    descriptor = {"descriptor_version": 1, "revision": 1, "id": "fixture-" + family.replace(".", "-"),
                  "family": family, "connection": {"base_url": origin,
                                                   "paths": {"buffered": buffered, "streaming": streaming}}}
    provider = nglm.SchemaProvider(ng.load_provider_descriptor(json.dumps(descriptor)), ng.ProviderRuntimeOptions())
    controls.max_output_tokens = 64
    if family != "openai.chat":
        controls.account_scope = "fixture-account"
    message = ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("ping")])
    prepared = provider.prepare(ng.make_provider_request(provider, "fixture-model", [message], [], controls))
    assert prepared.error is None, prepared.error.reason
    return json.loads(prepared.encoded_body)


@pytest.mark.parametrize("name", sorted(FIELDS))
def test_from_dict_table_matches_the_binding(name):
    expected = _properties(_class(name))
    if name == "ProviderControls":
        expected -= DEPRECATED
    assert set(FIELDS[name]) == expected


def test_from_dict_builds_nested_values_and_refuses_unknown_keys():
    controls = ng.ProviderControls.from_dict({
        "temperature": 0.25,
        "generate": {"response_modalities": ["Audio"],
                     "speech_config": {"voice_config": {"prebuilt_voice_name": "Kore"}}},
    })
    assert controls.temperature == 0.25
    assert controls.generate.response_modalities == [ng.GeminiResponseModality.Audio]
    assert controls.generate.speech_config.voice_config.prebuilt_voice_name == "Kore"
    with pytest.raises(ValueError, match="unknown key 'voice_name'"):
        ng.GenerateControls.from_dict({"speech_config": {"voice_config": {"voice_name": "Kore"}}})
    with pytest.raises(ValueError, match="expected one of"):
        ng.GenerateControls.from_dict({"response_modalities": ["AUDIO"]})
    with pytest.raises(TypeError, match="expected an int"):
        ng.ProviderControls.from_dict({"max_output_tokens": "64"})


def test_hosted_tools_take_their_class_from_the_type():
    responses = ng.ResponsesControls.from_dict({"hosted_tools": [
        {"type": "image_generation", "size": "1024x1024"},
        {"type": "web_search", "allowed_domains": ["example.com"]},
        {"type": "openrouter:web_search", "parameters": {"max_results": 3}},
        {"type": "policy_admitted_tool", "options": {"level": 1}},
    ]})
    tools = responses.hosted_tools
    assert [type(tool).__name__ for tool in tools] == [
        "ResponsesImageGenerationTool", "ResponsesWebSearchTool", "ResponsesOpenRouterServerTool",
        "ResponsesGenericHostedTool"]
    assert tools[0].size == "1024x1024"
    assert tools[2].parameters == {"max_results": 3}
    with pytest.raises(ValueError, match="unknown type"):
        ng.GenerateControls.from_dict({"builtin_tools": [{"type": "web_search"}]})


def test_the_nest_of_another_family_is_ignored():
    controls = ng.ProviderControls.from_dict({"generate": {"response_modalities": ["Audio"]},
                                               "responses": {"hosted_tools": [{"type": "web_search"}]}})
    body = _body("openai.chat", controls)
    assert "tools" not in body and "modalities" not in body


@pytest.mark.parametrize("family, data, check", [
    ("google.generate",
     {"generate": {"response_modalities": ["Audio"],
                   "speech_config": {"voice_config": {"prebuilt_voice_name": "Kore"}}}},
     lambda body: body["generationConfig"]["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"]
     == "Kore"),
    ("openai.responses", {"responses": {"hosted_tools": [{"type": "image_generation", "size": "1024x1024"}]}},
     lambda body: body["tools"] == [{"type": "image_generation", "size": "1024x1024"}]),
    ("anthropic.messages",
     {"messages": {"tools": [{"type": "web_search_20250305", "name": "web_search", "max_uses": 1}]}},
     lambda body: body["tools"][0]["type"] == "web_search_20250305"),
    ("openai.chat", {"chat": {"modalities": ["Text", "Audio"], "audio": {"voice": "alloy", "format": "wav"}}},
     lambda body: body["modalities"] == ["text", "audio"] and body["audio"]["voice"] == "alloy"),
    ("google.interactions",
     {"store": True, "interactions": {"background": True, "previous_interaction_id": "v1_previous"}},
     lambda body: body["background"] is True and body["previous_interaction_id"] == "v1_previous"),
])
def test_from_dict_and_classes_encode_the_same_body(family, data, check):
    from_dict = _body(family, ng.ProviderControls.from_dict(data))
    assert check(from_dict)
    built = ng.ProviderControls()
    for key, value in data.items():
        if isinstance(value, dict):
            setattr(built, key, getattr(ng, type(getattr(built, key)).__name__).from_dict(value))
        else:
            setattr(built, key, value)
    assert _body(family, built) == from_dict


def test_deprecated_members_move_into_their_nests():
    alias = ng.ProviderControls()
    alias.gemini_thinking_level = ng.GeminiThinkingLevel.Low
    nest = ng.ProviderControls.from_dict({"generate": {"thinking_level": "Low"}})
    assert _body("google.generate", alias) == _body("google.generate", nest)
    both = ng.ProviderControls.from_dict({"generate": {"thinking_level": "Low"}})
    both.gemini_thinking_level = ng.GeminiThinkingLevel.High
    with pytest.raises(ValueError, match="or its family nest"):
        _body("google.generate", both)
