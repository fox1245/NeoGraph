"""Live end-to-end checks of the Anthropic Messages request controls (tests/live/README.md).

Thinking modes and effort, tool choice, the temperature rule, prompt caching, and the same controls through
OpenRouter's Messages route. Nothing runs unless NEOGRAPH_E2E_LIVE=1 and the key variables are set.
"""

import pytest

import neograph_engine as ng

from live_support import Run, case, provider, user, weather_tool

TM, EF, TC = ng.MessagesThinkingMode, ng.MessagesOutputEffort, ng.MessagesToolChoiceMode
ROUTE = {"anthropic": ("anthropic.messages", "ANTHROPIC_API_KEY"),
         "openrouter": ("openrouter.messages", "OPENROUTER_API_KEY")}
ARITHMETIC = "What is 17*23? Answer with the number."


def _choice(mode, name=None):
    value = ng.MessagesToolChoice()
    value.mode = mode
    if name:
        value.name = name
    return value


def _run(route, model, prompt, stream, tools=(), **values):
    preset, _key = ROUTE[route]
    return Run(provider(preset, model), model, [user(prompt)], stream=stream, tools=tools, **values)


def _kinds(run):
    return [type(p).__name__ for p in run.parts()]


def _thinking(run):
    return [p for p in run.parts() if isinstance(p, ng.Thinking)]


def _tool_names(run):
    return [p.name for p in run.parts() if isinstance(p, ng.ProviderToolCall)]


def _modes(route):
    key = ROUTE[route][1]
    return [case(s, keys=[key], id=n) for n, s in (("collect", False), ("stream", True))]


@pytest.mark.parametrize("stream", _modes("anthropic"))
def test_adaptive_thinking_low_effort(stream):
    run = _run("anthropic", "claude-haiku-5-5", "Reply with one short word.", stream,
               max_output_tokens=400, thinking_mode=TM.Adaptive, output_effort=EF.Low)
    assert run.text().strip()


@pytest.mark.parametrize("stream", _modes("anthropic"))
def test_adaptive_thinking_high_effort(stream):
    run = _run("anthropic", "claude-haiku-5-5", ARITHMETIC, stream,
               max_output_tokens=800, thinking_mode=TM.Adaptive, output_effort=EF.High)
    assert "391" in run.text()


@pytest.mark.parametrize("stream", _modes("anthropic"))
def test_manual_thinking_budget_returns_signed_thinking(stream):
    run = _run("anthropic", "claude-haiku-4-5-20251001", ARITHMETIC, stream,
               max_output_tokens=2048, thinking_mode=TM.Manual, thinking_budget=1024)
    assert "391" in run.text()
    assert any(getattr(p, "signature", None) for p in _thinking(run))


@pytest.mark.parametrize("stream", _modes("anthropic"))
def test_disabled_thinking_has_no_thinking_part(stream):
    run = _run("anthropic", "claude-haiku-5-5", "Reply with one short word.", stream,
               max_output_tokens=64, thinking_mode=TM.Disabled)
    assert run.text().strip() and not _thinking(run)


@pytest.mark.parametrize("stream", _modes("anthropic"))
def test_tool_choice_named_tool(stream):
    run = _run("anthropic", "claude-haiku-5-5", "Hello there.", stream, tools=[weather_tool()],
               max_output_tokens=200, messages_tool_choice=_choice(TC.Tool, "get_weather"),
               thinking_mode=TM.Disabled)
    assert _tool_names(run) == ["get_weather"]


@pytest.mark.parametrize("stream", _modes("anthropic"))
def test_tool_choice_any(stream):
    run = _run("anthropic", "claude-haiku-4-5-20251001", "What's the weather in Paris?", stream,
               tools=[weather_tool()], max_output_tokens=200, messages_tool_choice=_choice(TC.Any))
    assert _tool_names(run)


@pytest.mark.parametrize("stream", _modes("anthropic"))
def test_tool_choice_none(stream):
    # Asked about the weather with tool use forbidden, the model may answer with no content at all, so the prompt
    # asks for something it can answer without the tool.
    run = _run("anthropic", "claude-haiku-4-5-20251001", "Say hello in one word.", stream,
               tools=[weather_tool()], max_output_tokens=100, messages_tool_choice=_choice(TC.None_))
    assert not _tool_names(run) and run.text().strip()


@pytest.mark.parametrize("stream", _modes("openrouter"))
def test_openrouter_manual_thinking_budget(stream):
    run = _run("openrouter", "anthropic/claude-haiku-4.5", ARITHMETIC, stream,
               max_output_tokens=2048, thinking_mode=TM.Manual, thinking_budget=1024)
    assert "391" in run.text() and _thinking(run)


@pytest.mark.parametrize("stream", _modes("openrouter"))
def test_openrouter_tool_choice_named_tool(stream):
    run = _run("openrouter", "anthropic/claude-haiku-4.5", "Hello there.", stream, tools=[weather_tool()],
               max_output_tokens=200, messages_tool_choice=_choice(TC.Tool, "get_weather"))
    assert _tool_names(run) == ["get_weather"]


@pytest.mark.parametrize("stream", _modes("anthropic")[:1])
def test_temperature_is_refused_before_dispatch_where_the_model_forbids_it(stream):
    run = _run("anthropic", "claude-haiku-5-5", "Reply with one short word.", stream,
               max_output_tokens=32, temperature=0.5)
    failure = run.outcome.failure
    assert failure is not None and failure.error.kind == ng.ProviderErrorKind.InvalidRequest
    assert not failure.error.http_status


@pytest.mark.parametrize("stream", _modes("anthropic")[:1])
def test_temperature_is_sent_where_the_model_allows_it(stream):
    run = _run("anthropic", "claude-haiku-4-5-20251001", "Reply with one short word.", stream,
               max_output_tokens=32, temperature=0.5)
    assert run.text().strip()


@pytest.mark.parametrize("stream", _modes("anthropic")[:1])
def test_prompt_cache_is_written_then_read(stream):
    system = ("You are a meticulous assistant for a weather service. " * 160).strip()
    cache = ng.MessagesCacheControl()
    cache.ttl = ng.MessagesCacheTtl.FiveMinutes
    values = dict(max_output_tokens=16, system=system, cache_control=cache)
    first = _run("anthropic", "claude-haiku-5-5", "Reply with one short word.", stream, **values).ok().usage
    second = _run("anthropic", "claude-haiku-5-5", "Reply with one short word.", stream, **values).ok().usage
    written = sum(v.value for v in (first.cache_write, first.cache_read) if v is not None)
    assert written > 0
    assert second.cache_read is not None and second.cache_read.value > 0
