"""Retained Chat/Messages controls through admitted localhost TLS providers."""

import json

import pytest

import neograph_engine as ng
import neograph_engine.llm as nglm


def _policy(peer, *, routed=True):
    source = json.loads(ng.provider_policy_json())
    for family in source["families"]:
        if family["family"] in ("openai.chat", "anthropic.messages"):
            family["openrouter_origins"] = [peer.base_url] if routed else []
    return ng.load_provider_policy(json.dumps(source), ng.provider_codec_defaults_json())


def _controls(**fields):
    value = ng.ProviderControls()
    for name, field in fields.items():
        setattr(value, name, field)
    return value


def _reasoning(**fields):
    value = ng.ChatReasoningOptions()
    for name, field in fields.items():
        setattr(value, name, field)
    return value


def _choice(mode, *, name="", parallel=None):
    value = ng.MessagesToolChoice()
    value.mode = mode
    value.name = name
    value.disable_parallel_tool_use = parallel
    return value


def _cache(ttl=None):
    value = ng.MessagesCacheControl()
    value.ttl = ttl
    return value


def _routing():
    value = ng.OpenRouterRouting()
    value.order = ["anthropic"]
    value.only = ["anthropic"]
    value.allow_fallbacks = False
    value.require_parameters = True
    value.data_collection = "deny"
    return value


def _tools():
    return [ng.ChatTool("lookup", "Read a local value", {
        "type": "object", "properties": {"key": {"type": "string"}},
    })]


def _request(provider, controls, *, model="fixture-model", messages=None,
             tools=None, mode=ng.ProviderMode.Collect):
    if messages is None:
        messages = [ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("Read the local value")])]
    return ng.make_provider_request(provider, model, messages, tools or [], controls, mode)


def _sse(frames, *, done=False):
    wire = "".join(("event: " + name + "\n" if name else "")
                   + "data: " + json.dumps(body) + "\n\n" for name, body in frames)
    if done:
        wire += "data: [DONE]\n\n"
    return wire.encode()


def _chat_response(request):
    metadata = {"id": "chat-local", "model": request["body"]["model"], "created": 1}
    usage = {"prompt_tokens": 2, "completion_tokens": 3, "total_tokens": 5}
    if request["body"].get("stream"):
        chunks = [
            {**metadata, "object": "chat.completion.chunk", "choices": [
                {"index": 0, "delta": {"role": "assistant", "content": "local answer"},
                 "finish_reason": None}]},
            {**metadata, "object": "chat.completion.chunk", "choices": [
                {"index": 0, "delta": {}, "finish_reason": "stop"}]},
            {**metadata, "object": "chat.completion.chunk", "choices": [], "usage": usage},
        ]
        return 200, _sse([("", chunk) for chunk in chunks], done=True), "text/event-stream"
    return 200, {**metadata, "object": "chat.completion", "choices": [
        {"index": 0, "message": {"role": "assistant", "content": "local answer"},
         "finish_reason": "stop"}], "usage": usage}, "application/json"


def _messages_response(request):
    message = {"id": "msg-local", "type": "message", "role": "assistant",
               "model": request["body"]["model"],
               "content": [{"type": "text", "text": "local answer"}],
               "stop_reason": "end_turn", "stop_sequence": None,
               "usage": {"input_tokens": 2, "output_tokens": 3,
                         "cache_read_input_tokens": 0, "cache_creation_input_tokens": 0}}
    if not request["body"].get("stream"):
        return 200, message, "application/json"
    initial = {**message, "content": [], "stop_reason": None,
               "usage": {**message["usage"], "output_tokens": 0}}
    frames = [
        ("message_start", {"type": "message_start", "message": initial}),
        ("content_block_start", {"type": "content_block_start", "index": 0,
                                 "content_block": {"type": "text", "text": ""}}),
        ("content_block_delta", {"type": "content_block_delta", "index": 0,
                                 "delta": {"type": "text_delta", "text": "local answer"}}),
        ("content_block_stop", {"type": "content_block_stop", "index": 0}),
        ("message_delta", {"type": "message_delta",
                           "delta": {"stop_reason": "end_turn", "stop_sequence": None},
                           "usage": {"output_tokens": 3}}),
        ("message_stop", {"type": "message_stop"}),
    ]
    return 200, _sse(frames), "text/event-stream"


def _success(provider, request):
    outcome = provider.invoke(request)
    assert outcome.failure is None
    assert outcome.text == "local answer"
    assert outcome.completion.messages[0].native is not None
    assert outcome.completion.usage.output_total.value == 3
    return outcome


def _rejected(peer, provider, controls, *, model="fixture-model", tools=None,
              messages=None, kind=ng.ProviderErrorKind.InvalidRequest):
    before = len(peer.requests)
    outcome = provider.invoke(_request(provider, controls, model=model,
                                       tools=tools, messages=messages))
    assert outcome.completion is None
    assert outcome.failure.error.kind == kind
    assert len(peer.requests) == before


@pytest.mark.parametrize("mode", [ng.ProviderMode.Collect, ng.ProviderMode.Stream])
@pytest.mark.parametrize("budgeted", [False, True])
def test_routed_chat_reasoning_and_alternative_models_have_owned_response(typed_peer, mode, budgeted):
    typed_peer.responder = _chat_response
    provider = typed_peer.provider("openai.chat", policy=_policy(typed_peer))
    reasoning = (_reasoning(max_tokens=256, exclude=False, enabled=True) if budgeted
                 else _reasoning(effort="low", exclude=False, enabled=True))
    controls = _controls(max_output_tokens=512, chat_reasoning=reasoning,
                         include_reasoning=True, usage_include=False,
                         models=["openai/gpt-4o-mini", "anthropic/claude-sonnet-4"],
                         provider=_routing())
    outcome = _success(provider, _request(provider, controls, mode=mode))
    body = typed_peer.requests[0]["body"]
    expected = {"exclude": False, "enabled": True,
                **({"max_tokens": 256} if budgeted else {"effort": "low"})}
    assert body["reasoning"] == expected
    assert body["include_reasoning"] is True
    assert body["usage"] == {"include": False}
    assert body["models"] == ["openai/gpt-4o-mini", "anthropic/claude-sonnet-4"]
    assert body["provider"]["allow_fallbacks"] is False
    assert outcome.completion.usage.total.value == 5
    assert len(typed_peer.requests) == 1


@pytest.mark.parametrize("field,explicit", [("include_reasoning", False), ("usage_include", False)])
def test_chat_omitted_and_explicit_false_are_distinct_native_configurations(typed_peer, field, explicit):
    typed_peer.responder = _chat_response
    provider = typed_peer.provider("openai.chat", policy=_policy(typed_peer))
    controls = _controls(max_output_tokens=512)
    request = _request(provider, controls)
    outcome = _success(provider, request)
    body = typed_peer.requests[0]["body"]
    assert ("include_reasoning" not in body if field == "include_reasoning" else "usage" not in body)
    history = list(request.messages) + outcome.messages + [
        ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("Continue")])]
    _success(provider, _request(provider, controls, messages=history))
    changed = _controls(max_output_tokens=512, **{field: explicit})
    _rejected(typed_peer, provider, changed, messages=history,
              kind=ng.ProviderErrorKind.ReplayIneligible)
    _success(provider, _request(provider, changed))
    emitted = typed_peer.requests[-1]["body"]
    assert (emitted["include_reasoning"] is False if field == "include_reasoning"
            else emitted["usage"] == {"include": False})
    assert len(typed_peer.requests) == 3


@pytest.mark.parametrize("fields", [
    {"chat_reasoning": "empty"},
    {"chat_reasoning": {"effort": "arbitrary"}},
    {"chat_reasoning": {"effort": "low", "max_tokens": 128}},
    {"chat_reasoning": {"max_tokens": 0}},
    {"chat_reasoning": {"max_tokens": 513}},
    {"chat_reasoning": {"max_tokens": (1 << 64) - 1}},
    {"chat_reasoning": {"effort": "low", "enabled": False}},
    {"chat_reasoning": {"exclude": True}, "include_reasoning": True},
    {"chat_reasoning": {"effort": "low"}, "reasoning_effort": "low"},
    {"models": ["same", "same"]},
    {"models": [""]},
    {"models": ["OpenAI/GPT-5-nano"], "temperature": 0.5},
])
def test_chat_control_conflicts_reject_before_peer_io(typed_peer, fields):
    provider = typed_peer.provider("openai.chat", policy=_policy(typed_peer))
    fields = dict(fields)
    if "chat_reasoning" in fields:
        value = fields["chat_reasoning"]
        fields["chat_reasoning"] = _reasoning(**(value if isinstance(value, dict) else {}))
    _rejected(typed_peer, provider, _controls(max_output_tokens=512, **fields))


@pytest.mark.parametrize("family,fields", [
    ("openai.chat", {"include_reasoning": False}),
    ("openai.chat", {"usage_include": False}),
    ("openai.chat", {"chat_reasoning": {"enabled": False}}),
    ("openai.chat", {"models": ["alternative"]}),
    ("anthropic.messages", {"provider": "routing"}),
])
def test_gateway_controls_need_explicit_origin_admission(typed_peer, family, fields):
    provider = typed_peer.provider(family, policy=_policy(typed_peer, routed=False))
    fields = dict(fields)
    if "chat_reasoning" in fields:
        fields["chat_reasoning"] = _reasoning(**fields["chat_reasoning"])
    if "provider" in fields:
        fields["provider"] = _routing()
    _rejected(typed_peer, provider, _controls(**fields))


@pytest.mark.parametrize("mode", [ng.ProviderMode.Collect, ng.ProviderMode.Stream])
@pytest.mark.parametrize("thinking", [ng.MessagesThinkingMode.Manual,
                                      ng.MessagesThinkingMode.Adaptive,
                                      ng.MessagesThinkingMode.Disabled])
def test_messages_thinking_effort_cache_choice_and_routing(typed_peer, mode, thinking):
    typed_peer.responder = _messages_response
    provider = typed_peer.provider("anthropic.messages", policy=_policy(typed_peer))
    disabled = thinking == ng.MessagesThinkingMode.Disabled
    choice = _choice(ng.MessagesToolChoiceMode.Tool if disabled else ng.MessagesToolChoiceMode.Auto,
                     name="lookup" if disabled else "", parallel=False)
    controls = _controls(max_output_tokens=2048, temperature=0.7, top_p=0.95,
                         thinking_mode=thinking, output_effort=ng.MessagesOutputEffort.High,
                         cache_control=_cache(ng.MessagesCacheTtl.OneHour),
                         messages_tool_choice=choice, provider=_routing())
    if thinking == ng.MessagesThinkingMode.Manual:
        controls.thinking_budget = 1024
    _success(provider, _request(provider, controls, tools=_tools(), mode=mode))
    body = typed_peer.requests[0]["body"]
    expected = ({"type": "enabled", "budget_tokens": 1024}
                if thinking == ng.MessagesThinkingMode.Manual
                else {"type": "disabled" if disabled else "adaptive"})
    assert body["thinking"] == expected
    if disabled:
        assert body["temperature"] == 0.7
    else:
        assert "temperature" not in body
    assert body["top_p"] == 0.95
    assert body["output_config"] == {"effort": "high"}
    assert body["cache_control"] == {"type": "ephemeral", "ttl": "1h"}
    assert body["tool_choice"] == {"type": "tool" if disabled else "auto",
                                   **({"name": "lookup"} if disabled else {}),
                                   "disable_parallel_tool_use": False}
    assert body["provider"] == {"only": ["anthropic"], "order": ["anthropic"],
                                "allow_fallbacks": False, "require_parameters": True,
                                "data_collection": "deny"}
    assert len(typed_peer.requests) == 1


@pytest.mark.parametrize("effort,name,ttl,wire_ttl,choice,wire_choice", [
    (ng.MessagesOutputEffort.Low, "low", None, None, ng.MessagesToolChoiceMode.Auto, "auto"),
    (ng.MessagesOutputEffort.Medium, "medium", ng.MessagesCacheTtl.FiveMinutes, "5m",
     ng.MessagesToolChoiceMode.Any, "any"),
    (ng.MessagesOutputEffort.Max, "max", ng.MessagesCacheTtl.OneHour, "1h",
     ng.MessagesToolChoiceMode.None_, "none"),
])
def test_messages_explicit_effort_cache_ttl_and_tool_selection(typed_peer, effort, name,
                                                              ttl, wire_ttl, choice, wire_choice):
    typed_peer.responder = _messages_response
    provider = typed_peer.provider("anthropic.messages", policy=_policy(typed_peer))
    controls = _controls(thinking_mode=ng.MessagesThinkingMode.Disabled,
                         output_effort=effort, cache_control=_cache(ttl),
                         messages_tool_choice=_choice(choice))
    _success(provider, _request(provider, controls, tools=_tools()))
    body = typed_peer.requests[0]["body"]
    assert body["output_config"] == {"effort": name}
    assert body["cache_control"] == {"type": "ephemeral", **({"ttl": wire_ttl} if wire_ttl else {})}
    assert body["tool_choice"] == {"type": wire_choice}


@pytest.mark.parametrize("thinking,budget,cap,top_p,temperature", [
    (ng.MessagesThinkingMode.Manual, None, 2048, None, None),
    (ng.MessagesThinkingMode.Manual, 0, 2048, None, None),
    (ng.MessagesThinkingMode.Manual, 1023, 2048, None, None),
    (ng.MessagesThinkingMode.Manual, 2048, 2048, None, None),
    (ng.MessagesThinkingMode.Manual, 2049, 2048, None, None),
    (ng.MessagesThinkingMode.Adaptive, 1024, 2048, None, None),
    (ng.MessagesThinkingMode.Disabled, 1024, 2048, None, None),
    (ng.MessagesThinkingMode.Manual, 1024, 2048, 0.949, None),
    (ng.MessagesThinkingMode.Adaptive, None, 2048, 0.949, None),
    (ng.MessagesThinkingMode.Disabled, None, 0, None, None),
    (ng.MessagesThinkingMode.Adaptive, None, 2048, None, float("nan")),
    (ng.MessagesThinkingMode.Manual, 1024, 2048, None, 1.01),
])
def test_messages_thinking_cap_sampling_boundaries_before_io(typed_peer, thinking, budget,
                                                            cap, top_p, temperature):
    provider = typed_peer.provider("anthropic.messages", policy=_policy(typed_peer))
    _rejected(typed_peer, provider, _controls(thinking_mode=thinking, thinking_budget=budget,
                                            max_output_tokens=cap, top_p=top_p,
                                            temperature=temperature))


@pytest.mark.parametrize("mode,name,parallel,thinking,with_tools", [
    (ng.MessagesToolChoiceMode.Tool, "unknown", None, ng.MessagesThinkingMode.Disabled, True),
    (ng.MessagesToolChoiceMode.Auto, "lookup", None, ng.MessagesThinkingMode.Disabled, True),
    (ng.MessagesToolChoiceMode.Any, "", None, ng.MessagesThinkingMode.Adaptive, True),
    (ng.MessagesToolChoiceMode.Tool, "lookup", None, ng.MessagesThinkingMode.Manual, True),
    (ng.MessagesToolChoiceMode.Any, "", None, ng.MessagesThinkingMode.Disabled, False),
    (ng.MessagesToolChoiceMode.None_, "", False, ng.MessagesThinkingMode.Disabled, True),
])
def test_messages_tool_choice_conflicts_before_io(typed_peer, mode, name, parallel, thinking, with_tools):
    provider = typed_peer.provider("anthropic.messages", policy=_policy(typed_peer))
    controls = _controls(thinking_mode=thinking, max_output_tokens=2048,
                         messages_tool_choice=_choice(mode, name=name, parallel=parallel))
    if thinking == ng.MessagesThinkingMode.Manual:
        controls.thinking_budget = 1024
    _rejected(typed_peer, provider, controls, tools=_tools() if with_tools else [])


@pytest.mark.parametrize("family,model", [
    ("openai.chat", "gpt-5-mini"), ("openai.chat", "GPT-6"),
    ("openai.chat", "openai/O3-mini"), ("openai.chat", "gateway/OpenAI/O4-test"),
    ("openai.chat", "o1-preview"),
    ("anthropic.messages", "anthropic/CLAUDE-OPUS-4-7"),
    ("anthropic.messages", "claude-opus-4-8-test"),
    ("anthropic.messages", "claude-opus-5"),
    ("anthropic.messages", "claude-sonnet-5"),
    ("anthropic.messages", "claude-fable-test"),
])
def test_model_temperature_prohibition_is_closed_before_io(typed_peer, family, model):
    typed_peer.responder = _chat_response if family == "openai.chat" else _messages_response
    provider = typed_peer.provider(family, policy=_policy(typed_peer))
    controls = _controls(temperature=0.7)
    if family == "anthropic.messages":
        controls.thinking_mode = ng.MessagesThinkingMode.Adaptive
    _rejected(typed_peer, provider, controls, model=model)
    controls.temperature = None
    _success(provider, _request(provider, controls, model=model))
    assert "temperature" not in typed_peer.requests[0]["body"]
    assert len(typed_peer.requests) == 1


@pytest.mark.parametrize("family,field", [
    ("anthropic.messages", "include_reasoning"),
    ("anthropic.messages", "usage_include"),
    ("anthropic.messages", "chat_reasoning"),
    ("anthropic.messages", "models"),
    ("openai.chat", "thinking_mode"),
    ("openai.chat", "output_effort"),
    ("openai.chat", "cache_control"),
    ("openai.chat", "messages_tool_choice"),
])
def test_family_inapplicable_explicit_controls_reject_without_io(typed_peer, family, field):
    provider = typed_peer.provider(family, policy=_policy(typed_peer))
    fields = {"include_reasoning": False, "usage_include": False,
              "chat_reasoning": _reasoning(enabled=False), "models": ["alternative"],
              "thinking_mode": ng.MessagesThinkingMode.Disabled,
              "output_effort": ng.MessagesOutputEffort.Low, "cache_control": _cache(),
              "messages_tool_choice": _choice(ng.MessagesToolChoiceMode.None_)}
    with pytest.raises(ValueError):
        _request(provider, _controls(**{field: fields[field]}))
    assert typed_peer.requests == []


def _descriptor_source(peer, family="anthropic.messages", headers=None):
    source = {"descriptor_version": 1, "revision": 1, "id": "local-deployment",
              "family": family, "connection": {"base_url": peer.base_url,
              "paths": {"buffered": "/operation", "streaming": "/operation"}}}
    if headers is not None:
        source["connection"]["headers"] = headers
    return json.dumps(source)


def _environment(workspace=None, beta=None):
    value = ng.ProviderDeploymentHeaderEnvironment()
    value.anthropic_workspace_id = workspace
    value.anthropic_beta = beta
    return value


def _deployment_provider(peer, source, environment, overrides=()):
    admitted = ng.load_provider_descriptor_with_deployment_headers(
        source, list(overrides), environment, _policy(peer))
    options = ng.ProviderRuntimeOptions(ca_file=peer.ca_file, default_timeout_ms=5000)
    options.http_version = ng.ProviderHttpVersion.Http1_1
    return nglm.SchemaProvider(admitted, options)


def _header_values(request, name):
    return [value for key, value in request["raw_headers"] if key.lower() == name]


@pytest.mark.parametrize("workspace,beta", [(None, None), ("", "")])
def test_optional_deployment_headers_are_omitted_on_real_messages_call(typed_peer, workspace, beta):
    typed_peer.responder = _messages_response
    provider = _deployment_provider(typed_peer, _descriptor_source(typed_peer),
                                    _environment(workspace, beta))
    _success(provider, _request(provider, _controls()))
    request = typed_peer.requests[0]
    assert _header_values(request, "anthropic-workspace-id") == []
    assert _header_values(request, "anthropic-beta") == []
    assert _header_values(request, "anthropic-version") == ["2023-06-01"]


@pytest.mark.parametrize("overrides,expected_beta,expected_tenant", [
    ([], "descriptor-beta", "original"),
    ([("anthropic-beta", "host-beta"), ("x-TENANT", "host-tenant")], "host-beta", "host-tenant"),
])
def test_deployment_header_precedence_is_case_insensitive_on_wire(typed_peer, overrides,
                                                                 expected_beta, expected_tenant):
    typed_peer.responder = _messages_response
    source = _descriptor_source(typed_peer, headers={"ANTHROPIC-BETA": "descriptor-beta",
                                                   "X-Tenant": "original"})
    environment = _environment("workspace-local", "environment-beta")
    provider = _deployment_provider(typed_peer, source, environment, overrides)
    # Admission captures host preprocessing; later changes do not mutate the admitted provider.
    environment.anthropic_workspace_id = "later-workspace"
    environment.anthropic_beta = "later-beta"
    _success(provider, _request(provider, _controls()))
    request = typed_peer.requests[0]
    assert _header_values(request, "anthropic-beta") == [expected_beta]
    assert _header_values(request, "anthropic-workspace-id") == ["workspace-local"]
    assert _header_values(request, "x-tenant") == [expected_tenant]


def test_messages_deployment_environment_does_not_leak_to_chat(typed_peer):
    typed_peer.responder = _chat_response
    provider = _deployment_provider(typed_peer, _descriptor_source(typed_peer, "openai.chat"),
                                    _environment("workspace-local", "beta-local"))
    _success(provider, _request(provider, _controls()))
    request = typed_peer.requests[0]
    assert _header_values(request, "anthropic-beta") == []
    assert _header_values(request, "anthropic-workspace-id") == []


@pytest.mark.parametrize("overrides,headers,workspace,beta", [
    ([("X-One", "a"), ("x-one", "b")], None, None, None),
    ([("Authorization", "forbidden-local")], None, None, None),
    ([("X-Tenant", "bad\r\nheader")], None, None, None),
    ([("bad header", "value")], None, None, None),
    ([("anthropic-version", "2099-01-01")], None, None, None),
    ([], {"X-One": "a", "x-one": "b"}, None, None),
    ([], {"Authorization": "forbidden-local"}, None, None),
    ([], None, "bad\nworkspace", None),
    ([], None, None, "bad\r\nbeta"),
])
def test_deployment_headers_reject_invalid_duplicate_or_reserved_before_io(typed_peer, overrides,
                                                                         headers, workspace, beta):
    with pytest.raises(ValueError):
        _deployment_provider(typed_peer, _descriptor_source(typed_peer, headers=headers),
                             _environment(workspace, beta), overrides)
    assert typed_peer.requests == []


@pytest.mark.parametrize("field", ["thinking_mode", "output_effort", "cache_control"])
def test_messages_native_history_binds_controls_but_allows_per_turn_choice_and_cap(typed_peer, field):
    typed_peer.responder = _messages_response
    provider = typed_peer.provider("anthropic.messages", policy=_policy(typed_peer))
    controls = _controls(max_output_tokens=2048, thinking_mode=ng.MessagesThinkingMode.Adaptive,
                         output_effort=ng.MessagesOutputEffort.High,
                         cache_control=_cache(ng.MessagesCacheTtl.OneHour),
                         messages_tool_choice=_choice(ng.MessagesToolChoiceMode.Auto, parallel=False))
    request = _request(provider, controls, tools=_tools())
    outcome = _success(provider, request)
    history = list(request.messages) + outcome.messages + [
        ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("Continue")])]
    controls.max_output_tokens = 4096
    controls.messages_tool_choice = _choice(ng.MessagesToolChoiceMode.Auto, parallel=True)
    _success(provider, _request(provider, controls, tools=_tools(), messages=history))
    assert typed_peer.requests[-1]["body"]["max_tokens"] == 4096
    assert typed_peer.requests[-1]["body"]["tool_choice"]["disable_parallel_tool_use"] is True
    changes = {"thinking_mode": ng.MessagesThinkingMode.Disabled,
               "output_effort": ng.MessagesOutputEffort.Low,
               "cache_control": _cache(ng.MessagesCacheTtl.FiveMinutes)}
    setattr(controls, field, changes[field])
    _rejected(typed_peer, provider, controls, tools=_tools(), messages=history,
              kind=ng.ProviderErrorKind.ReplayIneligible)
    assert len(typed_peer.requests) == 2


def test_messages_budget_without_mode_retains_manual_thinking(typed_peer):
    typed_peer.responder = _messages_response
    provider = typed_peer.provider("anthropic.messages", policy=_policy(typed_peer))
    controls = _controls(max_output_tokens=2048, thinking_budget=1024, top_p=0.95)
    _success(provider, _request(provider, controls))
    assert typed_peer.requests[0]["body"]["thinking"] == {"type": "enabled", "budget_tokens": 1024}


def test_chat_explicit_disabled_reasoning_and_empty_models_do_not_enable_fallback(typed_peer):
    typed_peer.responder = _chat_response
    provider = typed_peer.provider("openai.chat", policy=_policy(typed_peer))
    controls = _controls(chat_reasoning=_reasoning(enabled=False), include_reasoning=False,
                         usage_include=True, models=[])
    _success(provider, _request(provider, controls))
    body = typed_peer.requests[0]["body"]
    assert body["reasoning"] == {"enabled": False}
    assert body["include_reasoning"] is False
    assert body["usage"] == {"include": True}
    assert "models" not in body


@pytest.mark.parametrize("overrides,workspace", [
    ([], "workspace-local"),
    ([("ANTHROPIC-WORKSPACE-ID", "host-workspace")], "host-workspace"),
])
def test_deployment_environment_headers_are_injected_before_explicit_override(typed_peer, overrides,
                                                                            workspace):
    typed_peer.responder = _messages_response
    provider = _deployment_provider(typed_peer, _descriptor_source(typed_peer),
                                    _environment("workspace-local", "beta-local"), overrides)
    _success(provider, _request(provider, _controls()))
    request = typed_peer.requests[0]
    assert _header_values(request, "anthropic-workspace-id") == [workspace]
    assert _header_values(request, "anthropic-beta") == ["beta-local"]


def test_process_deployment_headers_are_captured_at_descriptor_admission(typed_peer, monkeypatch):
    typed_peer.responder = _messages_response
    monkeypatch.setenv("ANTHROPIC_WORKSPACE_ID", "synthetic-workspace-before")
    monkeypatch.setenv("ANTHROPIC_BETA", "synthetic-beta-before")
    admitted = ng.load_provider_descriptor_with_environment_headers(
        _descriptor_source(typed_peer), [("ANTHROPIC-BETA", "synthetic-host-beta")],
        _policy(typed_peer))
    options = ng.ProviderRuntimeOptions(ca_file=typed_peer.ca_file, default_timeout_ms=5000)
    options.http_version = ng.ProviderHttpVersion.Http1_1
    provider = nglm.SchemaProvider(admitted, options)
    _success(provider, _request(provider, _controls()))
    monkeypatch.setenv("ANTHROPIC_WORKSPACE_ID", "synthetic-workspace-after")
    monkeypatch.setenv("ANTHROPIC_BETA", "synthetic-beta-after")
    _success(provider, _request(provider, _controls()))
    assert len(typed_peer.requests) == 2
    for request in typed_peer.requests:
        assert _header_values(request, "anthropic-workspace-id") == ["synthetic-workspace-before"]
        assert _header_values(request, "anthropic-beta") == ["synthetic-host-beta"]
