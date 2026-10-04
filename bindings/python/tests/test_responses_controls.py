"""Responses controls and cursor custody through the real SDK and localhost TLS."""

import copy
import json

import pytest
import neograph_engine as ng
from neograph_engine.llm import SchemaProvider


MODEL = "local-responses-model"
USAGE = {"input_tokens": 10, "output_tokens": 7, "total_tokens": 17}
INCLUDES = [
    (ng.ResponsesInclude.ReasoningEncryptedContent, "reasoning.encrypted_content"),
    (ng.ResponsesInclude.WebSearchSources, "web_search_call.action.sources"),
    (ng.ResponsesInclude.FileSearchResults, "file_search_call.results"),
    (ng.ResponsesInclude.MessageOutputTextLogprobs, "message.output_text.logprobs"),
    (ng.ResponsesInclude.ComputerCallOutputImageUrl, "computer_call_output.output.image_url"),
    (ng.ResponsesInclude.CodeInterpreterCallOutputs, "code_interpreter_call.outputs"),
]


def _user(text):
    return ng.ProviderMessage(ng.ProviderRole.User, [ng.Text(text)])


def _result(call_id, text):
    return ng.ProviderMessage(ng.ProviderRole.Tool, [ng.ProviderToolResult(call_id, text)])


def _message(text):
    return {"id": "msg_answer", "type": "message", "status": "completed",
            "role": "assistant", "phase": "final_answer",
            "content": [{"type": "output_text", "text": text, "annotations": []}]}


def _call(call_id, x):
    return {"id": "fc_" + call_id, "type": "function_call", "status": "completed",
            "call_id": call_id, "name": "lookup", "arguments": json.dumps({"x": x})}


def _tools():
    return [ng.ChatTool("lookup", "Find a value", {
        "type": "object", "properties": {"x": {"type": "integer"}},
        "required": ["x"], "additionalProperties": False,
    })]


def _controls():
    controls = ng.ProviderControls()
    controls.max_output_tokens = 128
    controls.store = True
    controls.parallel_tool_calls = False
    controls.verbosity = ng.ResponsesVerbosity.High
    controls.truncation = ng.ResponsesTruncation.Auto
    controls.responses_include = []
    return controls


def _request(provider, messages, controls=None, mode=ng.ProviderMode.Collect, tools=None,
             model=MODEL):
    return ng.make_provider_request(provider, model, messages, tools=tools or [],
                                    controls=controls or ng.ProviderControls(), mode=mode)


def _envelope(body, output, cursor="resp_first", status="completed"):
    return {"id": cursor, "object": "response", "created_at": 1, "model": body["model"],
            "status": status, "output": output, "usage": USAGE if status != "in_progress" else None,
            "incomplete_details": None, "error": None}


def _frames(body, output, cursor):
    """Actual Responses SSE grammar, with item snapshots and terminal envelope."""
    frames = []

    def frame(kind, **fields):
        payload = {"type": kind, "sequence_number": len(frames), **fields}
        frames.append(f"event: {kind}\ndata: {json.dumps(payload)}\n\n")

    frame("response.created", response=_envelope(body, [], cursor, "in_progress"))
    for index, item in enumerate(output):
        if item["type"] == "message":
            initial = {**item, "status": "in_progress", "content": []}
        elif item["type"] == "function_call":
            initial = {**item, "status": "in_progress", "arguments": ""}
        else:
            initial = item
        frame("response.output_item.added", output_index=index, item=initial)
        owner = {"item_id": item["id"], "output_index": index}
        if item["type"] == "message":
            for part_index, part in enumerate(item["content"]):
                part_owner = {**owner, "content_index": part_index}
                frame("response.content_part.added", **part_owner, part={**part, "text": ""})
                frame("response.output_text.delta", **part_owner, delta=part["text"])
                frame("response.output_text.done", **part_owner, text=part["text"])
                frame("response.content_part.done", **part_owner, part=part)
        elif item["type"] == "function_call":
            frame("response.function_call_arguments.delta", **owner, delta=item["arguments"])
            frame("response.function_call_arguments.done", **owner, arguments=item["arguments"])
        frame("response.output_item.done", output_index=index, item=item)
    frame("response.completed", response=_envelope(body, output, cursor))
    return "".join(frames).encode()


def _reply(request, output, cursor="resp_first"):
    body = request["body"]
    if body["stream"]:
        return 200, _frames(body, output, cursor), "text/event-stream"
    return 200, _envelope(body, output, cursor), "application/json"


def _completed(outcome):
    assert outcome.failure is None, outcome.failure.error.safe_message if outcome.failure else ""
    assert outcome.completion.usage.total.value == 17
    return outcome.completion


def _no_send(peer, provider, request, kind):
    before = len(peer.requests)
    outcome = provider.invoke(request)
    assert outcome.completion is None
    assert outcome.failure.error.kind == kind
    assert not outcome.failure.error.attempt.request_may_have_left
    assert outcome.failure.error.attempt.request_body_bytes == 0
    assert len(peer.requests) == before
    return outcome


@pytest.mark.parametrize("mode", [ng.ProviderMode.Collect, ng.ProviderMode.Stream])
@pytest.mark.parametrize("verbosity,expected", [
    (ng.ResponsesVerbosity.Low, {"answer": "blue"}),
    (ng.ResponsesVerbosity.Medium, {"answer": "blue", "reason": "sky"}),
    (ng.ResponsesVerbosity.High, {"answer": "blue", "reason": "sky", "detail": "daylight"}),
])
def test_verbosity_coexists_with_structured_text(typed_peer, mode, verbosity, expected):
    answers = {"low": {"answer": "blue"},
               "medium": {"answer": "blue", "reason": "sky"},
               "high": {"answer": "blue", "reason": "sky", "detail": "daylight"}}

    def respond(request):
        text = request["body"]["text"]
        if text.get("format", {}).get("type") != "json_object":
            return 400, {"error": {"type": "invalid_request_error", "message": "JSON mode required"}}, "application/json"
        return _reply(request, [_message(json.dumps(answers[text["verbosity"]]))])

    typed_peer.responder = respond
    provider = typed_peer.provider("openai.responses")
    controls = ng.ProviderControls()
    controls.verbosity = verbosity
    controls.response_format = ng.ProviderResponseFormat()
    outcome = provider.invoke(_request(provider, [_user("Explain the color")], controls, mode))
    completion = _completed(outcome)
    assert json.loads(outcome.text) == expected
    assert completion.messages[0].native.complete
    assert completion.messages[0].wire_output[0]["phase"] == "final_answer"


@pytest.mark.parametrize("parallel,call_ids", [
    (None, ["call_one", "call_two"]), (False, ["call_one"]), (True, ["call_one", "call_two"]),
])
@pytest.mark.parametrize("mode", [ng.ProviderMode.Collect, ng.ProviderMode.Stream])
def test_parallel_tool_selection_changes_pending_client_work(typed_peer, parallel, call_ids, mode):
    def respond(request):
        body = request["body"]
        if body["input"][0].get("type") == "function_call_output":
            values = [item["output"] for item in body["input"]]
            return _reply(request, [_message("+".join(values))], "resp_finished")
        calls = [_call("call_one", 1)]
        if body.get("parallel_tool_calls", True):
            calls.append(_call("call_two", 2))
        return _reply(request, calls)

    typed_peer.responder = respond
    provider = typed_peer.provider("openai.responses")
    controls = _controls()
    controls.parallel_tool_calls = parallel
    prefix = [_user("Look up both values")]
    first = _completed(provider.invoke(_request(provider, prefix, controls, mode, _tools())))
    assert first.stop.kind == ng.ProviderStopKind.ToolUse
    calls = [part for part in first.messages[0].parts if isinstance(part, ng.ProviderToolCall)]
    assert [call.id for call in calls] == call_ids
    assert [call.input["x"] for call in calls] == list(range(1, len(call_ids) + 1))
    controls.previous_response_id = first.messages[0].id
    controls.previous_response_history = prefix + first.messages
    outcome = provider.invoke(_request(provider, [_result(call.id, str(call.input["x"])) for call in calls],
                                       controls, mode, _tools()))
    _completed(outcome)
    assert outcome.text == ("1" if parallel is False else "1+2")
    assert typed_peer.requests[-1]["body"]["input"] == [
        {"type": "function_call_output", "call_id": call_id, "output": str(index)}
        for index, call_id in enumerate(call_ids, 1)
    ]


@pytest.mark.parametrize("selection", [None, ng.ResponsesTruncation.Disabled, ng.ResponsesTruncation.Auto])
def test_truncation_controls_context_overflow_instead_of_silent_success(typed_peer, selection):
    def respond(request):
        body = request["body"]
        inputs = body["input"]
        if len(inputs) > 2 and body.get("truncation", "disabled") != "auto":
            return 400, {"error": {"type": "invalid_request_error", "code": "context_length_exceeded",
                                    "message": "Local context bound exceeded"}}, "application/json"
        retained = inputs[-2:]
        return _reply(request, [_message("|".join(item["content"][0]["text"] for item in retained))])

    typed_peer.responder = respond
    provider = typed_peer.provider("openai.responses")
    controls = ng.ProviderControls()
    controls.truncation = selection
    outcome = provider.invoke(_request(provider, [_user("old"), _user("recent"), _user("question")], controls))
    if selection == ng.ResponsesTruncation.Auto:
        _completed(outcome)
        assert outcome.text == "recent|question"
    else:
        assert outcome.completion is None
        assert outcome.failure.error.http_status == 400
        error_body = next(event.payload for event in outcome.failure.partial.raw_events
                          if event.type == "http.error")
        assert error_body["error"]["code"] == "context_length_exceeded"
        assert outcome.failure.error.attempt.request_may_have_left
    body = typed_peer.requests[0]["body"]
    if selection is None:
        assert "truncation" not in body
    else:
        assert body["truncation"] == ("auto" if selection == ng.ResponsesTruncation.Auto else "disabled")


@pytest.mark.parametrize("selection", ["default", "empty", "all"])
def test_include_selection_preserves_observable_reasoning_and_server_metadata(typed_peer, selection):
    def respond(request):
        includes = request["body"]["include"]
        reasoning = {"id": "rs_one", "type": "reasoning", "status": "completed",
                     "summary": [{"type": "summary_text", "text": "Use source evidence"}]}
        if "reasoning.encrypted_content" in includes:
            reasoning["encrypted_content"] = "local-encrypted-evidence"
        web = {"id": "ws_one", "type": "web_search_call", "status": "completed",
               "action": {"type": "search", "query": "color"}}
        files = {"id": "fs_one", "type": "file_search_call", "status": "completed", "queries": ["color"]}
        code = {"id": "ci_one", "type": "code_interpreter_call", "status": "completed", "code": "1 + 1"}
        message = _message("blue")
        if "web_search_call.action.sources" in includes:
            web["action"]["sources"] = [{"url": "https://example.invalid/color"}]
        if "file_search_call.results" in includes:
            files["results"] = [{"file_id": "file_local", "text": "blue"}]
        if "message.output_text.logprobs" in includes:
            message["content"][0]["logprobs"] = [{"token": "blue", "logprob": -0.1, "bytes": [98, 108, 117, 101], "top_logprobs": []}]
        if "code_interpreter_call.outputs" in includes:
            code["outputs"] = [{"type": "logs", "logs": "2"}]
        return _reply(request, [reasoning, web, files, code, message])

    typed_peer.responder = respond
    provider = typed_peer.provider("openai.responses")
    controls = ng.ProviderControls()
    controls.responses_include = None if selection == "default" else ([] if selection == "empty" else [value for value, _ in INCLUDES])
    outcome = provider.invoke(_request(provider, [_user("Find the color")], controls))
    completion = _completed(outcome)
    assert outcome.text == "blue"
    owner = completion.messages[0]
    reason = next(part for part in owner.parts if isinstance(part, ng.Reasoning))
    assert reason.summary == ["Use source evidence"]
    assert reason.encrypted_content == (None if selection == "empty" else "local-encrypted-evidence")
    output = {item["type"]: item for item in owner.wire_output}
    if selection == "all":
        assert output["web_search_call"]["action"]["sources"][0]["url"] == "https://example.invalid/color"
        assert output["file_search_call"]["results"][0]["text"] == "blue"
        assert output["code_interpreter_call"]["outputs"][0]["logs"] == "2"
        assert output["message"]["content"][0]["logprobs"][0]["logprob"] == -0.1
    else:
        assert "sources" not in output["web_search_call"]["action"]
        assert "logprobs" not in output["message"]["content"][0]
    assert typed_peer.requests[0]["body"]["include"] == (
        ["reasoning.encrypted_content"] if selection == "default" else
        [] if selection == "empty" else [wire for _, wire in INCLUDES])


@pytest.mark.parametrize("family", ["openai.chat", "anthropic.messages", "google.generate"])
@pytest.mark.parametrize("field,value", [
    ("verbosity", ng.ResponsesVerbosity.Low),
    ("truncation", ng.ResponsesTruncation.Disabled),
    ("parallel_tool_calls", False),
    ("responses_include", []),
    ("previous_response_id", "resp_owned"),
    ("previous_response_history", [_user("original prefix")]),
])
def test_responses_controls_reject_wrong_family_before_io(typed_peer, family, field, value):
    provider = typed_peer.provider(family)
    controls = ng.ProviderControls()
    setattr(controls, field, value)
    with pytest.raises(ValueError):
        _request(provider, [_user("new input")], controls)
    assert typed_peer.requests == []


class _Conversation:
    """Server-held transcript: requests must contain only the new turn."""

    def __init__(self, tools=False):
        self.history = {}
        self.tools = tools

    def __call__(self, request):
        body = request["body"]
        previous = body.get("previous_response_id")
        if previous is not None and previous not in self.history:
            return 404, {"error": {"type": "invalid_request_error", "code": "response_not_found",
                                    "message": "Unknown response cursor"}}, "application/json"
        prior = self.history.get(previous, [])
        inputs = body["input"]
        if self.tools:
            if not prior:
                assert inputs == [{"role": "user", "content": [{"type": "input_text", "text": "Lookup values"}]}]
                output = [_call("call_one", 1)]
            else:
                pending = next(item for item in reversed(prior) if item.get("type") == "function_call")
                expected = "one" if pending["call_id"] == "call_one" else "two"
                assert inputs == [{"type": "function_call_output", "call_id": pending["call_id"], "output": expected}]
                values = [item["output"] for item in prior + inputs if item.get("type") == "function_call_output"]
                output = [_call("call_two", 2)] if len(values) == 1 else [_message("+".join(values))]
        else:
            assert len(inputs) == 1 and inputs[0]["role"] == "user"
            question = inputs[0]["content"][0]["text"]
            users = [item["content"][0]["text"] for item in prior if item.get("role") == "user"]
            answer = "remembered" if not prior else users[0].removeprefix("Remember ") if question == "What color?" else users[-1]
            output = [_message(answer)]
        cursor = f"resp_state_{len(self.history) + 1}"
        self.history[cursor] = copy.deepcopy(prior + inputs + output)
        return _reply(request, output, cursor)


@pytest.mark.parametrize("mode", [ng.ProviderMode.Collect, ng.ProviderMode.Stream])
def test_cursor_text_chain_uses_server_history_not_client_replay(typed_peer, mode):
    conversation = _Conversation()
    typed_peer.responder = conversation
    provider = typed_peer.provider("openai.responses")
    controls = _controls()
    first = _completed(provider.invoke(_request(provider, [_user("Remember blue")], controls, mode)))
    assert first.messages[0].parts[0].value == "remembered"
    controls.previous_response_id = first.messages[0].id
    next_mode = ng.ProviderMode.Stream if mode == ng.ProviderMode.Collect else ng.ProviderMode.Collect
    second_outcome = provider.invoke(_request(provider, [_user("What color?")], controls, next_mode))
    second = _completed(second_outcome)
    assert second_outcome.text == "blue"
    assert second.messages[0].native is not None and not second.messages[0].native.complete
    _no_send(typed_peer, provider, _request(provider, [_user("Remember blue"), second.messages[0]], _controls()),
             ng.ProviderErrorKind.ReplayIneligible)
    controls.previous_response_id = second.messages[0].id
    third = provider.invoke(_request(provider, [_user("What did I ask?")], controls, mode))
    _completed(third)
    assert third.text == "What color?"
    assert len(conversation.history) == 3
    assert [request["body"]["input"][0]["content"][0]["text"] for request in typed_peer.requests] == [
        "Remember blue", "What color?", "What did I ask?",
    ]
    controls.previous_response_id = "resp_unknown"
    unknown = provider.invoke(_request(provider, [_user("What color?")], controls, mode))
    assert unknown.completion is None
    assert unknown.failure.error.http_status == 404
    error_body = next(event.payload for event in unknown.failure.partial.raw_events
                      if event.type == "http.error")
    assert error_body["error"]["code"] == "response_not_found"
    assert unknown.failure.error.attempt.request_may_have_left
    assert len(typed_peer.requests) == 4 and len(conversation.history) == 3


@pytest.mark.parametrize("cursor", ["", "resp bad", "resp\nbad", "a" * 257, "resp/é"])
def test_invalid_cursor_rejects_before_io(typed_peer, cursor):
    provider = typed_peer.provider("openai.responses")
    controls = _controls()
    controls.previous_response_id = cursor
    _no_send(typed_peer, provider, _request(provider, [_user("new turn")], controls), ng.ProviderErrorKind.InvalidRequest)


@pytest.mark.parametrize("mode", [ng.ProviderMode.Collect, ng.ProviderMode.Stream])
def test_authentic_cursor_client_calls_chain_without_full_replay_authority(typed_peer, mode):
    conversation = _Conversation(tools=True)
    typed_peer.responder = conversation
    provider = typed_peer.provider("openai.responses")
    controls = _controls()
    prefix = [_user("Lookup values")]
    first = _completed(provider.invoke(_request(provider, prefix, controls, mode, _tools())))
    assert first.stop.kind == ng.ProviderStopKind.ToolUse and first.messages[0].native.complete
    controls.previous_response_history = prefix + first.messages
    _no_send(typed_peer, provider, _request(provider, [_result("call_one", "one")], controls, mode, _tools()),
             ng.ProviderErrorKind.InvalidRequest)
    controls.previous_response_id = first.messages[0].id
    controls.previous_response_history = prefix + first.messages
    _no_send(typed_peer, provider, _request(provider, prefix + first.messages + [_result("call_one", "one")],
                                          controls, mode, _tools()),
             ng.ProviderErrorKind.ReplayIneligible)
    second = _completed(provider.invoke(_request(provider, [_result("call_one", "one")], controls, mode, _tools())))
    assert second.stop.kind == ng.ProviderStopKind.ToolUse
    assert second.messages[0].parts[0].id == "call_two"
    assert not second.messages[0].native.complete
    _no_send(typed_peer, provider, _request(provider, prefix + first.messages + [_result("call_one", "one")] + second.messages + [_result("call_two", "two")], _controls(), mode, _tools()),
             ng.ProviderErrorKind.ReplayIneligible)
    controls.previous_response_id = second.messages[0].id
    controls.previous_response_history = second.messages
    edited = second.messages
    parts = edited[0].parts
    parts[0].id = "call_edited"
    edited[0].parts = parts
    controls.previous_response_history = edited
    _no_send(typed_peer, provider, _request(provider, [_result("call_two", "two")], controls, mode, _tools()),
             ng.ProviderErrorKind.ReplayIneligible)
    controls.previous_response_history = second.messages
    _no_send(typed_peer, provider, _request(provider, [_result("call_one", "one")], controls, mode, _tools()),
             ng.ProviderErrorKind.InvalidRequest)
    final = provider.invoke(_request(provider, [_result("call_two", "two")], controls, mode, _tools()))
    _completed(final)
    assert final.text == "one+two"
    assert len(conversation.history) == 3 and len(typed_peer.requests) == 3
    assert [request["body"]["input"] for request in typed_peer.requests[1:]] == [
        [{"type": "function_call_output", "call_id": "call_one", "output": "one"}],
        [{"type": "function_call_output", "call_id": "call_two", "output": "two"}],
    ]
    assert all("previous_response_history" not in request["body"] for request in typed_peer.requests)


@pytest.mark.parametrize("mutation,kind", [
    ("missing_history", ng.ProviderErrorKind.InvalidRequest),
    ("omitted_prefix", ng.ProviderErrorKind.ReplayIneligible),
    ("edited_prefix", ng.ProviderErrorKind.ReplayIneligible),
    ("cursor_mismatch", ng.ProviderErrorKind.ReplayIneligible),
    ("edited_call", ng.ProviderErrorKind.ReplayIneligible),
    ("caller_reconstruction", ng.ProviderErrorKind.ReplayIneligible),
    ("verbosity", ng.ProviderErrorKind.ReplayIneligible),
    ("include", ng.ProviderErrorKind.ReplayIneligible),
    ("model", ng.ProviderErrorKind.ReplayIneligible),
    ("route", ng.ProviderErrorKind.ReplayIneligible),
    ("origin", ng.ProviderErrorKind.ReplayIneligible),
    ("orphan_result", ng.ProviderErrorKind.InvalidRequest),
    ("duplicate_result", ng.ProviderErrorKind.InvalidRequest),
])
def test_cursor_client_result_requires_matching_authentic_ownership(typed_peer, mutation, kind):
    typed_peer.responder = _Conversation(tools=True)
    provider = typed_peer.provider("openai.responses")
    prefix = [_user("Lookup values")]
    first = _completed(provider.invoke(_request(provider, prefix, _controls(), tools=_tools())))
    controls = _controls()
    controls.previous_response_id = first.messages[0].id
    history = prefix + first.messages
    inputs = [_result("call_one", "one")]
    model = MODEL
    if mutation == "missing_history":
        history = []
    elif mutation == "omitted_prefix":
        history = first.messages
    elif mutation == "edited_prefix":
        history[0] = _user("Altered original request")
    elif mutation == "cursor_mismatch":
        controls.previous_response_id = "resp_other"
    elif mutation == "edited_call":
        parts = history[-1].parts
        parts[0].id = "call_foreign"
        history[-1].parts = parts
    elif mutation == "caller_reconstruction":
        history[-1] = ng.ProviderMessage(ng.ProviderRole.Assistant, history[-1].parts, history[-1].id)
    elif mutation == "verbosity":
        controls.verbosity = ng.ResponsesVerbosity.Low
    elif mutation == "include":
        controls.responses_include = None
    elif mutation == "model":
        model = "another-model"
    elif mutation == "route":
        provider = typed_peer.provider("openai.responses", paths={"buffered": "/other", "streaming": "/other"})
    elif mutation == "origin":
        descriptor = ng.load_provider_descriptor(json.dumps({
            "descriptor_version": 1, "revision": 1, "id": "foreign-local-responses",
            "family": "openai.responses",
            "connection": {"base_url": typed_peer.base_url.replace("127.0.0.1", "localhost"),
                           "paths": {"buffered": "/operation", "streaming": "/operation"}},
        }))
        options = ng.ProviderRuntimeOptions(ca_file=typed_peer.ca_file, default_timeout_ms=5000)
        options.http_version = ng.ProviderHttpVersion.Http1_1
        provider = SchemaProvider(descriptor, options)
    elif mutation == "orphan_result":
        inputs = [_result("call_foreign", "one")]
    elif mutation == "duplicate_result":
        inputs = inputs + inputs
    controls.previous_response_history = history
    _no_send(typed_peer, provider, _request(provider, inputs, controls, tools=_tools(), model=model), kind)
    assert len(typed_peer.requests) == 1
    assert first.messages[0].parts[0].id == "call_one"


def test_failed_terminal_output_cannot_authorize_cursor_client_result(typed_peer):
    def respond(request):
        wire = _frames(request["body"], [_call("call_one", 1)], "resp_failed")
        error = {"type": "error", "error": {"code": "server_error", "message": "Local failure after terminal snapshot"}}
        return 200, wire + f"event: error\ndata: {json.dumps(error)}\n\n".encode(), "text/event-stream"

    typed_peer.responder = respond
    provider = typed_peer.provider("openai.responses")
    prefix = [_user("Lookup values")]
    failed = provider.invoke(_request(provider, prefix, _controls(), ng.ProviderMode.Stream, _tools()))
    assert failed.completion is None
    assert failed.failure.error.kind == ng.ProviderErrorKind.RemoteFailure
    partial = failed.failure.partial.messages
    assert partial[0].parts[0].id == "call_one"
    assert partial[0].native is None or not partial[0].native.complete
    controls = _controls()
    controls.previous_response_id = partial[0].id
    controls.previous_response_history = prefix + partial
    _no_send(typed_peer, provider, _request(provider, [_result("call_one", "one")], controls, tools=_tools()),
             ng.ProviderErrorKind.ReplayIneligible)
    assert len(typed_peer.requests) == 1


@pytest.mark.parametrize("include_encrypted", [False, True])
def test_explicit_include_does_not_hide_vendor_replay_rejection(typed_peer, include_encrypted):
    def respond(request):
        body = request["body"]
        replayed = [item for item in body["input"] if item.get("type") == "reasoning"]
        if replayed:
            if not replayed[0].get("encrypted_content"):
                return 400, {"error": {"type": "invalid_request_error", "code": "invalid_encrypted_content",
                                        "message": "Encrypted replay evidence is absent"}}, "application/json"
            return _reply(request, [_message("replayed")], "resp_replayed")
        reasoning = {"id": "rs_initial", "type": "reasoning", "status": "completed",
                     "summary": [{"type": "summary_text", "text": "Remember the answer"}]}
        if "reasoning.encrypted_content" in body["include"]:
            reasoning["encrypted_content"] = "local-encrypted-replay"
        return _reply(request, [reasoning, _message("blue")])

    typed_peer.responder = respond
    provider = typed_peer.provider("openai.responses")
    controls = ng.ProviderControls()
    controls.responses_include = None if include_encrypted else []
    prefix = [_user("Remember blue")]
    first = _completed(provider.invoke(_request(provider, prefix, controls)))
    history = prefix + first.messages + [_user("What color?")]
    changed = ng.ProviderControls()
    changed.responses_include = [] if include_encrypted else None
    _no_send(typed_peer, provider, _request(provider, history, changed), ng.ProviderErrorKind.ReplayIneligible)
    replay = provider.invoke(_request(provider, history, controls))
    if include_encrypted:
        _completed(replay)
        assert replay.text == "replayed"
    else:
        assert replay.completion is None
        assert replay.failure.error.http_status == 400
        error_body = next(event.payload for event in replay.failure.partial.raw_events
                          if event.type == "http.error")
        assert error_body["error"]["code"] == "invalid_encrypted_content"
        assert replay.failure.error.attempt.request_may_have_left
    assert len(typed_peer.requests) == 2


def test_duplicate_include_selection_is_rejected_before_io(typed_peer):
    provider = typed_peer.provider("openai.responses")
    controls = ng.ProviderControls()
    controls.responses_include = [ng.ResponsesInclude.FileSearchResults, ng.ResponsesInclude.FileSearchResults]
    _no_send(typed_peer, provider, _request(provider, [_user("Find the color")], controls),
             ng.ProviderErrorKind.InvalidRequest)
