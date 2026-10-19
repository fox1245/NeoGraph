"""Shared setup for the Program API examples (29 and up).

A Program run needs four things before the example's own story starts: Python
node types registered in this process, an immutable registry snapshot that names
them, a run budget, and a ``LocalProgramHost`` that compiles, admits and runs the
JavaScript. This module holds that setup so each example shows only what it teaches.

    from _program import node, budget, make_host, admit
    import neograph_engine as ng

    @node("demo.work")
    def work(name, input, config):                # input.state, input.ctx
        return [ng.ChannelWrite("value", 1)]

    host = make_host(nodes=["demo.work"])
    version = admit(host, JAVASCRIPT, budget())
    result = host.run(version, {}, budget())

Model-calling nodes (``llm_call``) are bound by ``make_model_host``: the host gets a
provider that talks to a ``_scripted.ScriptedModel``. A Program prices every model call
from a catalog compiled into NeoGraph, so the scripted server is presented as a catalog
model routed through OpenRouter (a loopback origin declared in the provider policy);
nothing leaves 127.0.0.1 and no key is involved.
"""

from __future__ import annotations

import hashlib
import json
import os
import urllib.parse

import neograph_engine as ng

OWNER = "tenant:example"

# A model in the price catalog; the scripted server answers for it.
MODEL = "deepseek/deepseek-v4-flash-0731"
# A call reserves its worst case before it is sent: the model's input window plus the
# output cap. Small values keep a budget of about a thousand tokens usable.
INPUT_LIMIT = 200
OUTPUT_CAP = 64

REDUCERS = ("append", "overwrite")

if not getattr(ng, "_HAVE_PROGRAM", False):
    raise SystemExit("This neograph_engine build has no Program support "
                     "(build with NEOGRAPH_BUILD_PROGRAM=ON and NEOGRAPH_BUILD_QUICKJS_CONTROL=ON).")


def digest(label: str) -> str:
    """A stand-in implementation digest. A real deployment pins the digest of its build."""
    return "sha256:" + hashlib.sha256(label.encode()).hexdigest()


# --------------------------------------------------------------------------
# Node types
# --------------------------------------------------------------------------

def node(type_name):
    """Register ``fn(name, input, config) -> list[ChannelWrite]`` as the node type ``type_name``.

    ``name`` is the node's name in the graph, ``input`` the whole ``NodeInput``
    (``input.state``, ``input.ctx.resume_value``, ``input.ctx.cancel_token``) and ``config``
    the dict from the JavaScript ``g.node(name, {type: ..., ...config})``. ``ng.node`` passes
    only the state. A node may raise ``ng.NodeInterrupt`` to ask a question.
    """
    def decorator(fn):
        class _Node(ng.GraphNode):
            def __init__(self, name, config):
                super().__init__()
                self._name = name
                self._config = dict(config or {})

            def get_name(self):
                return self._name

            def run(self, input):
                return fn(self._name, input, self._config) or []

        ng.NodeFactory.register_type(type_name, lambda name, config, ctx: _Node(name, config))
        return fn

    return decorator


def condition(name):
    """Register ``fn(state) -> route label`` as a conditional-edge condition."""
    def decorator(fn):
        ng.ConditionRegistry.register_condition(name, fn)
        return fn

    return decorator


def registry(nodes=(), conditions=(), model_provider=None):
    """An immutable snapshot of the node types and conditions registered in this process.

    ``append`` and ``overwrite`` are always included. ``model_provider`` is the name under
    which the built-in ``llm_call`` node requires a provider; the host supplies the object.
    """
    builder = ng.ProgramRegistryBuilder()
    for type_name in nodes:
        builder.add_registered_node(type_name, "1.0.0", digest("node:" + type_name))
    for name in conditions:
        builder.add_registered_condition(name, "1.0.0", digest("condition:" + name))
    for name in REDUCERS:
        builder.add_registered_reducer(name, "1.0.0", digest("reducer:" + name))
    if model_provider:
        builder.add_provider(model_provider, "1.0.0", digest("provider:" + model_provider))
        builder.add_registered_node("llm_call", "1.0.0", digest("node:llm_call"),
                                    provider=model_provider)
    return builder.build()


# --------------------------------------------------------------------------
# Budgets
# --------------------------------------------------------------------------

_RUN = dict(wall_time_ms=10_000, model_tokens=0, monetary_microunits=0, max_concurrency=4,
            max_program_operations=16, max_core_steps=50, max_dynamic_compiles=0,
            max_child_depth=0, max_total_children=0)
# A host's ceiling must be positive for every resource except the two child limits,
# even for a Program that never calls a model.
_CEILING = dict(wall_time_ms=120_000, model_tokens=1, monetary_microunits=1, max_concurrency=8,
                max_program_operations=64, max_core_steps=2_000, max_dynamic_compiles=1,
                max_child_depth=0, max_total_children=0)


def _make(defaults, overrides):
    unknown = set(overrides) - set(defaults) - {"max_node_retries"}
    if unknown:
        raise TypeError(f"unknown budget fields: {sorted(unknown)}")
    value = ng.ProgramRunBudget()
    for key, item in {**defaults, **overrides}.items():
        setattr(value, key, item)
    return value


def budget(**overrides) -> "ng.ProgramRunBudget":
    """A run budget. ``compile_admit`` seals this exact budget; ``run`` must pass the same."""
    return _make(_RUN, overrides)


def ceiling(**overrides) -> "ng.ProgramRunBudget":
    """The most any run of this host may be granted."""
    return _make(_CEILING, overrides)


# --------------------------------------------------------------------------
# Hosts
# --------------------------------------------------------------------------

def make_host(nodes=(), conditions=(), *, owner=OWNER, host_ceiling=None, **options):
    """A ``LocalProgramHost`` over a registry of the given node types and conditions.

    ``options`` go to ``LocalProgramHost`` (``scheduler_threads``, ``worker_count``,
    ``soundness_check``, ``failure_text_redactor`` ...).
    """
    return ng.LocalProgramHost(registry(nodes, conditions), owner,
                               host_ceiling or ceiling(), **options)


def admit(host, javascript: str, run_budget, source_id: str = "program.js"):
    """Compile and admit a JavaScript Program; returns the ``ProgramVersion``."""
    return host.compile_admit(ng.ProgramSource.from_javascript(source_id, javascript), run_budget)


def channel(result, name):
    """The final value of a channel in a result's Core output."""
    return ((result.output or {}).get("channels", {}).get(name) or {}).get("value")


def describe(result) -> str:
    """One line for an assertion message."""
    failure = result.failure
    detail = f"{failure.code}: {failure.message[:200]}" if failure else "no failure"
    return f"{result.status.name} trace={result.execution_trace} {detail}"


# --------------------------------------------------------------------------
# A host with a model: llm_call bound to a scripted server
# --------------------------------------------------------------------------

def model_url(scripted) -> str:
    """The base URL of a ``ScriptedModel`` as the OpenRouter-style route used below."""
    return scripted.base_url + "/v1"


def _descriptor(base_url: str):
    endpoint = urllib.parse.urlsplit(base_url.rstrip("/"))
    origin = f"{endpoint.scheme}://{endpoint.netloc}"
    route = endpoint.path.rstrip("/") + "/chat/completions"
    document = json.loads(ng.provider_policy_json())
    chat = next(f for f in document["families"] if f["family"] == "openai.chat")
    chat["openrouter_origins"].append(origin)          # a loopback origin priced as OpenRouter
    # The scripted model's input limit is a catalogue fact, layered over the embedded catalogue.
    models = {"version": 1, "models": [{"id": MODEL, "families": ["openai.chat"], "input_limit": INPUT_LIMIT}]}
    policy = ng.load_provider_policy(json.dumps(document), ng.provider_codec_defaults_json(), json.dumps(models))
    return ng.load_provider_descriptor(json.dumps({
        "descriptor_version": 1, "revision": 1, "id": "example-openrouter-chat",
        "family": "openai.chat",
        "connection": {"base_url": origin, "paths": {"buffered": route, "streaming": route}},
    }), policy)


def native_archive(directory: str, scripted_url: str, *, first: bool, owner: str = OWNER):
    """Custody for a durable host's provider journal (owner-only files under ``directory``).

    ``first=True`` provisions it; a later process opens the same files with ``first=False``.
    """
    custody = os.path.join(os.path.realpath(directory), "custody")   # canonical: macOS temp dirs are symlinks
    os.makedirs(custody, mode=0o700, exist_ok=True)
    opened = (ng.NativeArchive.provision if first else ng.NativeArchive.open)(
        os.path.join(custody, "records"), os.path.join(custody, "archive-key"), owner,
        _descriptor(scripted_url))
    if not isinstance(opened, ng.NativeArchive):
        raise RuntimeError(f"native archive: {opened}")
    return opened


def model_context(scripted_url: str) -> "ng.NodeContext":
    """The provider, model and request controls a host binds to its ``llm_call`` nodes."""
    if not getattr(ng, "_HAVE_LLM", False):
        raise SystemExit("This neograph_engine build has no provider support (NEOGRAPH_BUILD_LLM=ON).")
    from neograph_engine.llm import SchemaProvider

    routing = ng.OpenRouterRouting()
    routing.zdr = True
    defaults = ng.SchemaProviderDefaults()
    defaults.provider = routing
    provider = SchemaProvider(
        _descriptor(scripted_url),
        ng.ProviderRuntimeOptions(api_key="local-smoke", default_timeout_ms=60_000), defaults)
    controls = ng.ProviderControls()
    controls.max_output_tokens = OUTPUT_CAP
    return ng.NodeContext(provider=provider, model=MODEL, provider_controls=controls)


def make_model_host(scripted_url: str, nodes=(), conditions=(), *, owner=OWNER,
                    host_ceiling=None, **options):
    """A host whose ``llm_call`` nodes call the scripted model at ``scripted_url``.

    ``options`` go to ``LocalProgramHost``; ``sqlite_directory=`` makes it durable (then pass
    ``native_history_archive=native_archive(...)`` too).
    """
    return ng.LocalProgramHost(
        registry(nodes, conditions, model_provider="openrouter"), owner,
        host_ceiling or ceiling(model_tokens=10_000_000, monetary_microunits=10_000_000),
        node_context=model_context(scripted_url), binding_label="example-scripted", **options)
