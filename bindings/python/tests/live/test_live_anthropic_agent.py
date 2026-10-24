"""Live end-to-end agent runs on Anthropic Messages (tests/live/README.md).

The cases run the way an application runs a model: a GraphEngine whose llm_call and tool_dispatch nodes loop until
the model stops asking for tools, Python tools, and one checkpointed thread per conversation. They check what a single
request cannot: tool calls that depend on each other, a conversation that must remember its first turn, signed and
redacted thinking carried through tool rounds, parallel tool calls, tool errors, a person who reviews or approves tool
calls, prompt caching over a conversation, and the same loops streamed, asynchronous and through OpenRouter.
Assertions check structure, call order and arguments, computed values and recalled facts, not prose. Nothing runs
unless NEOGRAPH_E2E_LIVE=1 and the key variables are set.
"""

import asyncio
import json
import re
import uuid

import pytest

import neograph_engine as ng

from live_support import TEXT_MODELS, Run, case, case_model_id, controls, live_model, provider, user, weather_tool

TM = ng.MessagesThinkingMode
ANTHROPIC, OPENROUTER = "anthropic.messages", "openrouter.messages"
ADAPTIVE = "anthropic.messages.adaptive"
KEYS = {ANTHROPIC: "ANTHROPIC_API_KEY", OPENROUTER: "OPENROUTER_API_KEY"}
MODES = (("collect", False), ("stream", True))
STR, INT = {"type": "string"}, {"type": "integer"}
# Manual thinking needs a budget of at least 1024 tokens and an output cap above it.
THINKING_CONTROLS = dict(max_output_tokens=4000, thinking_mode=TM.Manual, thinking_budget=1024)

# A model loop: the model answers, and while its answer asks for tools the tools run and the model answers again.
AGENT = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION, "name": "live_agent",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {"llm": {"type": "llm_call"}, "tools": {"type": "tool_dispatch"}},
    "edges": [{"from": ng.START_NODE, "to": "llm"}, {"from": "tools", "to": "llm"}],
    "conditional_edges": [{"from": "llm", "condition": "has_tool_calls",
                           "routes": {"true": "tools", "false": ng.END_NODE}}],
}


class Tools:
    """Python tools that record every call that ran, in order, as (name, arguments, result)."""

    def __init__(self):
        self.calls = []

    def make(self, name, description, properties, handler):
        log = self.calls

        class _Tool(ng.Tool):
            def get_name(self):
                return name

            def get_definition(self):
                return ng.ChatTool(name=name, description=description, parameters={
                    "type": "object", "properties": properties, "required": list(properties)})

            def execute(self, arguments):
                try:
                    result = handler(arguments)
                except Exception as error:
                    log.append((name, dict(arguments), f"raised {type(error).__name__}"))
                    raise
                log.append((name, dict(arguments), result))
                return result

        return _Tool()

    def names(self):
        return [name for name, _arguments, _result in self.calls]

    def arguments(self, name):
        return [arguments for called, arguments, _result in self.calls if called == name]


# The same loop that stops before every tool dispatch, so that a person can look at the call first.
REVIEWED = {**AGENT, "name": "live_reviewed_agent", "interrupt_before": ["tools"]}


def _agent(preset, model, tools=(), instructions="", graph=AGENT, **control_values):
    context = ng.NodeContext(provider=provider(preset, model), model=model, tools=list(tools),
                             instructions=instructions, provider_controls=controls(**control_values))
    engine = ng.GraphEngine.compile(graph, context)
    engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
    return engine


def _thread():
    return "live-" + uuid.uuid4().hex[:12]


class Turn:
    """One run of the agent graph on a thread: the result, and for a stream the graph's token events.

    `ask` is the user's text added to the thread's history; `history` instead replaces the history with typed
    provider messages the caller carried over from an earlier run.
    """

    def __init__(self, engine, thread, ask=None, stream=False, max_steps=30, history=None, asynchronous=False):
        self.tokens = []
        config = ng.RunConfig(thread_id=thread, max_steps=max_steps, resume_if_exists=True)
        if history is None:
            config.input = {"messages": [{"role": "user", "content": ask}]}
        else:
            config.provider_messages = history
        if asynchronous:
            async def run():
                return await (engine.run_stream_async(config, self._on_event) if stream else engine.run_async(config))
            self.result = asyncio.run(run())
        else:
            self.result = engine.run_stream(config, self._on_event) if stream else engine.run(config)

    def _on_event(self, event):
        if event.type == ng.GraphEvent.Type.LLM_TOKEN:
            self.tokens.append(event.data)

    def messages(self):
        return self.result.output["channels"]["messages"]["value"]

    def text(self):
        return self.messages()[-1]["content"]

    def assistant_text(self):
        return "".join(m["content"] for m in self.messages() if m["role"] == "assistant")

    def streamed(self):
        return "".join(self.tokens)

    def rounds(self):
        return self.result.execution_trace.count("llm")

    def input_tokens(self):
        return self.result.provider_outcomes[-1].completion.usage.input_total.value


def _last_message(result):
    return result.output["channels"]["messages"]["value"][-1]


def _numbers(text):
    """The numbers in a text, without thousands separators, as strings."""
    return re.findall(r"\d+(?:\.\d+)?", text.replace(",", ""))


def _signed_thinking(result):
    return [p for m in result.native_messages for p in m.parts
            if isinstance(p, ng.Thinking) and getattr(p, "signature", None)]


def _tool_calls(result):
    return [[p for p in m.parts if isinstance(p, ng.ProviderToolCall)] for m in result.native_messages]


WEATHER = {"seoul": 18, "lima": 22, "oslo": 4}
EMPLOYEES = {"ada okafor": "E-7731"}
DEPARTMENTS = {"E-7731": "D-42"}
BUDGETS = {"D-42": 184250}
BUDGET_QUESTION = ("What is the yearly budget of the department that employee Ada Okafor works in, increased by "
                   "10 percent? Answer with the number only.")


def _budget_tools(log):
    return [
        log.make("lookup_employee", "Find the employee id of a person by full name.", {"name": STR},
                 lambda a: EMPLOYEES.get(a["name"].lower(), "unknown employee")),
        log.make("get_department", "Get the department code of an employee id.", {"employee_id": STR},
                 lambda a: DEPARTMENTS.get(a["employee_id"], "unknown employee id")),
        log.make("get_budget", "Get the yearly budget, in dollars, of a department code.", {"department": STR},
                 lambda a: str(BUDGETS.get(a["department"], "unknown department"))),
    ]


def _expect_budget_chain(log, text):
    # Each call can only be made once the previous result is known: the arguments carry the earlier answers.
    assert log.names() == ["lookup_employee", "get_department", "get_budget"]
    assert log.arguments("get_department") == [{"employee_id": "E-7731"}]
    assert log.arguments("get_budget") == [{"department": "D-42"}]
    assert "202675" in _numbers(text)


def _model(preset):
    return TEXT_MODELS[preset][0]


ROUTE_NAMES = {ANTHROPIC: "anthropic", OPENROUTER: "openrouter"}
DIRECT = [case(ANTHROPIC, keys=[KEYS[ANTHROPIC]], id="anthropic")]
ROUTED_TOO = DIRECT + [case(OPENROUTER, keys=[KEYS[OPENROUTER]], id="openrouter")]


@pytest.mark.parametrize("preset", DIRECT)
def test_dependent_tool_calls_feed_each_other(preset):
    log = Tools()
    engine = _agent(preset, _model(preset), _budget_tools(log), "Use the tools to answer. Never guess an id.",
                    max_output_tokens=500)
    turn = Turn(engine, _thread(), BUDGET_QUESTION)
    _expect_budget_chain(log, turn.text())
    assert turn.rounds() == 4


@pytest.mark.parametrize("stream", [case(s, keys=[KEYS[ANTHROPIC]], id=f"async-{n}") for n, s in MODES])
def test_async_run_completes_a_tool_loop(stream):
    log = Tools()
    engine = _agent(ANTHROPIC, _model(ANTHROPIC), _budget_tools(log), "Use the tools to answer. Never guess an id.",
                    max_output_tokens=500)
    turn = Turn(engine, _thread(), BUDGET_QUESTION, stream=stream, asynchronous=True)
    _expect_budget_chain(log, turn.text())
    if stream:
        assert turn.streamed() == turn.assistant_text()


LINKS = {"n1": (12, "n4"), "n4": (7, "n2"), "n2": (30, "n7"), "n7": (5, "n3"), "n3": (21, "n6"), "n6": (9, "n5"),
         "n5": (16, None)}


@pytest.mark.parametrize("stream", [case(s, keys=[KEYS[ANTHROPIC]], id=n) for n, s in MODES])
def test_long_tool_chain_visits_every_link(stream):
    log = Tools()
    tool = log.make("read_node", "Read a node of a linked list: its integer value and the id of the next node, "
                    "null at the end of the list.", {"node_id": STR},
                    lambda a: json.dumps(dict(zip(("value", "next"), LINKS.get(a["node_id"], (None, None))))))
    engine = _agent(ANTHROPIC, _model(ANTHROPIC), [tool], max_output_tokens=500)
    turn = Turn(engine, _thread(), "The linked list starts at node n1. Follow it to the end and tell me the sum "
                "of all the node values. Answer with the number only.", stream=stream)
    # Only the result of one call names the next node, so the calls cannot be batched.
    assert [a["node_id"] for a in log.arguments("read_node")] == ["n1", "n4", "n2", "n7", "n3", "n6", "n5"]
    assert "100" in _numbers(turn.text())
    assert turn.rounds() == 8
    if stream:
        assert turn.streamed() == turn.assistant_text()


@pytest.mark.parametrize("preset", DIRECT)
def test_conversation_remembers_its_first_turn(preset):
    engine = _agent(preset, _model(preset), instructions="Answer briefly.", max_output_tokens=300)
    thread = _thread()
    asks = ["Hi! My cat is called Biscuit and my lucky number is 4172. Just say OK.",
            "What is 12 times 12?",
            "I moved to Tromso last year. Just say OK.",
            "Name one primary colour.",
            "Spell the word 'graph' backwards.",
            "Without asking me anything: what is my cat called, what is my lucky number, and where do I live?"]
    turns = [Turn(engine, thread, ask) for ask in asks]
    for number, turn in enumerate(turns, start=1):
        assert len(turn.messages()) == 2 * number
        assert turn.result.execution_trace == ["llm"]
    # The history grows by the carried turns: each request is longer than the one before.
    sizes = [turn.input_tokens() for turn in turns]
    assert sizes == sorted(set(sizes))
    assert "144" in _numbers(turns[1].text())
    assert "hparg" in re.sub("[^a-z]", "", turns[4].text().lower())
    last = turns[-1].text().lower()
    assert "biscuit" in last and "4172" in _numbers(last) and "troms" in last


@pytest.mark.parametrize("preset", DIRECT)
def test_concurrent_conversations_keep_their_own_history(preset):
    engine = _agent(preset, _model(preset), instructions="Answer briefly.", max_output_tokens=200)
    pets = {_thread(): name for name in ("Biscuit", "Pepper", "Marzipan")}

    async def converse(thread, pet):
        tell = ng.RunConfig(thread_id=thread, resume_if_exists=True, input={"messages": [
            {"role": "user", "content": f"My cat is called {pet}. Just say OK."}]})
        await engine.run_async(tell)
        ask = ng.RunConfig(thread_id=thread, resume_if_exists=True, input={"messages": [
            {"role": "user", "content": "What is my cat called? Answer with the name only."}]})
        return await engine.run_async(ask)

    async def everyone():
        return await asyncio.gather(*(converse(thread, pet) for thread, pet in pets.items()))

    for (thread, pet), result in zip(pets.items(), asyncio.run(everyone())):
        messages = result.output["channels"]["messages"]["value"]
        assert len(messages) == 4 and pet.lower() in messages[-1]["content"].lower()
        # The other threads' cats never reach this one.
        others = [other.lower() for other in pets.values() if other != pet]
        assert not any(other in m["content"].lower() for m in messages for other in others)


@pytest.mark.parametrize("preset", DIRECT)
def test_a_remembered_fact_drives_a_later_tool_call(preset):
    log = Tools()
    tool = log.make("get_weather", "Get the current temperature in Celsius of a city.", {"city": STR},
                    lambda a: json.dumps({"city": a["city"], "temp_c": WEATHER.get(a["city"].lower())}))
    engine = _agent(preset, _model(preset), [tool], "Answer briefly.", max_output_tokens=300)
    thread = _thread()
    Turn(engine, thread, "I live in Lima. Just say OK.")
    Turn(engine, thread, "What is 7 times 8?")
    assert not log.calls
    turn = Turn(engine, thread, "What is the temperature where I live right now? Use the tool.")
    assert [a["city"].lower() for a in log.arguments("get_weather")] == ["lima"]
    assert "22" in _numbers(turn.text())


INVENTORY = [("We received 40 bolts (SKU BOLT) and 25 nuts (SKU NUT). Record both.", {"BOLT": 40, "NUT": 25}),
             ("We shipped 15 bolts. Record it.", {"BOLT": 25, "NUT": 25}),
             ("Of the nuts, 5 were damaged; take them out of stock.", {"BOLT": 25, "NUT": 20}),
             ("Check the system and tell me how many bolts and nuts are in stock now.", {"BOLT": 25, "NUT": 20})]


INVENTORY_RUNS = [case(ANTHROPIC, {}, keys=[KEYS[ANTHROPIC]], id="anthropic"),
                 case(OPENROUTER, {}, keys=[KEYS[OPENROUTER]], id="openrouter"),
                 case(ANTHROPIC, THINKING_CONTROLS, keys=[KEYS[ANTHROPIC]], id="anthropic-thinking")]


@pytest.mark.parametrize("preset, extra", INVENTORY_RUNS)
def test_inventory_tasks_across_turns(preset, extra):
    stock = {}
    log = Tools()

    def adjust(arguments):
        sku = arguments["sku"].upper()
        stock[sku] = stock.get(sku, 0) + int(arguments["delta"])
        return json.dumps({"sku": sku, "quantity": stock[sku]})

    tools = [log.make("adjust_stock", "Add (positive delta) or remove (negative delta) units of a SKU. Returns the "
                      "new quantity.", {"sku": STR, "delta": INT}, adjust),
             log.make("get_stock", "Read the quantity in stock of a SKU.", {"sku": STR},
                      lambda a: json.dumps({"sku": a["sku"].upper(), "quantity": stock.get(a["sku"].upper(), 0)}))]
    engine = _agent(preset, _model(preset), tools, "You keep the warehouse inventory. Record every change with the "
                    "tools and never guess a quantity.", **{"max_output_tokens": 500, **extra})
    thread = _thread()
    for number, (ask, expected) in enumerate(INVENTORY, start=1):
        before = len(log.calls)
        turn = Turn(engine, thread, ask)
        assert stock == expected, f"after turn {number}"
        assert len(log.calls) > before, f"turn {number} used no tool"
    # Four changes were asked for over three turns, and each was recorded once.
    changes = [(a["sku"].upper(), a["delta"]) for a in log.arguments("adjust_stock")]
    assert sorted(changes) == [("BOLT", -15), ("BOLT", 40), ("NUT", -5), ("NUT", 25)]
    assert {"25", "20"} <= set(_numbers(turn.text()))
    if extra:
        # The thread carries every earlier turn's signed thinking block into the later requests.
        assert _signed_thinking(turn.result)


THINKING = [case(preset, stream, keys=[KEYS[preset]], id=f"{ROUTE_NAMES[preset]}-{mode}")
            for preset in ROUTE_NAMES for mode, stream in MODES]


@pytest.mark.parametrize("preset, stream", THINKING)
def test_manual_thinking_survives_tool_rounds_and_a_later_turn(preset, stream):
    log = Tools()
    engine = _agent(preset, _model(preset), _budget_tools(log), "Use the tools to answer. Never guess an id.",
                    **THINKING_CONTROLS)
    turn = Turn(engine, _thread(), BUDGET_QUESTION, stream=stream)
    # The provider checks the signature of a thinking block it is given back, so a run that went on through three
    # tool rounds after a thinking turn returned that block unaltered.
    assert _signed_thinking(turn.result)
    _expect_budget_chain(log, turn.text())
    if stream:
        assert turn.streamed() == turn.assistant_text()
    # The caller can carry the typed history, thinking included, into a new thread.
    history = [*turn.result.native_messages, user("Which department did you find for her? Answer from what you "
                                                  "already know.")]
    assert "D-42" in Turn(engine, _thread(), history=history).text()


# Anthropic documents this string as the way to make a model answer with a redacted thinking block.
REDACTION_TRIGGER = ("ANTHROPIC_MAGIC_STRING_TRIGGER_REDACTED_THINKING_"
                     "46C9A13E193C177646C7398A98432ECCCE4C1253D5E2D82641AC0E52CC2876CB")


@pytest.mark.parametrize("stream", [case(s, keys=[KEYS[ANTHROPIC]], id=n) for n, s in MODES])
def test_redacted_thinking_survives_tool_rounds(stream):
    log = Tools()
    engine = _agent(ANTHROPIC, _model(ANTHROPIC), _budget_tools(log), "Use the tools to answer. Never guess an id.",
                    **THINKING_CONTROLS)
    turn = Turn(engine, _thread(), f"{REDACTION_TRIGGER}\n\n{BUDGET_QUESTION}", stream=stream)
    redacted = [p for m in turn.result.native_messages for p in m.parts if isinstance(p, ng.RedactedThinking)]
    assert redacted and all(p.data for p in redacted)
    _expect_budget_chain(log, turn.text())
    if stream:
        assert turn.streamed() == turn.assistant_text()


@pytest.mark.parametrize("preset", DIRECT)
def test_provider_message_list_replays_a_thinking_tool_turn(preset):
    model, prompt = _model(preset), "What is the weather in Seoul? Call get_weather, then give the temperature in C."
    p = provider(preset, model)
    values = dict(tools=[weather_tool()], max_output_tokens=3000, thinking_mode=TM.Manual, thinking_budget=1024)
    first = Run(p, model, [user(prompt)], **values)
    turn = first.ok().messages
    thinking = [part for message in turn for part in message.parts if isinstance(part, ng.Thinking)]
    calls = [part for message in turn for part in message.parts if isinstance(part, ng.ProviderToolCall)]
    assert calls and thinking and thinking[0].signature
    result = ng.ProviderMessage(role=ng.ProviderRole.Tool,
                                parts=[ng.ProviderToolResult(calls[0].id, json.dumps({"temp_c": 17.9}))])
    # The caller keeps the message list: the turn exactly as the model produced it, signed thinking block and tool
    # call, is accepted together with its tool result.
    assert "17.9" in Run(p, model, [user(prompt), *turn, result], **values).text()
    # A copy rebuilt from the parts has no native provenance, so it is refused before any request is sent.
    rebuilt = [ng.ProviderMessage(role=m.role, parts=[x for x in m.parts if not isinstance(x, ng.Thinking)])
               for m in turn]
    failure = Run(p, model, [user(prompt), *rebuilt, result], **values).outcome.failure
    assert failure is not None and failure.error.kind == ng.ProviderErrorKind.ReplayIneligible
    assert not failure.error.http_status


@pytest.mark.parametrize("stream", [case(s, keys=[KEYS[ANTHROPIC]], id=n) for n, s in MODES])
def test_parallel_tool_calls_in_one_turn(stream):
    log = Tools()
    tool = log.make("get_weather", "Get the current temperature in Celsius of a city.", {"city": STR},
                    lambda a: json.dumps({"city": a["city"], "temp_c": WEATHER.get(a["city"].lower())}))
    engine = _agent(ANTHROPIC, _model(ANTHROPIC), [tool], "When calls do not depend on each other, make them in "
                    "the same turn.", max_output_tokens=600)
    turn = Turn(engine, _thread(), "Get the weather of Seoul, Lima and Oslo, then tell me which is the warmest.",
                stream=stream)
    assert sorted(a["city"].lower() for a in log.arguments("get_weather")) == ["lima", "oslo", "seoul"]
    batches = [calls for calls in _tool_calls(turn.result) if calls]
    assert max(len(calls) for calls in batches) >= 2, "the model made the independent calls one at a time"
    # Every call is answered by exactly one result of its own id.
    asked = sorted(call.id for calls in batches for call in calls)
    answered = sorted(m["tool_call_id"] for m in turn.messages() if m["role"] == "tool")
    assert asked == answered and len(set(asked)) == 3
    assert "lima" in turn.text().lower() and {"18", "22", "4"} <= set(_numbers(turn.text()))
    if stream:
        assert turn.streamed() == turn.assistant_text()


@pytest.mark.parametrize("preset", DIRECT)
def test_a_tool_error_result_is_read_and_the_call_retried(preset):
    attempts = []

    def ledger(arguments):
        attempts.append(arguments)
        if len(attempts) == 1:
            return json.dumps({"error": "the ledger is busy; call get_balance again with the same arguments"})
        return json.dumps({"account": arguments["account"], "balance": 1204.5})

    log = Tools()
    engine = _agent(preset, _model(preset), [log.make("get_balance", "Get the balance of an account.",
                                                      {"account": STR}, ledger)], max_output_tokens=500)
    turn = Turn(engine, _thread(), "What is the balance of account A-17? Answer with the number only.")
    assert len(attempts) >= 2 and all(a == {"account": "A-17"} for a in attempts)
    assert "error" in log.calls[0][2] and "1204.5" in log.calls[-1][2]
    assert "1204.5" in _numbers(turn.text())


@pytest.mark.parametrize("preset", DIRECT)
def test_a_tool_that_raises_is_an_error_result_the_model_answers_from(preset):
    log = Tools()

    def unavailable(_arguments):
        raise RuntimeError("ledger service unavailable")

    engine = _agent(preset, _model(preset), [log.make("get_balance", "Get the balance of an account.",
                                                      {"account": STR}, unavailable)], max_output_tokens=500)
    turn = Turn(engine, _thread(), "What is the balance of account A-17? If you cannot find out, say so.")
    results = [part for m in turn.result.native_messages for part in m.parts
               if isinstance(part, ng.ProviderToolResult)]
    assert log.calls and results and all(r.is_error for r in results)
    # The provider accepted the error result, and the model answered after it.
    assert turn.messages()[-1]["role"] == "assistant" and turn.text().strip()


@pytest.mark.parametrize("extra", [case({}, keys=[KEYS[ANTHROPIC]], id="plain"),
                                   case(THINKING_CONTROLS, keys=[KEYS[ANTHROPIC]], id="thinking")])
def test_every_tool_call_waits_for_a_person_and_the_loop_goes_on(extra):
    log = Tools()
    engine = _agent(ANTHROPIC, _model(ANTHROPIC), _budget_tools(log), "Use the tools to answer. Never guess an id.",
                    graph=REVIEWED, **{"max_output_tokens": 500, **extra})
    thread = _thread()
    result = engine.run(ng.RunConfig(thread_id=thread, max_steps=30, input={
        "messages": [{"role": "user", "content": BUDGET_QUESTION}]}))
    proposed = []
    while result.interrupted:
        # The paused call is on the checkpoint and has not run.
        assert len(log.calls) == len(proposed)
        proposed.append([call["name"] for call in _last_message(result)["tool_calls"]])
        result = engine.resume(thread)
    assert proposed == [["lookup_employee"], ["get_department"], ["get_budget"]]
    _expect_budget_chain(log, _last_message(result)["content"])
    if extra:
        # The signed thinking block went into the checkpoint and came back out of it unaltered.
        assert _signed_thinking(result)


TITLES = {"old archive": "R-200", "draft": "R-100"}


@pytest.mark.parametrize("approved", [case(True, keys=[KEYS[ANTHROPIC]], id="approved"),
                                      case(False, keys=[KEYS[ANTHROPIC]], id="refused")])
def test_gated_tool_call_resumes_with_the_operators_answer(approved):
    records = {"R-100": "draft", "R-200": "archived"}
    log = Tools()
    tools = [log.make("find_record", "Find the id of the record with a given title.", {"title": STR},
                      lambda a: TITLES.get(a["title"].lower(), "no such record")),
             log.make("delete_record", "Permanently delete a record by id.", {"record_id": STR},
                      lambda a: json.dumps({"deleted": records.pop(a["record_id"], None) is not None}))]
    engine = _agent(ANTHROPIC, _model(ANTHROPIC), tools, "Use the tools. Delete only what is asked.",
                    max_output_tokens=500)

    def gate(call, context):
        if call.name != "delete_record":
            return ng.ToolDecision.allow()
        if context.resume_value is None:
            return ng.ToolDecision.interrupt("deleting needs approval", {"tool": call.name})
        if context.resume_value["approved"]:
            return ng.ToolDecision.allow()
        return ng.ToolDecision.deny("the operator refused")

    engine.set_tool_gate(gate)
    thread = _thread()
    paused = engine.run(ng.RunConfig(thread_id=thread, max_steps=30, input={
        "messages": [{"role": "user", "content": "Find the record titled 'old archive' and delete it."}]}))
    assert paused.interrupted and paused.interrupt_node == "tools"
    assert paused.interrupt_value["value"] == {"tool": "delete_record"}
    assert sorted(records) == ["R-100", "R-200"]
    finished = engine.resume(thread, {"approved": approved})
    assert not finished.interrupted
    assert ("R-200" in records) is (not approved)
    assert _last_message(finished)["role"] == "assistant" and _last_message(finished)["content"].strip()


def _cache_system():
    # A session marker of its own, so that an earlier run's cache cannot answer the first turn.
    rules = "\n".join(f"Rule {n}: when you report on station S-{n:03d}, name the station id and its unit of measure."
                      for n in range(1, 221))
    return f"Session {uuid.uuid4()}. You answer briefly.\n{rules}"


def _count(value):
    return 0 if value is None else value.value


def _cache_cases():
    roles = ((ANTHROPIC, True), (ADAPTIVE, False))
    models = [live_model(role) for role, _strip in roles]
    ids = [case_model_id(role, strip_date=strip) for role, strip in roles]
    if ids[0] == ids[1]:
        ids = [f"{role}:{cid}" for (role, _strip), cid in zip(roles, ids)]
    return [case(model, keys=[KEYS[ANTHROPIC]], id=cid) for model, cid in zip(models, ids)]


@pytest.mark.parametrize("model", _cache_cases())
def test_prompt_cache_grows_over_a_conversation(model):
    cache = ng.MessagesCacheControl()
    cache.ttl = ng.MessagesCacheTtl.FiveMinutes
    engine = _agent(ANTHROPIC, model, max_output_tokens=60, system=_cache_system(), cache_control=cache)
    thread = _thread()
    turns = [Turn(engine, thread, ask) for ask in ("Hello, what is 2+2?", "Now what is 3+3?", "And 4+4?", "And 5+5?")]
    usages = [turn.result.provider_outcomes[-1].completion.usage for turn in turns]
    writes, reads = [_count(u.cache_write) for u in usages], [_count(u.cache_read) for u in usages]
    assert writes[0] > 0
    # Every later turn reads the prefix cached before it, which is longer each time: the history grows.
    assert all(read > 0 for read in reads[1:])
    assert reads[1:] == sorted(set(reads[1:]))


@pytest.mark.parametrize("stream", [case(s, keys=[KEYS[ANTHROPIC]], id=n) for n, s in MODES])
def test_claude_5_5_adaptive_thinking_with_tools(stream):
    log = Tools()
    links = {"n1": (12, "n4"), "n4": (7, "n2"), "n2": (30, None)}
    tool = log.make("read_node", "Read a node of a linked list: its integer value and the id of the next node, "
                    "null at the end of the list.", {"node_id": STR},
                    lambda a: json.dumps(dict(zip(("value", "next"), links.get(a["node_id"], (None, None))))))
    engine = _agent(ANTHROPIC, live_model(ADAPTIVE), [tool], max_output_tokens=1500, thinking_mode=TM.Adaptive)
    turn = Turn(engine, _thread(), "The linked list starts at node n1. Follow it to the end and tell me the sum "
                "of all the node values. Answer with the number only.", stream=stream)
    assert [a["node_id"] for a in log.arguments("read_node")] == ["n1", "n4", "n2"]
    assert "49" in _numbers(turn.text())
    # Adaptive thinking may skip a turn, but a thinking block that comes back carries its signature.
    assert all(getattr(p, "signature", None) for m in turn.result.native_messages for p in m.parts
               if isinstance(p, ng.Thinking))
    if stream:
        assert turn.streamed() == turn.assistant_text()
