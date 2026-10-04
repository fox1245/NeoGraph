"""Graph accounting retains known counts and missing provider usage."""

import pytest
import neograph_engine as ng


def _engine(provider):
    definition = {
        "name": "py_usage", "schema_version": 1,
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"llm": {"type": "llm_call"}},
        "edges": [{"from": ng.START_NODE, "to": "llm"},
                  {"from": "llm", "to": ng.END_NODE}],
    }
    return ng.GraphEngine.compile(definition, ng.NodeContext(provider=provider, model="local-model"))


def test_run_reports_token_usage(provider_peer):
    cfg = ng.RunConfig(thread_id="usage-single",
                       input={"messages": [{"role": "user", "content": "local request"}]})
    result = _engine(provider_peer.provider()).run(cfg)
    assert result.usage.input_total.value == 10
    assert result.usage.output_total.value == 5
    assert result.usage.total.value == 15


@pytest.mark.parametrize("stream", [False, True])
@pytest.mark.parametrize("reported", [None, {"prompt_tokens": 0, "completion_tokens": 0,
                                           "total_tokens": 0}])
def test_outcomes_distinguish_unknown_usage_from_zero(provider_peer, reported, stream):
    provider_peer.usage = reported
    engine = _engine(provider_peer.provider())
    cfg = ng.RunConfig(thread_id="usage-nullable")
    cfg.input = {"messages": [{"role": "user", "content": "local request"}]}
    result = (engine.run_stream(cfg, lambda _event: None) if stream else
              engine.run(cfg))
    outcome, = result.provider_outcomes
    usage = outcome.completion.usage
    if reported is None:
        assert usage.input_total is None
        assert usage.output_total is None
        assert usage.total is None
    else:
        assert usage.input_total.value == 0
        assert usage.output_total.value == 0
        assert usage.total.value == 0


def test_a_graph_with_no_llm_has_no_provider_report():
    definition = {
        "name": "py_no_llm", "channels": {"x": {"reducer": "overwrite"}},
        "nodes": {}, "edges": [{"from": ng.START_NODE, "to": ng.END_NODE}],
    }
    result = ng.GraphEngine.compile(definition, ng.NodeContext()).run(
        ng.RunConfig(thread_id="usage-empty"))
    assert result.usage.total is None
    assert result.provider_outcomes == []


@pytest.mark.parametrize("stream", [False, True])
@pytest.mark.parametrize("detail_state", ["reported", "missing", "zero"])
def test_graph_and_accumulator_fold_real_cache_reasoning_reports(
        provider_peer, stream, detail_state):
    engine = _engine(provider_peer.provider())
    accumulator = ng.UsageAccumulator()
    reports = [
        {"prompt_tokens": 20, "completion_tokens": 8, "total_tokens": 28,
         "prompt_tokens_details": {"cached_tokens": 3, "cache_write_tokens": 2},
         "completion_tokens_details": {"reasoning_tokens": 4}},
        {"prompt_tokens": 30, "completion_tokens": 12, "total_tokens": 42,
         "prompt_tokens_details": {"cached_tokens": 5, "cache_write_tokens": 4},
         "completion_tokens_details": {"reasoning_tokens": 6}},
    ]
    expected = {"input_total": 50, "output_total": 20, "total": 70,
                "cache_read": 8, "cache_write": 6, "reasoning": 10,
                "input_uncached": 36}
    if detail_state == "missing":
        reports[1].pop("prompt_tokens_details")
        reports[1].pop("completion_tokens_details")
        expected.update(cache_read=None, cache_write=None, reasoning=None,
                        input_uncached=None)
    elif detail_state == "zero":
        for report in reports:
            report["prompt_tokens_details"] = {"cached_tokens": 0, "cache_write_tokens": 0}
            report["completion_tokens_details"] = {"reasoning_tokens": 0}
        expected.update(cache_read=0, cache_write=0, reasoning=0, input_uncached=50)

    results = []
    for turn, report in enumerate(reports):
        provider_peer.usage = report
        provider_peer.reply = f"answer-{turn}"
        provider_peer.stream_chunks = [f"answer-{turn}"]
        cfg = ng.RunConfig(
            thread_id=f"usage-details-{turn}",
            input={"messages": [{"role": "user", "content": f"distinct-input-{turn}"}]},
        )
        cfg.usage = accumulator
        result = (engine.run_stream(cfg, lambda _event: None) if stream else
                  engine.run(cfg))
        assert result.output["channels"]["messages"]["value"][-1]["content"] == f"answer-{turn}"
        results.append(result)

    assert len(provider_peer.requests) == 2
    assert [request[0]["text"] for request in provider_peer.logical_requests] == [
        "distinct-input-0", "distinct-input-1",
    ]
    assert [result.provider_outcomes[-1].text for result in results] == ["answer-0", "answer-1"]
    for usage in (results[-1].usage, accumulator.snapshot()):
        for name, value in expected.items():
            count = getattr(usage, name)
            if value is None:
                assert count is None
            else:
                assert count.value == value
    assert accumulator.authority_snapshot().charged == 70
