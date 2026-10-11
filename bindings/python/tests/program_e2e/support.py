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
import json
import os
import subprocess
import sys
import textwrap
import threading
import time

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


class Step(ng.GraphNode):
    """One configurable Python node type, driven by its JavaScript config.

    Config keys (all optional):
      boom: true                 raise RuntimeError
      ask: "question"            raise ng.NodeInterrupt until a resume value arrives
      delay_ms: N                cooperative sleep that polls input.ctx.cancel_token
      blocking_sleep_ms: N       plain time.sleep (a node that ignores cancellation)
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
        if config.get("boom"):
            JOURNAL.add("raised", self._name)
            raise RuntimeError(f"boom from {self._name}")
        if config.get("blocking_sleep_ms"):
            time.sleep(config["blocking_sleep_ms"] / 1000.0)
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


def _route_by_topic(state):
    return "billing" if state.get("topic") == "billing" else "general"


def _until_target(state):
    return "done" if (state.get("counter") or 0) >= (state.get("target") or 0) else "again"


NODE_TYPES = ("e2e.step", "e2e.fan", "e2e.square")
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
    for name, fn in CONDITIONS.items():
        ng.ConditionRegistry.register_condition(name, fn)


register_python_types()


def digest(label: str) -> str:
    return "sha256:" + hashlib.sha256(label.encode()).hexdigest()


def build_registry(extra_node_types=()) -> "ng.ProgramRegistrySnapshot":
    builder = ng.ProgramRegistryBuilder()
    for node_type in (*NODE_TYPES, *extra_node_types):
        builder.add_registered_node(node_type, "1.0.0", digest("node:" + node_type))
    for name in CONDITIONS:
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
