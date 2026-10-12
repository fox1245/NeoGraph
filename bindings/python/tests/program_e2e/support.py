"""Shared building blocks for the Program API end-to-end scenarios.

Everything here uses only the public Python surface a user reads about in
docs/python-binding.md: Python GraphNode subclasses registered with
ng.NodeFactory, conditions registered with ng.ConditionRegistry, an immutable
ng.ProgramRegistryBuilder snapshot, sealed-v1 JavaScript sources, and
ng.LocalProgramHost / ng.ProgramCompiler.

The module is importable from the tests and from the helper subprocesses that
the cross-process scenarios launch (`from program_e2e import support`): it
registers the same node types and builds the same registry in every process.
"""

from __future__ import annotations

import hashlib
import http.server
import json
import os
import subprocess
import sys
import textwrap
import threading
import time
from urllib.parse import urlsplit

import pytest

import neograph_engine as ng

PACKAGE_DIR = os.path.dirname(os.path.abspath(__file__))
TESTS_DIR = os.path.dirname(PACKAGE_DIR)


# --------------------------------------------------------------------------
# Expected failures
# --------------------------------------------------------------------------


def known_issue(issue: str, reason: str):
    """Strict xfail for an open, tracked issue.

    A fix makes the test XPASS, which strict=True reports as a failure, so the
    marker has to be removed together with the fix.
    """
    def decorate(fn):
        fn = pytest.mark.xfail(strict=True, reason=f"{issue}: {reason}")(fn)
        return pytest.mark.known_issue(fn)
    return decorate


def slow(fn):
    """A scenario that waits on purpose for a second or more (deselect with -m "not slow")."""
    return pytest.mark.slow(fn)


# --------------------------------------------------------------------------
# Budgets
# --------------------------------------------------------------------------

BUDGET_FIELDS = (
    "wall_time_ms",
    "model_tokens",
    "monetary_microunits",
    "max_concurrency",
    "max_program_operations",
    "max_core_steps",
    "max_dynamic_compiles",
    "max_child_depth",
    "max_total_children",
)

_RUN_DEFAULTS = dict(
    wall_time_ms=10_000,
    model_tokens=0,
    monetary_microunits=0,
    max_concurrency=4,
    max_program_operations=16,
    max_core_steps=50,
    max_dynamic_compiles=0,
    max_child_depth=0,
    max_total_children=0,
)

# A LocalProgramHost policy ceiling must be positive for every resource except
# the (paired) child budgets, even when the Program never calls a model.
_CEILING_DEFAULTS = dict(
    wall_time_ms=120_000,
    model_tokens=1,
    monetary_microunits=1,
    max_concurrency=8,
    max_program_operations=64,
    max_core_steps=2_000,
    max_dynamic_compiles=1,
    max_child_depth=0,
    max_total_children=0,
)


def _make_budget(values: dict) -> "ng.ProgramRunBudget":
    value = ng.ProgramRunBudget()
    for key, item in values.items():
        setattr(value, key, item)
    return value


def budget(**overrides) -> "ng.ProgramRunBudget":
    """A run budget. compile_admit() pins the run budget to exactly this value."""
    unknown = set(overrides) - set(BUDGET_FIELDS)
    if unknown:
        raise TypeError(f"unknown budget fields: {sorted(unknown)}")
    return _make_budget({**_RUN_DEFAULTS, **overrides})


def ceiling(**overrides) -> "ng.ProgramRunBudget":
    unknown = set(overrides) - set(BUDGET_FIELDS)
    if unknown:
        raise TypeError(f"unknown budget fields: {sorted(unknown)}")
    return _make_budget({**_CEILING_DEFAULTS, **overrides})


def budget_dict(value: "ng.ProgramRunBudget") -> dict:
    return {key: getattr(value, key) for key in BUDGET_FIELDS}


# --------------------------------------------------------------------------
# Observable side effects of Python nodes
# --------------------------------------------------------------------------


class Journal:
    """Thread-safe record of what Python nodes actually did in this process."""

    def __init__(self):
        self._lock = threading.Lock()
        self._events: list[tuple[str, str]] = []

    def add(self, kind: str, name: str) -> None:
        with self._lock:
            self._events.append((kind, name))

    def clear(self) -> None:
        with self._lock:
            self._events.clear()

    def names(self, kind: str = "ran") -> list[str]:
        with self._lock:
            return [name for event_kind, name in self._events if event_kind == kind]


JOURNAL = Journal()


def assert_started_bodies_saw_cancel(name: str, *, ended_on_their_own: int = 0) -> None:
    """Every body of `name` that started, and did not end on its own, saw its token cancelled.

    A sibling the runtime cancels before a worker starts it never runs, so it leaves no
    "cancelled" entry (#367, #368); the contract is that a body which did start is told,
    not that every cancelled member starts."""
    started = JOURNAL.names("started").count(name)
    cancelled = JOURNAL.names("cancelled").count(name)
    assert cancelled == started - ended_on_their_own, (
        f"{started} bodies of {name!r} started, {ended_on_their_own} ended on their own, "
        f"{cancelled} saw the cancellation")


class Step(ng.GraphNode):
    """One configurable Python node type, driven by its JavaScript config.

    Config keys (all optional):
      boom: true                 raise RuntimeError
      boom_on_topic: "t"         raise RuntimeError when the "topic" channel is "t"
      boom_first: true           raise RuntimeError on this node's first call in the test only
      ask: "question"            raise ng.NodeInterrupt until a resume value arrives
      delay_ms: N                cooperative sleep that polls input.ctx.cancel_token
      blocking_sleep_ms: N       plain time.sleep (a node that ignores cancellation)
      slow_first_ms: N           sleep N ms on this node's first call in the test only;
                                 with cooperative: true the sleep polls the cancel token
      delay_from_state: true     read the delay in ms from the "delay" channel
      incr: true                 counter += 1
      record_resume: true        write input.ctx.resume_value to the "answer" channel
    Every node appends its own name to the "trace" channel.
    """

    def __init__(self, name, config):
        super().__init__()
        self._name = name
        self._config = dict(config or {})

    def get_name(self):
        return self._name

    def _sleep(self, input, milliseconds):
        token = input.ctx.cancel_token
        deadline = time.monotonic() + milliseconds / 1000.0
        while time.monotonic() < deadline:
            if token is not None and token.is_cancelled():
                JOURNAL.add("cancelled", self._name)
                raise RuntimeError(f"{self._name} observed cancellation")
            time.sleep(0.005)

    def run(self, input):
        config = self._config
        JOURNAL.add("started", self._name)
        first_call = JOURNAL.names("started").count(self._name) == 1
        if config.get("boom") or (config.get("boom_first") and first_call) or (
                "boom_on_topic" in config and input.state.get("topic") == config["boom_on_topic"]):
            JOURNAL.add("raised", self._name)
            raise RuntimeError(f"boom from {self._name}")
        if config.get("blocking_sleep_ms"):
            time.sleep(config["blocking_sleep_ms"] / 1000.0)
        if config.get("slow_first_ms") and first_call:
            if config.get("cooperative"):
                self._sleep(input, config["slow_first_ms"])
            else:
                time.sleep(config["slow_first_ms"] / 1000.0)
        delay = config.get("delay_ms") or 0
        if config.get("delay_from_state"):
            delay = input.state.get("delay") or 0
        if delay:
            self._sleep(input, delay)
        writes = [ng.ChannelWrite("trace", [self._name])]
        if config.get("ask"):
            answer = input.ctx.resume_value
            if answer is None:
                JOURNAL.add("asked", self._name)
                raise ng.NodeInterrupt({"question": config["ask"]}, reason="needs approval")
            writes.append(ng.ChannelWrite("answer", answer))
        if config.get("record_resume"):
            writes.append(ng.ChannelWrite("answer", input.ctx.resume_value))
        if config.get("incr"):
            writes.append(ng.ChannelWrite("counter", (input.state.get("counter") or 0) + 1))
        JOURNAL.add("ran", self._name)
        return writes


class Fan(ng.GraphNode):
    """Map step: one ng.Send per item in the "items" channel."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        JOURNAL.add("ran", self._name)
        return [ng.Send("square", {"item": item}) for item in (input.state.get("items") or [])]


class Square(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        item = input.state.get("item")
        JOURNAL.add("ran", f"{self._name}:{item}")
        return [ng.ChannelWrite("results", [item * item])]


class ChecksFailed(Exception):
    """What TEST raises in the failure-routing scenarios (#364).

    The binding reads `code` (an author code), `retryable` and
    `public_message` from the exception; it never reads str(exc) into a record.
    """

    def __init__(self, message, code=None, public_message=None, retryable=None):
        super().__init__(message)
        if code is not None:
            self.code = code
        if public_message is not None:
            self.public_message = public_message
        if retryable is not None:
            self.retryable = retryable


class FixTest(ng.GraphNode):
    """TEST in the failure-routing scenarios: fails until the "fixed" channel is true.

    Config keys (all optional): code, public_message, retryable (copied onto
    the raised ChecksFailed), diagnostic (raise ng.ProgramDiagnosticError instead).
    """

    def __init__(self, name, config):
        super().__init__()
        self._name = name
        self._config = dict(config or {})

    def get_name(self):
        return self._name

    def run(self, input):
        JOURNAL.add("started", self._name)
        if not input.state.get("fixed"):
            JOURNAL.add("raised", self._name)
            if self._config.get("diagnostic"):
                raise ng.ProgramDiagnosticError("a Program diagnostic inside a node")
            raise ChecksFailed(f"tests failed in {self._name}",
                              code=self._config.get("code"),
                              public_message=self._config.get("public_message"),
                              retryable=self._config.get("retryable"))
        JOURNAL.add("ran", self._name)
        return [ng.ChannelWrite("trace", [self._name])]


class Review(ng.GraphNode):
    """ASK_USER: records the human's answer ({"decision", "fix"}) from the resume value."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        answer = input.ctx.resume_value or {}
        JOURNAL.add("ran", self._name)
        return [ng.ChannelWrite("trace", [self._name]),
                ng.ChannelWrite("decision", answer.get("decision", "abort")),
                ng.ChannelWrite("fixed", bool(answer.get("fix")))]


def _route_by_topic(state):
    return "billing" if state.get("topic") == "billing" else "general"


def _until_target(state):
    return "done" if (state.get("counter") or 0) >= (state.get("target") or 0) else "again"


NODE_TYPES = ("e2e.step", "e2e.fan", "e2e.square")
# The #364 failure-routing scenarios (test_09) add these to their registry.
FAILURE_NODE_TYPES = ("e2e.fix_test", "e2e.review")
FAILURE_CONDITIONS = {
    "e2e.decision": lambda state: state.get("decision") or "abort",
}
CONDITIONS = {
    "e2e.route_by_topic": _route_by_topic,
    "e2e.always_fail": lambda state: "fail",
    "e2e.always_pass": lambda state: "pass",
    "e2e.until_target": _until_target,
    "e2e.forever": lambda state: "again",
}


def register_python_types() -> None:
    """Register node factories and conditions (process-global registries)."""
    ng.NodeFactory.register_type("e2e.step", lambda name, config, ctx: Step(name, config))
    ng.NodeFactory.register_type("e2e.fan", lambda name, config, ctx: Fan(name))
    ng.NodeFactory.register_type("e2e.square", lambda name, config, ctx: Square(name))
    ng.NodeFactory.register_type("e2e.fix_test", lambda name, config, ctx: FixTest(name, config))
    ng.NodeFactory.register_type("e2e.review", lambda name, config, ctx: Review(name))
    for name, fn in {**CONDITIONS, **FAILURE_CONDITIONS}.items():
        ng.ConditionRegistry.register_condition(name, fn)


register_python_types()


def digest(label: str) -> str:
    return "sha256:" + hashlib.sha256(label.encode()).hexdigest()


def build_registry(extra_node_types=(), extra_conditions=()) -> "ng.ProgramRegistrySnapshot":
    builder = ng.ProgramRegistryBuilder()
    for node_type in (*NODE_TYPES, *extra_node_types):
        builder.add_registered_node(node_type, "1.0.0", digest("node:" + node_type))
    for name in (*CONDITIONS, *extra_conditions):
        builder.add_registered_condition(name, "1.0.0", digest("condition:" + name))
    for reducer in ("append", "overwrite"):
        builder.add_registered_reducer(reducer, "1.0.0", digest("reducer:" + reducer))
    return builder.build()


def make_host(owner: str = "tenant:e2e", *, registry=None, budget_ceiling=None,
              scheduler_threads=None, **kwargs) -> "ng.LocalProgramHost":
    if scheduler_threads is not None:
        kwargs["scheduler_threads"] = scheduler_threads
    return ng.LocalProgramHost(
        registry if registry is not None else build_registry(),
        owner,
        budget_ceiling if budget_ceiling is not None else ceiling(),
        **kwargs,
    )


# --------------------------------------------------------------------------
# JavaScript sources
# --------------------------------------------------------------------------

STANDARD_CHANNELS = (
    'g.channel("trace", {reducer: "append", initial: []});\n'
    '  g.channel("answer", {reducer: "overwrite", initial: null});\n'
    '  g.channel("counter", {reducer: "overwrite", initial: 0});\n'
    '  g.channel("target", {reducer: "overwrite", initial: 0});\n'
    '  g.channel("topic", {reducer: "overwrite", initial: ""});\n'
    '  g.channel("delay", {reducer: "overwrite", initial: 0});'
)


def define_graph(statements: str, name: str = "work") -> str:
    """A define() module: `statements` are builder calls on `g` (one per line)."""
    body = textwrap.indent(textwrap.dedent(statements).strip(), "  ")
    return (
        "export function define() {\n"
        f'  const g = ng.graph("{name}");\n'
        f"  {STANDARD_CHANNELS}\n"
        f"{body}\n"
        "  return g;\n"
        "}\n"
    )


def with_main(define_source: str, main_body: str) -> str:
    body = textwrap.indent(textwrap.dedent(main_body).strip(), "  ")
    return define_source + "\nexport function* main(input) {\n" + body + "\n}\n"


# Programs that the tests and their helper subprocesses share.

TRIAGE = define_graph("""
    g.node("intake", {type: "e2e.step"});
    g.node("classify", {type: "e2e.step"});
    g.node("billing", {type: "e2e.step"});
    g.node("general", {type: "e2e.step"});
    g.entry("intake");
    g.edge("intake", "classify");
    g.conditionalEdge("classify", "e2e.route_by_topic", {billing: "billing", general: "general"});
    g.exit("billing");
    g.exit("general");
""", name="triage")

APPROVAL_GATE = define_graph("""
    g.node("draft", {type: "e2e.step"});
    g.node("deploy", {type: "e2e.step", record_resume: true});
    g.entry("draft");
    g.edge("draft", "deploy");
    g.interruptBefore("deploy");
    g.exit("deploy");
""", name="release")


def source(text: str, source_id: str = "program.js") -> "ng.ProgramSource":
    return ng.ProgramSource.from_javascript(source_id, text)


def admit(host, text: str, run_budget=None, source_id: str = "program.js"):
    return host.compile_admit(source(text, source_id), run_budget or budget())


def run_program(text: str, input=None, run_budget=None, host=None):
    host = host or make_host()
    run_budget = run_budget or budget()
    version = admit(host, text, run_budget)
    return host, version, host.run(version, input or {}, run_budget)


def trace_channel(result) -> list:
    output = result.output or {}
    return (output.get("channels", {}).get("trace") or {}).get("value")


def channel(result, name):
    output = result.output or {}
    return (output.get("channels", {}).get(name) or {}).get("value")


def describe(result) -> str:
    """Compact text used in assertion messages."""
    failure = result.failure
    detail = f"{failure.code}: {failure.message[:300]}" if failure else "no failure"
    return f"status={result.status} trace={result.execution_trace} {detail}"


# --------------------------------------------------------------------------
# Subprocesses (fresh interpreters for identity and restart scenarios)
# --------------------------------------------------------------------------


def python_env() -> dict:
    """The parent's environment with an absolute PYTHONPATH that can import `program_e2e`."""
    env = dict(os.environ)
    paths = [os.path.abspath(p) for p in env.get("PYTHONPATH", "").split(os.pathsep) if p]
    paths.append(TESTS_DIR)
    env["PYTHONPATH"] = os.pathsep.join(dict.fromkeys(paths))
    env.setdefault("NEOGRAPH_SUPPRESS_FANOUT_WARNING", "1")
    return env


def run_python(code: str, *args: str, timeout: float = 120):
    """Run `code` in a fresh interpreter that imports the same neograph_engine.

    The child starts in this package's directory, which holds no
    neograph_engine source tree that could shadow the module under test.
    """
    return subprocess.run(
        [sys.executable, "-c", textwrap.dedent(code), *args],
        capture_output=True,
        text=True,
        timeout=timeout,
        env=python_env(),
        cwd=PACKAGE_DIR,
    )


def last_json_line(stdout: str):
    for line in reversed(stdout.strip().splitlines()):
        line = line.strip()
        if line.startswith("{"):
            return json.loads(line)
    raise AssertionError(f"no JSON line in subprocess output:\n{stdout}")


# --------------------------------------------------------------------------
# Model calls: an OpenRouter Chat Completions provider and a loopback peer
# --------------------------------------------------------------------------

REPO_DIR = os.path.dirname(os.path.dirname(os.path.dirname(TESTS_DIR)))
CATALOG_PATH = os.path.join(REPO_DIR, "data", "pricing", "catalog.json")

# A price catalog entry with an OpenRouter route ceiling.
PRICED_MODEL = "deepseek/deepseek-v4-flash-0731"
# Declared in the loopback peer's SDK policy so a call's reservation stays small.
PRICED_INPUT_LIMIT = 100_000
OUTPUT_CAP = 64


def openrouter_descriptor(base_url, *, input_limits=None):
    """The validated descriptor openrouter_provider() uses."""
    endpoint = urlsplit(base_url.rstrip("/"))
    origin = f"{endpoint.scheme}://{endpoint.netloc}"
    path = endpoint.path.rstrip("/") + "/chat/completions"
    policy = None
    loopback = endpoint.hostname in {"127.0.0.1", "::1"}
    if loopback or input_limits:
        document = json.loads(ng.provider_policy_json())
        chat = next(f for f in document["families"] if f["family"] == "openai.chat")
        if loopback:
            chat["openrouter_origins"].append(origin)
        for model, limit in (input_limits or {}).items():
            document["models"].append({"family": "openai.chat", "model": model,
                                       "defaults": chat["defaults"], "output_limit": None,
                                       "input_limit": limit})
        policy = ng.load_provider_policy(json.dumps(document), ng.provider_codec_defaults_json())
    return ng.load_provider_descriptor(json.dumps({
        "descriptor_version": 1,
        "revision": 1,
        "id": "e2e-openrouter-chat",
        "family": "openai.chat",
        "connection": {"base_url": origin, "paths": {"buffered": path, "streaming": path}},
    }), policy)


def openrouter_provider(base_url, api_key, *, input_limits=None, timeout_ms=120_000):
    """SchemaProvider for OpenRouter Chat Completions, as in
    examples/cookbook/openrouter-provider/via_openai_compat.py.

    A loopback base URL is declared an OpenRouter origin through the closed
    policy loader, exactly like the cookbook's no-cost smoke: the SDK then
    admits OpenRouter routing and parses usage.cost, and the price catalog
    prices the endpoint as OpenRouter. input_limits maps a model to the input
    token limit the SDK policy declares for it.
    """
    from neograph_engine.llm import SchemaProvider

    descriptor = openrouter_descriptor(base_url, input_limits=input_limits)
    routing = ng.OpenRouterRouting()
    routing.zdr = True
    defaults = ng.SchemaProviderDefaults()
    defaults.provider = routing
    options = ng.ProviderRuntimeOptions(api_key=api_key, default_timeout_ms=timeout_ms)
    return SchemaProvider(descriptor, options, defaults), routing


class ChatPeer:
    """Loopback Chat Completions peer. It records every request it receives,
    parsed ("body") and as the bytes it arrived as ("raw_body").

    usage is the reply's usage object; add "cost" for OpenRouter's reported
    cost in USD. A streaming request (the built-in llm_call node streams inside
    a Program) is answered with SSE chunks, the usage in the last one, as
    OpenRouter does. A request whose last user message contains "slow" is
    answered after slow_seconds. A request whose last user message contains
    fail_marker is refused with HTTP 400, which the provider reports as a failed
    call with its outcome as evidence.
    """

    def __init__(self):
        self.requests = []
        self.reply = "Paris."
        self.usage = {"prompt_tokens": 21, "completion_tokens": 3, "total_tokens": 24,
                      "cost": 0.001234567}
        self.slow_seconds = 5.0
        self.fail_marker = "fail-call"
        self._lock = threading.Lock()
        peer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                raw_body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                body = json.loads(raw_body)
                with peer._lock:
                    peer.requests.append({"path": self.path, "body": body, "raw_body": raw_body,
                                          "auth": self.headers.get("Authorization")})
                last = (body.get("messages") or [{}])[-1].get("content") or ""
                if "slow" in str(last):
                    time.sleep(peer.slow_seconds)
                if peer.fail_marker and peer.fail_marker in str(last):
                    refusal = json.dumps({"error": {"message": "refused by the e2e peer",
                                                    "code": 400}}).encode()
                    try:
                        self.send_response(400)
                        self.send_header("Content-Type", "application/json")
                        self.send_header("Content-Length", str(len(refusal)))
                        self.end_headers()
                        self.wfile.write(refusal)
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                    return
                base = {"id": "chatcmpl-e2e", "created": 1, "model": body.get("model")}
                if body.get("stream"):
                    frames = [
                        {**base, "object": "chat.completion.chunk",
                         "choices": [{"index": 0, "finish_reason": None,
                                      "delta": {"role": "assistant", "content": peer.reply}}]},
                        {**base, "object": "chat.completion.chunk",
                         "choices": [{"index": 0, "finish_reason": "stop", "delta": {}}]},
                    ]
                    if peer.usage is not None:
                        frames.append({**base, "object": "chat.completion.chunk", "choices": [],
                                       "usage": peer.usage})
                    reply = ("".join("data: " + json.dumps(frame) + "\n\n" for frame in frames)
                             + "data: [DONE]\n\n").encode()
                    content_type = "text/event-stream"
                else:
                    document = {**base, "object": "chat.completion",
                                "choices": [{"index": 0, "finish_reason": "stop",
                                             "message": {"role": "assistant",
                                                         "content": peer.reply}}]}
                    if peer.usage is not None:
                        document["usage"] = peer.usage
                    reply = json.dumps(document).encode()
                    content_type = "application/json"
                try:
                    self.send_response(200)
                    self.send_header("Content-Type", content_type)
                    self.send_header("Content-Length", str(len(reply)))
                    self.end_headers()
                    self.wfile.write(reply)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *args):
                pass

        self._server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._server.daemon_threads = True
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    @property
    def base_url(self):
        return f"http://127.0.0.1:{self._server.server_address[1]}/api/v1"

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._server.shutdown()
        self._server.server_close()

    def request_count(self):
        with self._lock:
            return len(self.requests)


def chat_controls():
    controls = ng.ProviderControls()
    controls.max_output_tokens = OUTPUT_CAP
    return controls


def build_priced_registry(extra_node_types=()):
    """The shared registry plus provider "openrouter" and an llm_call node bound to it."""
    builder = ng.ProgramRegistryBuilder()
    for node_type in (*NODE_TYPES, *extra_node_types):
        builder.add_registered_node(node_type, "1.0.0", digest("node:" + node_type))
    for name in CONDITIONS:
        builder.add_registered_condition(name, "1.0.0", digest("condition:" + name))
    for reducer in ("append", "overwrite"):
        builder.add_registered_reducer(reducer, "1.0.0", digest("reducer:" + reducer))
    builder.add_provider("openrouter", "1.0.0", digest("provider:openrouter"))
    builder.add_registered_node("llm_call", "1.0.0", digest("node:llm_call"), provider="openrouter")
    return builder.build()


def make_bound_host(base_url, *, model=PRICED_MODEL, owner="tenant:priced", budget_ceiling=None,
                    sqlite_directory=None, api_key="local-smoke", input_limits=None,
                    controls=None, extra_node_types=(), **kwargs):
    """A LocalProgramHost whose llm_call nodes call `model` at `base_url`."""
    provider, _routing = openrouter_provider(
        base_url, api_key,
        input_limits={model: PRICED_INPUT_LIMIT} if input_limits is None else input_limits)
    context = ng.NodeContext(provider=provider, model=model,
                             provider_controls=controls or chat_controls())
    return ng.LocalProgramHost(
        build_priced_registry(extra_node_types), owner,
        budget_ceiling if budget_ceiling is not None else PRICED_CEILING,
        node_context=context, binding_label="e2e-openrouter",
        sqlite_directory=sqlite_directory, **kwargs)


# A policy ceiling with room for model tokens and money.
PRICED_CEILING = ceiling(model_tokens=10_000_000, monetary_microunits=10_000_000)


def provider_archive(directory, base_url, owner="tenant:priced", *, provision):
    """NativeArchive custody for a durable host's provider journal.

    The key file lives apart from the records, in an owner-only directory.
    """
    custody = os.path.join(directory, "custody")
    os.makedirs(custody, mode=0o700, exist_ok=True)
    records, key = os.path.join(custody, "records"), os.path.join(custody, "archive-key")
    descriptor = openrouter_descriptor(base_url, input_limits={PRICED_MODEL: PRICED_INPUT_LIMIT})
    opened = (ng.NativeArchive.provision if provision else ng.NativeArchive.open)(
        records, key, owner, descriptor)
    if not isinstance(opened, ng.NativeArchive):
        raise RuntimeError(f"native archive: {opened}")
    return opened


def priced_budget(**overrides):
    """A run budget with room for model tokens and money."""
    values = dict(model_tokens=1_000_000, monetary_microunits=1_000_000)
    values.update(overrides)
    return budget(**values)


def route_ceiling_hold(model=PRICED_MODEL, input_tokens=PRICED_INPUT_LIMIT, output_tokens=OUTPUT_CAP):
    """The reservation the catalog makes for one call, in microunits (rounded up)."""
    with open(CATALOG_PATH, encoding="utf-8") as handle:
        catalog = json.load(handle)
    entry = next(e for e in catalog["entries"]
                 if e["origin"] == "openrouter" and model in [e["model"], *e.get("aliases", [])])
    ceiling_rates = entry["route_ceiling"]

    def micro(rate):  # USD per million tokens -> micro-USD per million tokens
        whole, _, fraction = rate.partition(".")
        return int(whole) * 1_000_000 + int((fraction + "000000")[:6])

    top_input = max(micro(ceiling_rates[key]) for key in ("input", "cache_read", "cache_write")
                    if key in ceiling_rates)
    exact = input_tokens * top_input + output_tokens * micro(ceiling_rates["output"])
    return -(-exact // 1_000_000)
