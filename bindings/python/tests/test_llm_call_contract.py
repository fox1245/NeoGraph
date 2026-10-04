"""Consumer-visible llm_call prompt assembly at the real provider boundary."""

import pytest

import neograph_engine as ng


def _definition(node):
    return {
        "name": "llm-call-contract", "schema_version": 1,
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"answer": node},
        "edges": [{"from": ng.START_NODE, "to": "answer"},
                  {"from": "answer", "to": ng.END_NODE}],
    }


def test_llm_call_uses_node_context_instructions(provider_peer):
    context = ng.NodeContext(
        provider=provider_peer.provider(), model="context-model",
        instructions="Context system prompt",
    )
    engine = ng.GraphEngine.compile(_definition({"type": "llm_call"}), context)
    result = engine.run(ng.RunConfig(
        thread_id="llm-call-contract",
        input={"messages": [{"role": "user", "content": "ping"}]},
    ))
    assert result.output["channels"]["messages"]["value"][-1]["content"] == "ok"
    request, = provider_peer.requests
    assert request["model"] == "context-model"
    assert [(message["role"], message["text"])
            for message in provider_peer.logical_requests[0]] == [
        ("system", "Context system prompt"), ("user", "ping"),
    ]


def test_strict_llm_call_rejects_per_node_system_prompt(provider_peer):
    context = ng.NodeContext(provider=provider_peer.provider())
    with pytest.raises(RuntimeError):
        ng.GraphEngine.compile(_definition({
            "type": "llm_call", "config": {"system": "ignored"},
        }), context)
    assert provider_peer.requests == []
