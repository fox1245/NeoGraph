"""Gemini controls and history admission through actual SDK localhost TLS calls."""

import json

import pytest
import neograph_engine as ng


MODEL = "fixture-model"
PATHS = {
    "buffered": f"/v1beta/models/{MODEL}:generateContent",
    "streaming": f"/v1beta/models/{MODEL}:streamGenerateContent?alt=sse",
}


def _provider(peer, policy=None):
    return peer.provider("google.generate", paths=PATHS, policy=policy)


def _user(text="question"):
    return ng.ProviderMessage(ng.ProviderRole.User, [ng.Text(text)])


def _tools():
    return [ng.ChatTool(
        name=name, description=f"Find {name} value",
        parameters={"type": "object", "properties": {"x": {"type": "integer"}}},
    ) for name in ("lookup", "other")]


def _call(identity, name, x):
    call = ng.ProviderToolCall()
    call.id = identity
    call.name = name
    call.kind = ng.ProviderToolCallKind.ClientExecuted
    call.input = {"x": x}
    return call


def _choice(mode=ng.GeminiToolChoiceMode.Validated, names=("lookup",)):
    choice = ng.GeminiToolChoice()
    choice.mode = mode
    choice.allowed_function_names = list(names)
    return choice


def _safety():
    settings = []
    for category, threshold in (
        (ng.GeminiSafetyCategory.Harassment, ng.GeminiSafetyThreshold.BlockNone),
        (ng.GeminiSafetyCategory.HateSpeech, ng.GeminiSafetyThreshold.BlockOnlyHigh),
        (ng.GeminiSafetyCategory.SexuallyExplicit, ng.GeminiSafetyThreshold.BlockMediumAndAbove),
        (ng.GeminiSafetyCategory.DangerousContent, ng.GeminiSafetyThreshold.BlockLowAndAbove),
        (ng.GeminiSafetyCategory.CivicIntegrity, ng.GeminiSafetyThreshold.Off),
    ):
        setting = ng.GeminiSafetySetting()
        setting.category = category
        setting.threshold = threshold
        settings.append(setting)
    return settings


def _controls():
    controls = ng.ProviderControls()
    controls.max_output_tokens = 2048
    controls.include_thoughts = False
    controls.thinking_budget = None
    controls.gemini_thinking_level = ng.GeminiThinkingLevel.High
    controls.temperature = 0.25
    controls.safety_settings = _safety()
    controls.gemini_tool_choice = _choice()
    return controls


def _request(provider, messages=None, controls=None, mode=ng.ProviderMode.Collect):
    return ng.make_provider_request(
        provider, MODEL, [_user()] if messages is None else messages,
        tools=_tools(), controls=ng.ProviderControls() if controls is None else controls,
        mode=mode,
    )


def _response(parts=None, generation="local-generation"):
    return {
        "modelVersion": MODEL, "responseId": generation,
        "candidates": [{"index": 0, "content": {"role": "model", "parts":
            [{"text": "accepted", "thoughtSignature": "LOCAL_TEXT_SIG"}]
            if parts is None else parts}, "finishReason": "STOP"}],
        "usageMetadata": {"promptTokenCount": 10, "cachedContentTokenCount": 4,
                          "candidatesTokenCount": 7, "thoughtsTokenCount": 3,
                          "totalTokenCount": 20},
    }


def _serve(peer, parts=None):
    def respond(request):
        body = _response(parts)
        if request["path"] == PATHS["streaming"]:
            return 200, ("data: " + json.dumps(body) + "\n\n").encode(), "text/event-stream"
        return 200, body, "application/json"
    peer.responder = respond


def _accepted(outcome, text="accepted"):
    assert outcome.failure is None
    assert outcome.text == text
    assert outcome.completion.usage.input_total.value == 10
    assert outcome.completion.usage.output_total.value == 10
    assert outcome.completion.usage.reasoning.value == 3
    assert outcome.completion.usage.total.value == 20
    assert outcome.messages[-1].native.complete


def _denied(peer, provider, request, kind, sent=0):
    outcome = provider.invoke(request)
    assert outcome.completion is None
    assert outcome.failure.error.kind == kind
    assert not outcome.failure.error.attempt.request_may_have_left
    assert outcome.failure.error.attempt.request_body_bytes == 0
    assert len(peer.requests) == sent


@pytest.mark.parametrize("level, wire_level", [
    (ng.GeminiThinkingLevel.Minimal, "minimal"),
    (ng.GeminiThinkingLevel.Low, "low"),
    (ng.GeminiThinkingLevel.Medium, "medium"),
    (ng.GeminiThinkingLevel.High, "high"),
])
def test_typed_generation_controls_reach_generate_protocol(typed_peer, level, wire_level):
    _serve(typed_peer)
    provider = _provider(typed_peer)
    controls = _controls()
    controls.gemini_thinking_level = level
    _accepted(provider.invoke(_request(provider, controls=controls)))
    request = typed_peer.requests[0]
    assert request["method"] == "POST"
    assert request["path"] == PATHS["buffered"]
    body = request["body"]
    assert body["generationConfig"] == {
        "maxOutputTokens": 2048, "temperature": 0.25,
        "thinkingConfig": {"includeThoughts": False, "thinkingLevel": wire_level},
    }
    assert body["safetySettings"] == [
        {"category": "HARM_CATEGORY_HARASSMENT", "threshold": "BLOCK_NONE"},
        {"category": "HARM_CATEGORY_HATE_SPEECH", "threshold": "BLOCK_ONLY_HIGH"},
        {"category": "HARM_CATEGORY_SEXUALLY_EXPLICIT", "threshold": "BLOCK_MEDIUM_AND_ABOVE"},
        {"category": "HARM_CATEGORY_DANGEROUS_CONTENT", "threshold": "BLOCK_LOW_AND_ABOVE"},
        {"category": "HARM_CATEGORY_CIVIC_INTEGRITY", "threshold": "OFF"},
    ]
    assert body["toolConfig"] == {
        "functionCallingConfig": {"mode": "VALIDATED", "allowedFunctionNames": ["lookup"]},
    }
    assert body["tools"][0]["functionDeclarations"] == [
        {"name": tool.name, "description": tool.description,
         "parametersJsonSchema": tool.parameters} for tool in _tools()
    ]


@pytest.mark.parametrize("mode, wire_mode, names", [
    (ng.GeminiToolChoiceMode.Auto, "AUTO", ()),
    (ng.GeminiToolChoiceMode.Any, "ANY", ("lookup", "other")),
    (ng.GeminiToolChoiceMode.None_, "NONE", ()),
    (ng.GeminiToolChoiceMode.Validated, "VALIDATED", ("other",)),
])
def test_typed_function_selection_preserves_mode_and_allowlist(typed_peer, mode, wire_mode, names):
    _serve(typed_peer)
    provider = _provider(typed_peer)
    controls = _controls()
    controls.gemini_tool_choice = _choice(mode, names)
    _accepted(provider.invoke(_request(provider, controls=controls)))
    expected = {"mode": wire_mode}
    if names:
        expected["allowedFunctionNames"] = list(names)
    assert typed_peer.requests[0]["body"]["toolConfig"] == {"functionCallingConfig": expected}


def test_optional_controls_omit_wire_members_without_losing_explicit_zero(typed_peer):
    _serve(typed_peer)
    provider = _provider(typed_peer)
    controls = ng.ProviderControls()
    controls.gemini_history_mode = None
    controls.gemini_thinking_level = None
    controls.thinking_budget = None
    controls.gemini_tool_choice = None
    controls.temperature = None
    _accepted(provider.invoke(_request(provider, controls=controls)))
    body = typed_peer.requests[0]["body"]
    assert "temperature" not in body["generationConfig"]
    thinking = body["generationConfig"].get("thinkingConfig", {})
    assert "thinkingLevel" not in thinking
    assert "thinkingBudget" not in thinking
    assert "safetySettings" not in body
    assert "toolConfig" not in body
    controls.temperature = 0.0
    controls.include_thoughts = False
    controls.thinking_budget = 0
    _accepted(provider.invoke(_request(provider, controls=controls)))
    body = typed_peer.requests[1]["body"]
    assert body["generationConfig"]["temperature"] == 0.0
    assert body["generationConfig"]["thinkingConfig"]["includeThoughts"] is False
    assert body["generationConfig"]["thinkingConfig"]["thinkingBudget"] == 0


@pytest.mark.parametrize("control", [
    "gemini_history_mode", "gemini_thinking_level", "safety_settings", "gemini_tool_choice",
])
def test_gemini_controls_reject_wrong_family_before_io(typed_peer, control):
    provider = typed_peer.provider("openai.chat")
    controls = ng.ProviderControls()
    values = {
        "gemini_history_mode": ng.GeminiHistoryMode.NativeOnly,
        "gemini_thinking_level": ng.GeminiThinkingLevel.Low,
        "safety_settings": _safety(), "gemini_tool_choice": _choice(),
    }
    setattr(controls, control, values[control])
    with pytest.raises(ValueError):
        _request(provider, controls=controls)
    assert typed_peer.requests == []


def test_interactions_string_thinking_control_is_not_generate_control(typed_peer):
    provider = _provider(typed_peer)
    controls = ng.ProviderControls()
    controls.thinking_level = "high"
    with pytest.raises(ValueError):
        _request(provider, controls=controls)
    assert typed_peer.requests == []


@pytest.mark.parametrize("invalid", [
    "budget_level_conflict", "temperature_high", "temperature_negative", "safety_duplicate",
    "zero_budget_level_conflict",
    "choice_required_conflict", "choice_unknown", "choice_duplicate", "auto_allowlist",
])
def test_invalid_generation_controls_are_not_sent(typed_peer, invalid):
    provider = _provider(typed_peer)
    controls = _controls()
    if invalid == "budget_level_conflict":
        controls.thinking_budget = 1024
    elif invalid == "zero_budget_level_conflict":
        controls.thinking_budget = 0
    elif invalid == "temperature_high":
        controls.temperature = 3.0
    elif invalid == "temperature_negative":
        controls.temperature = -0.25
    elif invalid == "safety_duplicate":
        controls.safety_settings = _safety() + [_safety()[0]]
    elif invalid == "choice_required_conflict":
        controls.required_tool = "lookup"
    elif invalid == "choice_unknown":
        controls.gemini_tool_choice = _choice(names=("missing",))
    elif invalid == "choice_duplicate":
        controls.gemini_tool_choice = _choice(names=("lookup", "lookup"))
    else:
        controls.gemini_tool_choice = _choice(ng.GeminiToolChoiceMode.Auto)
    _denied(typed_peer, provider, _request(provider, controls=controls), ng.ProviderErrorKind.InvalidRequest)


def test_admitted_model_temperature_prohibition_is_enforced_without_io(typed_peer):
    policy = json.loads(ng.provider_policy_json())
    family = next(item for item in policy["families"] if item["family"] == "google.generate")
    family["temperature_forbidden_model_prefixes"] = ["fixture-"]
    admitted = ng.load_provider_policy(json.dumps(policy), ng.provider_codec_defaults_json())
    provider = _provider(typed_peer, admitted)
    controls = _controls()
    _denied(typed_peer, provider, _request(provider, controls=controls), ng.ProviderErrorKind.InvalidRequest)
    controls.temperature = None
    _serve(typed_peer)
    _accepted(provider.invoke(_request(provider, controls=controls)))
    assert "temperature" not in typed_peer.requests[0]["body"]["generationConfig"]


def _foreign_history(with_tools):
    history = [_user(), ng.ProviderMessage(ng.ProviderRole.Assistant, [ng.Text("prior answer")])]
    if with_tools:
        history += [
            ng.ProviderMessage(ng.ProviderRole.Assistant, [
                ng.Text("checking"), _call("foreign-a", "lookup", 1),
                ng.Text("also"), _call("foreign-b", "other", 2),
            ]),
            ng.ProviderMessage(ng.ProviderRole.Tool, [
                ng.ProviderToolResult("foreign-b", '{"second":2}'),
                ng.ProviderToolResult("foreign-a", '{"first":1}', is_error=True),
            ]),
        ]
    return history


@pytest.mark.parametrize("with_tools", [False, True])
@pytest.mark.parametrize("mode", [ng.ProviderMode.Collect, ng.ProviderMode.Stream])
def test_foreign_history_requires_explicit_portable_mode_and_binds_native_prefix(typed_peer, with_tools, mode):
    _serve(typed_peer)
    provider = _provider(typed_peer)
    history = _foreign_history(with_tools)
    controls = _controls()
    controls.gemini_history_mode = ng.GeminiHistoryMode.NativeOnly
    _denied(typed_peer, provider, _request(provider, history, controls, mode),
            ng.ProviderErrorKind.ReplayIneligible)
    if with_tools:
        _denied(typed_peer, provider, _request(provider, [history[0]] + history[2:], controls, mode),
                ng.ProviderErrorKind.ReplayIneligible)
    controls.gemini_history_mode = None
    _denied(typed_peer, provider, _request(provider, history, controls, mode),
            ng.ProviderErrorKind.ReplayIneligible)
    controls.gemini_history_mode = ng.GeminiHistoryMode.PortableForeign
    first = provider.invoke(_request(provider, history, controls, mode))
    _accepted(first)
    body = typed_peer.requests[0]["body"]
    expected = [
        {"role": "user", "parts": [{"text": "question"}]},
        {"role": "model", "parts": [{"text": "prior answer"}]},
    ]
    if with_tools:
        expected += [
            {"role": "model", "parts": [
                {"text": "checking"},
                {"functionCall": {"id": "foreign-a", "name": "lookup", "args": {"x": 1}},
                 "thoughtSignature": "skip_thought_signature_validator"},
                {"text": "also"},
                {"functionCall": {"id": "foreign-b", "name": "other", "args": {"x": 2}}},
            ]},
            {"role": "user", "parts": [
                {"functionResponse": {"name": "other", "id": "foreign-b", "response": {"second": 2}}},
                {"functionResponse": {"name": "lookup", "id": "foreign-a",
                                      "response": {"error": {"first": 1}}}},
            ]},
        ]
    assert body["contents"] == expected
    assert all(message.native is None and message.wire_output is None for message in history)
    continuation = history + first.messages
    changed = _foreign_history(with_tools)
    changed[1].parts = [ng.Text("changed imported answer")]
    _denied(typed_peer, provider, _request(provider, changed + first.messages, controls, mode),
            ng.ProviderErrorKind.ReplayIneligible, sent=1)
    next_mode = ng.ProviderMode.Stream if mode == ng.ProviderMode.Collect else ng.ProviderMode.Collect
    _accepted(provider.invoke(_request(provider, continuation, controls, next_mode)))
    assert typed_peer.requests[1]["body"]["contents"] == expected + [{
        "role": "model", "parts": [{"text": "accepted", "thoughtSignature": "LOCAL_TEXT_SIG"}],
    }]


@pytest.mark.parametrize("invalid, kind", [
    ("foreign_thinking", ng.ProviderErrorKind.ReplayIneligible),
    ("foreign_call_signature", ng.ProviderErrorKind.ReplayIneligible),
    ("duplicate_call", ng.ProviderErrorKind.InvalidRequest),
    ("unknown_function", ng.ProviderErrorKind.InvalidRequest),
    ("array_arguments", ng.ProviderErrorKind.InvalidRequest),
    ("missing_result", ng.ProviderErrorKind.InvalidRequest),
    ("duplicate_result", ng.ProviderErrorKind.InvalidRequest),
    ("orphan_result", ng.ProviderErrorKind.InvalidRequest),
])
def test_portable_mode_does_not_admit_signed_foreign_parts_or_broken_tool_ownership(typed_peer, invalid, kind):
    provider = _provider(typed_peer)
    controls = _controls()
    controls.gemini_history_mode = ng.GeminiHistoryMode.PortableForeign
    history = _foreign_history(True)
    if invalid == "foreign_thinking":
        history[1].parts = [ng.Text("prior answer"), ng.Thinking("private", "captured")]
    elif invalid == "missing_result":
        history.pop()
    elif invalid == "duplicate_result":
        history[-1].parts = [ng.ProviderToolResult("foreign-a", '{}'),
                             ng.ProviderToolResult("foreign-a", '{}')]
    elif invalid == "orphan_result":
        history[-1].parts = [ng.ProviderToolResult("unowned", '{}')]
    else:
        parts = history[2].parts
        call = parts[1]
        if invalid == "foreign_call_signature":
            call.wire_metadata = {"thoughtSignature": "captured"}
        elif invalid == "duplicate_call":
            parts[3] = _call("foreign-a", "other", 2)
        elif invalid == "unknown_function":
            call.name = "undeclared"
        elif invalid == "array_arguments":
            call.input = []
        parts[1] = call
        history[2].parts = parts
    _denied(typed_peer, provider, _request(provider, history, controls), kind)


NATIVE_PARTS = [
    {"text": "Consider", "thought": True, "thoughtSignature": "LOCAL_THOUGHT_SIG"},
    {"text": "need lookup", "thoughtSignature": "LOCAL_TEXT_SIG"},
    {"functionCall": {"id": "call-owned", "name": "lookup", "args": {"x": 1}},
     "thoughtSignature": "LOCAL_CALL_SIG"},
]


@pytest.mark.parametrize("damage", [
    "thinking_signature", "call_arguments", "text", "temperature", "thinking_level",
    "safety", "account_scope", "missing_prefix",
])
def test_portable_mode_never_demotes_damaged_or_mismatched_native_history(typed_peer, damage):
    _serve(typed_peer, NATIVE_PARTS)
    provider = _provider(typed_peer)
    controls = _controls()
    first = provider.invoke(_request(provider, controls=controls))
    _accepted(first, "need lookup")
    assert first.completion.stop.kind == ng.ProviderStopKind.ToolUse
    controls.gemini_history_mode = ng.GeminiHistoryMode.PortableForeign
    native = first.messages[0]
    history = [_user(), native, ng.ProviderMessage(ng.ProviderRole.Tool, [
        ng.ProviderToolResult("call-owned", '{"result":1}'),
    ])]
    if damage in ("thinking_signature", "call_arguments", "text"):
        parts = native.parts
        if damage == "thinking_signature":
            parts[0].signature = "changed signature"
        elif damage == "call_arguments":
            parts[2].input = {"x": 99}
        else:
            parts[1].value = "changed answer"
        native.parts = parts
    elif damage == "temperature":
        controls.temperature = 0.5
    elif damage == "thinking_level":
        controls.gemini_thinking_level = ng.GeminiThinkingLevel.Low
    elif damage == "safety":
        settings = controls.safety_settings
        settings[0].threshold = ng.GeminiSafetyThreshold.Off
        controls.safety_settings = settings
    elif damage == "account_scope":
        controls.account_scope = "different-account"
    else:
        history.pop(0)
    _denied(typed_peer, provider, _request(provider, history, controls),
            ng.ProviderErrorKind.ReplayIneligible, sent=1)
    # The immutable outcome still supplies the original sealed message; per-turn
    # tool selection may change, but the original generated parts must not.
    controls = _controls()
    controls.gemini_history_mode = ng.GeminiHistoryMode.PortableForeign
    controls.gemini_tool_choice = _choice(ng.GeminiToolChoiceMode.None_, ())
    original = [_user()] + first.messages + [ng.ProviderMessage(ng.ProviderRole.Tool, [
        ng.ProviderToolResult("call-owned", '{"result":1}'),
    ])]
    _serve(typed_peer)
    _accepted(provider.invoke(_request(provider, original, controls)))
    body = typed_peer.requests[1]["body"]
    assert body["contents"][1] == {"role": "model", "parts": NATIVE_PARTS}
    assert body["contents"][2] == {"role": "user", "parts": [{
        "functionResponse": {"name": "lookup", "id": "call-owned", "response": {"result": 1}},
    }]}
    assert body["toolConfig"] == {"functionCallingConfig": {"mode": "NONE"}}
