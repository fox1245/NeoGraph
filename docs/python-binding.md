# Python Binding

**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)

`neograph-engine` is the pybind11 surface of the same C++ runtime. The wheel enables Core, LLM, Program/QuickJS, MCP and SQLite runtime durability; optional source builds expose only the components they compile.

```bash
pip install neograph-engine
```

## Core graph quickstart

```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite("messages", [
        {"role": "assistant", "content": f"Hello, {state.get('name')}!"}
    ])]

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "demo",
    "channels": {
        "name": {"reducer": "overwrite"},
        "messages": {"reducer": "append"},
    },
    "nodes": {"greet": {"type": "greet"}},
    "edges": [
        {"from": ng.START_NODE, "to": "greet"},
        {"from": "greet", "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))
print(result.output["channels"]["messages"]["value"])
```

## Core API parity

Python exposes the C++ execution abilities rather than a separate Python scheduler:

- sync and asyncio run/stream/resume;
- exact-checkpoint `resume_from`, fork, state inspection and ordered state writes;
- static and dynamic HITL through graph interrupts and `NodeInterrupt`;
- `RunMetadata` deadlines, trace/run identity and model-token ceilings;
- graph-wide and per-node `RetryPolicy`, including jitter;
- execution-local or explicitly reusable `CacheScope`;
- checkpoint and long-term Store backends;
- custom nodes, reducers, conditions, providers and tools;
- tool gates, execution policy, mandatory lifecycle Hooks and strict runtime interposition.

### Parity contract

“Parity” means that Python reaches the same native execution path and safety contract; it does not mean that every internal C++ storage or authority type is copied into Python.

| Capability | Native C++ path | Python surface | Status |
|---|---|---|---|
| Compile and execute Core graphs | `GraphEngine` | `GraphEngine.compile`, run/stream/async methods | Same scheduler and runtime |
| Runtime identity, deadlines and budgets | `RunMetadata`, `RunConfig` | `RunMetadata`, `RunConfig.model_token_budget` | Same per-run values |
| Retry and node-cache policy | `RetryPolicy`, `CacheScope` | graph/node setters and cache scopes | Same runtime policy |
| Checkpoint, HITL and time travel | checkpoint stores and resume APIs | resume, exact `resume_from`, fork, state history/update | Same checkpoint contract |
| Program authoring and local execution | compiler, Catalog and `ProgramRuntime` | `ProgramCompiler`, `LocalProgramHost`, handles/results | Native owner-scoped convenience host |
| Mandatory lifecycle Hooks | registry, runner and `HookRuntime` | definitions plus `create_hook_runtime` callbacks | Same fail-closed lifecycle boundary |
| Runtime context and strict dispatch | context stores, receipts and interposition | matching immutable values, stores and `StrictRuntimeProfile` | Same native controller |
| Durable wheel defaults | SQLite Core/context/dispatch stores | `_HAVE_SQLITE` exports | Enabled in the PyPI wheel |

Raw `ProgramCatalog`, transition stores, replacement/migration controllers, synthesis gateways, Hook journals and RPC executors remain host-composition APIs. Exposing only fragments of those authority-bearing paths would bypass the required `proposal -> compile -> admit -> publish -> migrate/spawn` protocol. A future Python host controller must bind that protocol and its nonrenewable lineage budget as one owner-scoped unit; `_HAVE_PROGRAM` does not claim raw control-plane administration parity.

### Runtime retry overrides

```python
policy = ng.RetryPolicy()
policy.max_retries = 3
policy.initial_delay_ms = 100
policy.backoff_multiplier = 2.0
policy.max_delay_ms = 2_000
policy.jitter_pct = 0.2

engine.set_retry_policy(policy)
engine.set_node_retry_policy("remote_call", policy)
```

The graph definition's `"retry_policy"` remains the declarative default. Runtime setters are a distinct C++/Python configuration surface.

### Metadata and exact resume

```python
config = ng.RunConfig(thread_id="job-42", input={"task": "..."})
config.model_token_budget = 20_000
metadata = ng.RunMetadata(
    timeout_ms=30_000,
    trace_id="trace-42",
    run_id="run-42",
    owner_scope="tenant-a",
)
result = engine.run(config, metadata)

# Never substitutes a newer checkpoint:
result = engine.resume_from(config, checkpoint_id, {"approved": True}, metadata)
```

Inside a Python node, the same values are available through `input.ctx.trace_id`, `run_id`, `has_deadline`, `deadline_remaining_ms`, and `model_token_budget`.

### Cache scope

```python
engine.set_node_cache_enabled("pure_parser", True)  # execution-local default
engine.set_node_cache_enabled("pure_parser", True, ng.CacheScope.Reusable)
```

`Reusable` is an explicit assertion that the node is independent of tenant, provider, Store, tools, credentials, time and resume state.

## Program and QuickJS

Python wheels build `neograph::program` and the restricted QuickJS frontend. A Python-defined node can participate in an immutable Program registry and execute through the native `ProgramRuntime`.

```python
import neograph_engine as ng

registry = (
    ng.ProgramRegistryBuilder()
    .add_registered_node(
        "my_node", "1.0.0", "sha256:" + "1" * 64
    )
    .add_registered_reducer(
        "overwrite", "1.0.0", "sha256:" + "2" * 64
    )
    .build()
)

source = ng.ProgramSource.from_javascript("agent.js", r'''
export function define() {
  const graph = ng.graph("main");
  graph.channel("value", {reducer: "overwrite", initial: 0});
  graph.node("work", {type: "my_node"});
  graph.entry("work");
  graph.exit("work");
  return graph;
}
export function* main(input) {
  return yield ng.callCore("main", input, "python:main");
}
''')

ceiling = ng.ProgramRunBudget()
ceiling.wall_time_ms = 10_000
ceiling.model_tokens = 1_000
ceiling.monetary_microunits = 1_000
ceiling.max_concurrency = 2
ceiling.max_program_operations = 32
ceiling.max_core_steps = 20
ceiling.max_dynamic_compiles = 1

run_budget = ng.ProgramRunBudget()
run_budget.wall_time_ms = 10_000
run_budget.max_concurrency = 2
run_budget.max_program_operations = 32
run_budget.max_core_steps = 20

host = ng.LocalProgramHost(registry, "tenant-a", ceiling)
version = host.compile_admit(source, run_budget)
result = host.run(version, {}, run_budget)
```

`LocalProgramHost` is an owner-scoped in-memory convenience host. It still uses the C++ compiler, Catalog, admission policy, transition store and ProgramRuntime. Generated proposals should additionally pass a host semantic validator before admission; see [DSL capability evaluation](DSL_CAPABILITY_EVAL.md).

The exact installed JavaScript vocabulary is available as a dict:

```python
manifest = ng.javascript_authoring_capability_manifest()
```

## Mandatory lifecycle Hooks

Hooks are triggered by host lifecycle events, not by a model deciding to call a tool.

```python
data = ng.HookDefinitionData()
data.phase = ng.HookPhase.CheckpointPublished
data.target_id = "audit"
data.delivery = ng.HookDelivery.BlockingMandatory
data.failure_mode = ng.HookFailureMode.FailClosed
data.effect = ng.ToolEffectClass.ReadOnly

mapper = ng.HookInputMapper()
mapper.kind = ng.HookInputMapperKind.Template
mapper.value_template = {"kind": "checkpoint"}
data.input_mapper = mapper

definition = ng.HookDefinition.create(data)
runtime = ng.create_hook_runtime(
    [definition],
    {"audit": lambda arguments, event_type, event_data: persist(arguments)},
)
engine.set_hook_runtime(runtime)
```

A callback failure under `FailClosed` blocks the protected runtime boundary. `Continue` is available only when observational loss is acceptable.

## Runtime context, Skills and strict dispatch

The binding exposes immutable RAW history records, context artifacts, epochs, required Skills/constraints, transformation receipts and provider dispatch receipts.

```python
requirements = ng.RuntimeContextRequirements()
requirements.required_artifact_ids = [skill.id, constraint.id]
requirements.required_skill_artifact_ids = [skill.id]

assembler = ng.RuntimeTurnAssembler(
    context_store,
    max_input_tokens=32_000,
    requirements=requirements,
)
```

`ContextTransformReceipt` permits arbitrary derived evidence but requires every required artifact to remain byte-identical.

For the full strict path use durable SQLite stores:

```python
contexts = ng.SQLiteContextStore("runtime.sqlite3")
receipts = ng.SQLiteProviderDispatchReceiptStore("runtime.sqlite3")
hooks = ng.create_hook_runtime(definitions, callbacks)

profile = ng.StrictRuntimeProfile(
    provider,
    contexts,
    receipts,
    hooks,
    provider_binding_identity,
    max_input_tokens=32_000,
    required_context_artifact_ids=[constraint.id],
    required_skill_artifact_ids=[skill.id],
)
profile.activate("tenant-a", strict_epoch)
completion = profile.invoke(params)
profile.attach(engine)
```

## HITL and state

Static `interrupt_before`/`interrupt_after`, dynamic `NodeInterrupt`, synchronous `resume`, asyncio `resume_async`, and exact `resume_from` require a checkpoint store.

```python
if result.interrupted:
    result = engine.resume(result_thread_id, {"approved": True})
```

Use `get_state_history`, `update_state`, and `fork` for inspection and time-travel. `get_state_view()` provides flat Pydantic-backed channel access while `get_state()` retains the canonical nested representation.

## Async and cancellation

`run_async`, `run_stream_async`, and `resume_async` return `asyncio.Future` objects. Cancelling the Future propagates through `CancelToken` into in-flight native I/O. Streaming callbacks are marshalled back to the caller's asyncio loop thread.

Python-defined providers implement synchronous `complete`/`complete_stream`; async-native provider implementations remain C++ extensions.

## Protocols and observability

- MCP client tools are available through `neograph_engine.mcp` when built.
- A2A client types are available through `neograph_engine.a2a` when built.
- `ProtocolHostAdapter` integrates official Python A2A/ACP server SDKs with NeoGraph session semantics.
- `neograph_engine.tracing` and `neograph_engine.openinference` emit vendor-neutral OTel/OpenInference data for Phoenix, Langfuse, Arize and compatible backends.

## Optional components

The public package marks optional C++ components honestly:

- `_HAVE_PROGRAM`, `_HAVE_SQLITE`, `_HAVE_POSTGRES`, `_HAVE_MCP`, `_HAVE_A2A`;
- missing components are absent rather than emulated in Python;
- the PyPI wheel enables Program/QuickJS, LLM, MCP and SQLite; source builds follow their CMake options.

## Tests and examples

The binding suite covers Core execution, custom callbacks, asyncio, cancellation, Program compilation/runtime, mandatory Hooks, strict context, SQLite persistence, protocols and README examples.

- [Python examples](../bindings/python/examples/README.md)
- [C++ examples](../examples/README.md)
- [QuickJS authoring boundary](QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
- [Strict runtime interposition](STRICT_RUNTIME_INTERPOSITION.md)
