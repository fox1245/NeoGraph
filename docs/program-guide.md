# Program guide: JavaScript that coordinates graphs

**Languages:** [English](program-guide.md) | [한국어](program-guide.ko.md) | [日本語](program-guide.ja.md) | [简体中文](program-guide.zh-CN.md)

A graph has a fixed shape: its nodes, edges and channels are decided when it is compiled. A Program is a small JavaScript module that the host compiles, admits and runs, and that sits one level above the graph. It exports `define()`, which returns the one graph the Program owns, and optionally `main()`, a generator that decides how that graph is run: how many copies at once, how their results are joined, what happens when the budget runs out. The runtime records every step of `main()` in a journal, so a run can pause for a person, stop, and continue.

This page explains the model and walks through one complete Program. The rules are in the [Python binding guide](python-binding.md#program-and-quickjs), what a module may and may not contain in the [authoring rules](program-graphs.md#authoring-rules), the graph features in [Program graph features](program-graphs.md) and the error codes in the [diagnostics guide](diagnostics.md).

## When to use a Program

Use a plain graph when one graph and its checkpoints are enough. A Program is for work around the graph: running several copies in parallel and joining them with a rule (`ng.all`, `ng.race`, `ng.quorum`), keeping a budget over a whole run, recording a run so it can resume in another process, and letting a host admit code that a model or a user wrote. The JavaScript cannot reach the file system, the network, a clock, a random source, `eval`, dynamic `import`, `Promise` or `async`. All it can do is build a graph and yield commands to the runtime. Every node type, condition and reducer it names must be in a registry that the host built, so a generated Program cannot run code the host did not register.

## Requirements

The Program layer needs a build with Program and QuickJS control: `pip install neograph-engine` includes them, and a source build needs `-DNEOGRAPH_BUILD_PYBIND=ON -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON` (then `pip install pydantic certifi` and put the build directory on `PYTHONPATH`).

The example below uses failure routing, `fork` and the SQLite state directory. The 0.14.0 wheel on PyPI has them and the 0.13.1 wheel does not, so with an older wheel run it against a build of the current source, and the same goes for the examples that use them (Python 32, 35 and 36). To start smaller, run [`29_program_hello.py`](../bindings/python/examples/29_program_hello.py), which compiles, admits and runs one Program in under a hundred lines. In C++, start from [`65_program_javascript.cpp`](../examples/65_program_javascript.cpp).

## A worked example: a release check

A release job checks three services in parallel. The checks of `payments` fail. Instead of failing the whole job, the failure goes to a review step, where the job waits for a person. The person approves the release, the other services are not checked again, and the job finishes. The same job runs under a budget.

The example is three Python files. `release_program.py` holds everything that does not change between runs; `run_release.py` and `restart.py` use it.

The listings combine failure routing, a review pause, a budget and a restart in one story. The repository has a runnable example for each of those parts alone: [`32_program_failure_routing.py`](../bindings/python/examples/32_program_failure_routing.py) (the failure route and the operator's `retry` or `abort`), [`34_program_pause_resume.py`](../bindings/python/examples/34_program_pause_resume.py) (`interruptBefore` and `host.resume`), [`33_program_budgets.py`](../bindings/python/examples/33_program_budgets.py), [`35_program_fork.py`](../bindings/python/examples/35_program_fork.py) and [`36_program_crash_recovery.py`](../bindings/python/examples/36_program_crash_recovery.py). The C++ twins are examples [66](../examples/66_program_failure_routing.cpp) to [69](../examples/69_program_crash_recovery.cpp).

### 1. The node code

The nodes are ordinary Python. `check` fails for `payments`. A failure names itself with the attributes `code`, `public_message` and `retryable` of the exception; the text of the exception never leaves the node. `Review` runs after a person has answered, and the answer is its resume value.

```python
import hashlib

import neograph_engine as ng


# 1. The node code. Programs refer to these by name; they stay ordinary Python.

CHECKED = []   # which services ran their checks, to show that nothing runs twice

class ChecksFailed(Exception):
    code = "release.checks_failed"        # an author code: lower-case, dotted
    public_message = "the release checks failed"
    retryable = True


@ng.node("release.check")
def check(state):
    service = state.get("service")
    CHECKED.append(service)
    if service == "payments":
        raise ChecksFailed(f"{service}: 2 tests failed")
    return [ng.ChannelWrite("report", [f"{service}: checks passed"])]


class Review(ng.GraphNode):
    """Runs after a person answered; the answer is the resume value."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        answer = input.ctx.resume_value or {}
        return [ng.ChannelWrite("decision", answer.get("decision", "hold"))]


@ng.node("release.ship")
def ship(state):
    return [ng.ChannelWrite("report", [f"{state.get('service')}: shipped"])]


@ng.node("release.hold")
def hold(state):
    return [ng.ChannelWrite("report", [f"{state.get('service')}: held"])]


ng.NodeFactory.register_type("release.review", lambda name, config, ctx: Review(name))
ng.ConditionRegistry.register_condition(
    "release.decision", lambda state: state.get("decision") or "hold")
```

### 2. The registry

A registry is the list of executable names a Program may use, each with a version and a digest. The host builds it from nodes, conditions and reducers that are already registered in this process.

```python
# 2. The registry: the only executable names a Program may use.

def digest(label):
    return "sha256:" + hashlib.sha256(label.encode()).hexdigest()


def build_registry():
    builder = ng.ProgramRegistryBuilder()
    for node_type in ("release.check", "release.review", "release.ship", "release.hold"):
        builder.add_registered_node(node_type, "1.0.0", digest(node_type))
    builder.add_registered_condition("release.decision", "1.0.0", digest("release.decision"))
    for reducer in ("append", "overwrite"):
        builder.add_registered_reducer(reducer, "1.0.0", digest(reducer))
    return builder.build()
```

### 3. The Program

`define()` describes the graph. `check` is tried twice (`max_retries: 1`). If it still fails, the failure goes to `review` instead of ending the run, and a record of the failure is appended to the `failures` channel. `review` is held by `interruptBefore`, so the run stops there and waits for an answer. After the answer, the condition `release.decision` sends the run on to `ship` or to `hold`, which is a failure exit: it ends the run `Failed` with the code `release.held`.

`main()` runs the graph once for each service with `ng.all`, at most three at a time, and returns the reports in the order of the services.

```python
# 3. The Program: define() describes the graph, main() coordinates runs of it.

SOURCE = r'''
export function define() {
  const g = ng.graph("release");
  g.channel("service", {reducer: "overwrite", initial: ""});
  g.channel("report", {reducer: "append", initial: []});
  g.channel("failures", {reducer: "append", initial: []});
  g.channel("decision", {reducer: "overwrite", initial: ""});
  g.node("check", {type: "release.check"});
  g.node("review", {type: "release.review"});
  g.node("ship", {type: "release.ship"});
  g.node("hold", {type: "release.hold"});
  g.entry("check");
  g.edge("check", "ship");
  g.exit("ship");
  // Two attempts. Then the failure goes to "review" instead of failing the run.
  g.retryPolicy("check", {max_retries: 1, initial_delay_ms: 10, backoff_multiplier: 1,
                          max_delay_ms: 10, on_failure: "review", error_channel: "failures"});
  g.interruptBefore("review");               // a person answers before "review" runs
  g.conditionalEdge("review", "release.decision", {approve: "ship", hold: "hold"});
  g.failureExit("hold", {code: "release.held"});
  return g;
}

export function* main(input) {
  const results = yield ng.all(
    input.services.map((name) => ng.callCore("release", {service: name}, "release:" + name)),
    {max_in_flight: 3}, "fan-out");
  return results.map((result) => result.report);
}
'''
```

Two rules to keep in mind. `ng.callCore` can name only the graph that `define()` returns, by the name given to `ng.graph()`. And a failed command ends the run: the generator is not resumed, so a `try`/`catch` around a `yield` never runs. Handle failures in the graph, as `retryPolicy` does here, or read `result.status` and `result.failure` in the host.

### 4. Budgets and the host

The host has a ceiling for every Program it admits. A Program is admitted together with one run budget, and every run of that version must use exactly that budget. The ceiling's fields, except the two child limits, must be positive even for a Program that never calls a model.

```python
# 4. The budgets. The host ceiling bounds every Program; the run budget bounds one run.

def budget(**fields):
    value = ng.ProgramRunBudget()
    for name, number in fields.items():
        setattr(value, name, number)
    return value


ceiling = budget(wall_time_ms=120_000, model_tokens=1, monetary_microunits=1,
                 max_concurrency=8, max_program_operations=64, max_core_steps=2_000,
                 max_dynamic_compiles=1)
run_budget = budget(wall_time_ms=10_000, max_concurrency=6, max_program_operations=16,
                    max_core_steps=50)


def admit(host, run_budget):
    """Compile the Program, check it against the host's ceiling and return its version."""
    source = ng.ProgramSource.from_javascript("release.js", SOURCE)
    return host.compile_admit(source, run_budget)


def make_host(state_dir=None):
    host = ng.LocalProgramHost(build_registry(), "tenant:docs", ceiling,
                               sqlite_directory=state_dir)
    return host, admit(host, run_budget)
```

`compile_admit` compiles the source, checks the bundle against the ceiling and returns a `ProgramVersion`. The same source, registry and budget give the same version id in another process; the tests check this across processes.

### 5. Run it, and pause

Create `run_release.py`:

```python
from release_program import CHECKED, admit, budget, make_host, run_budget

host, version = make_host()

# Run: "payments" fails its checks, so the run pauses for a person.

services = {"services": ["api", "payments", "web"]}
paused = host.run(version, services, run_budget)
print(paused.status)
print(paused.interrupt.core_node, paused.interrupt.value["failure"]["code"])
```

```text
ProgramTerminalStatus.Interrupted
review release.checks_failed
```

`api` and `web` finished their checks. `payments` failed twice, so its failure was routed to `review`, and the run stopped with the status `Interrupted`. `result.interrupt.core_node` names the node that waits, and `result.interrupt.value` holds `{"reason": "failure_route", "failure": {...}}`, where the failure record has `node`, `kind` (`exhausted` here), `code`, `attempts`, `retryable`, `step`, `routed_to` and `effects`. The reports of `api` and `web` are already in the run.

### 6. Fork it to try the other answer

```python
# Fork the paused run to see what "hold" would do. The original stays paused.
# The fork takes a share of the paused run's remaining budget.

fork_budget = budget(wall_time_ms=2_000, max_concurrency=3, max_program_operations=8,
                     max_core_steps=20)
forked = host.fork(paused.run_id, {"decision": "hold"}, version, fork_budget).wait()
print(forked.status, forked.failure.code)
```

```text
ProgramTerminalStatus.Failed release.held
```

`fork` starts a new run from the paused run's checkpoint and answers the question with `"hold"`. The fork ends `Failed` with the code of the failure exit. The original run is still paused. Finished work is replayed, not run again.

The fork's budget is a share of the original run's remaining budget. `wall_time_ms`, `model_tokens`, `monetary_microunits`, `max_program_operations`, `max_core_steps` and `max_dynamic_compiles` are amounts: the fork takes its share out of what the original has left, and the original keeps the rest. Here the original keeps its remaining `wall_time_ms` less 2,000 and its remaining `max_core_steps` less 20. `max_concurrency` is a limit, not an amount: the fork gets a limit of its own, at most the original's, and the original keeps its own, so the fork's 3 cost the original nothing. A fork that would leave the original with no `wall_time_ms` or no `max_core_steps` is refused at the `fork` call with `P_FORK_SOURCE_BUDGET`, and a fork larger than what the original has left with `P_FORK_BUDGET`. Neither changes the original run.

A node that runs again in the fork, such as the one that asked, repeats the provider calls it made before it asked. A durable provider-call journal lets the fork replay them, so they are neither sent nor charged again; `LocalProgramHost` has one when it has a `sqlite_directory`. A host that keeps everything in memory has none, so there the fork sends and charges those calls again. That is not an error, and what the fork sends and charges does not change, but the fork says so. When the paused attempt recorded provider calls in the paused operation, `handle.notices` on the handle `fork` returned holds one notice with the code `P_FORK_PROVIDER_CALLS_NOT_JOURNALED`; its `witness` lists those calls (`call_id`, `node`, `model`, `microunits`) and their total. The notice cannot tell which of them the fork runs again: a call of a node that finished in the paused step is replayed from the step's saved writes and is not sent again. A fork on a host with a journal, or one whose paused attempt recorded no such call, has no notice. The notice is not stored with the run, so a handle from `reconnect` has none. Details are in the [Python binding guide](python-binding.md#in-memory-state-and-resume) and in [Pricing evidence and the model catalogue](reference-en.md#pricing-evidence-and-the-model-catalogue).

A fork also replays a call after SchemaProvider's descriptor policy has changed, because the journal names a call by what the request asks, not by the policy that admitted it. The handle then carries a `P_PROVIDER_CALL_POLICY_CHANGED` notice, which `handle.notices` shows after `wait()`. If the journal has the source's call but cannot match it, as with a journal written by 0.15, the fork cannot show that the call differs. It fails with `NG.ConfigError` instead of sending and charging the call again. `LocalProgramHost(fork_may_resend_provider_calls=True)` allows the resend, and the handle then carries `P_FORK_PROVIDER_CALL_RESENT`.

### 7. Resume with the answer

```python
# Resume the original with "approve". The other services do not run again.

done = host.resume(paused.run_id, {"decision": "approve"}).wait()
print(done.status)
print(done.output)
print(sorted(CHECKED))
```

```text
ProgramTerminalStatus.Completed
[['api: checks passed', 'api: shipped'], ['payments: shipped'], ['web: checks passed', 'web: shipped']]
['api', 'payments', 'payments', 'web']
```

`resume` answers the pending question and continues the same run. `payments` skips its checks and ships, because a person approved it. The last line shows that nothing ran twice: `api` and `web` appear once, `payments` twice (its two failed attempts).

### 8. A budget that is too small

```python
# A budget that is too small refuses the join before any member starts.

small = budget(wall_time_ms=10_000, max_concurrency=6, max_program_operations=2,
               max_core_steps=50)
refused = host.run(admit(host, small), services, small)
print(refused.status, refused.failure.code)
```

```text
ProgramTerminalStatus.BudgetExhausted P_PROGRAM_OPERATION_BUDGET
```

The join needs more Program operations than this budget allows (`max_program_operations=2`). The runtime refuses the whole join before it starts any member, so no service is checked. A new budget means a new `compile_admit` and a new version; `host.run` with a budget that differs from the admitted one raises `P_START_BUDGET`.

### 9. Survive a restart

By default `LocalProgramHost` keeps everything in memory, and `resume` works only through the host object that started the run. With `sqlite_directory` the host keeps runs, versions and checkpoints in SQLite files, so a new process can continue the run. It must use the same registry and admit the same Program.

Create `restart.py`:

```python
import sys

from release_program import make_host, run_budget

state_dir, step = sys.argv[1], sys.argv[2]
host, version = make_host(state_dir)

if step == "start":
    paused = host.run(version, {"services": ["api", "payments", "web"]}, run_budget)
    print(paused.status, paused.run_id)
else:
    done = host.resume(sys.argv[3], {"decision": "approve"}).wait()
    print(done.status, done.output)
```

```bash
python restart.py state start
python restart.py state resume run-...
```

The first command prints the status `Interrupted` and the run id; pass that id to the second, which prints `Completed` and the reports. The directory `state` holds `program.db`, `checkpoints.db` and `provider_calls.db`.

A run that a process left running, not paused, is continued with `host.reconnect(run_id)`, and what it does depends on the Program. A `define()`-only Program started without child budgets (`max_child_depth` and `max_total_children` both 0, the default) holds an execution lease for its `wall_time_ms`, so that no other process continues a run whose owner may still be alive. `reconnect` raises `P_RUN_LEASE_HELD` until the lease expires, then ends the run `TimedOut` and does not continue it. A Program with `main()`, or one started with child budgets (both above zero), holds no lease, and `reconnect` continues it from its last checkpoint at once, replaying the model calls it had journaled and charging them once ([`36_program_crash_recovery.py`](../bindings/python/examples/36_program_crash_recovery.py)).

## What `define()` can say

`ng.graph(name)` returns a builder. All methods return the builder.

| Method | Meaning |
|---|---|
| `channel(name, {reducer, initial})` | A state channel with a reducer (`overwrite`, `append` or a registered one). |
| `node(name, {type, ...config})` | A node of a registered type; the other keys are its configuration. |
| `edge(from, to)`, `conditionalEdge(from, condition, routes)` | Static and routed edges. The condition is a registered name. |
| `entry(node)`, `exit(node)` | Connect `__start__` and `__end__`. |
| `barrier(node, waitFor)` | A join that waits for every listed node or `{any_of: [...]}` slot. |
| `interruptBefore(...)`, `interruptAfter(...)` | Pause the run before or after the node and wait for an answer. |
| `retryPolicy(node, {...})` | Retries, timeouts and, with `on_failure`, a failure edge. |
| `failureExit(node, {code})` | A node that ends the run `Failed` with that code. |
| `metadata({...})` | Claims about the graph; grants nothing. |

`ng.javascript_authoring_capability_manifest()` returns the exact vocabulary this build accepts, with TypeScript declarations in `dts_prelude`. [Program graph features](program-graphs.md) describes the less common keys.

## What `main()` can yield

Each `yield` hands one command to the runtime and receives the command's result when it completes.

| Command | Result |
|---|---|
| `ng.callCore(name, input, site)` | Runs the graph and returns its output: `channels` with each channel's `value` and `version`, and each channel's value also under its own name. |
| `ng.all(members, options, site)` (`ng.parallel` is the same) | Runs the member commands concurrently, `max_in_flight` at a time, and returns their results in the order given. A failed member fails the run. |
| `ng.race(members, options, site)` | Returns the result of the first member to finish. |
| `ng.quorum(members, required, site)` | Returns once the required number of members succeeded. |
| `ng.await(command, timeoutMs, site)` | Waits for one command up to a deadline. A deadline that passes ends the run `TimedOut` with `P_AWAIT_TIMEOUT`. |
| `ng.spawn(binding, input, site)` | Starts a child Program. It needs a C++ host with a child-binding resolver; `LocalProgramHost` has none and fails with `P_CHILD_BINDING`. |
| `ng.emit(value, site)`, `ng.checkpoint(value, site)` | Record `value` in the run's journal and return it. Each counts as one Program operation. |
| `ng.cancelScope(scope, reason, site)` | Cancels the run (`scope` is `"current"`); the run ends `Cancelled` with `P_RUNTIME_CANCELLED`. |

The last argument of each command is a label for the call site. A paused member of `ng.all` pauses the run: the other members finish, the run is `Interrupted`, and `resume` answers the paused member.

## Statuses

`ProgramResult.status` is a `ProgramTerminalStatus`. Read `result.failure` (`code`, `message`, `core_node`, `witness`) when it is not `Completed`.

| Status | Meaning |
|---|---|
| `Completed` | `main()` returned, or the graph reached its exit. `result.output` holds the value. |
| `Interrupted` | The run waits for an answer; `result.interrupt` says where. `resume` or `fork` continues it. |
| `Failed` | A failure the graph did not route, a failure exit, or `main()` threw. `result.failure.code` names it. |
| `BudgetExhausted` | A budgeted resource ran out, for example `P_PROGRAM_OPERATION_BUDGET`, `P_CORE_STEP_BUDGET` or `P_CONCURRENCY_BUDGET`. Model tokens end the run with `NG.BudgetExhausted`, whether a reply took the run over the budget or the next call was refused before it was sent. Money and Core steps end it with a record of how far the run went over ([Budgets](#budgets)), also when the run finishes over its grant. |
| `TimedOut` | The run's `wall_time_ms` or an `ng.await` deadline passed. A run that completes after its `wall_time_ms` is `TimedOut` too ([Budgets](#budgets)). |
| `Cancelled` | `ng.cancelScope` or the handle's `cancel()` stopped the run. |

`AmbiguousEffect` and `CheckpointIncompatible` also exist; see `include/neograph/program/result.h`.

## Budgets

A `ProgramRunBudget` has ten fields: `wall_time_ms`, `model_tokens`, `monetary_microunits`, `max_concurrency`, `max_program_operations`, `max_core_steps`, `max_dynamic_compiles`, `max_child_depth`, `max_total_children` and `max_node_retries`. `result.usage` counts what a run used and `result.remaining_budget` is what is left. Zero grants nothing: a run with `model_tokens = 0` that calls a model ends `BudgetExhausted`. The ceiling, the admitted budget and the model-cost rules are in the [Python binding guide](python-binding.md#budgets).

A run that ends over a budget carries a failure record that names the budget, and the witness says how far over it went. `failure.witness` has an entry under the budget's key with `granted`, `used` and `over_by`; `over_by` is 0 when the run was stopped at its grant, or when a model call was refused before it was sent.

| Budget | Status | `failure.code` | Witness key |
|---|---|---|---|
| `wall_time_ms` | `TimedOut` | `P_RUNTIME_TIMEOUT` | `wall_time_ms` |
| `model_tokens` | `BudgetExhausted` | `NG.BudgetExhausted` | `model_tokens` |
| `monetary_microunits` | `BudgetExhausted` | `P_MONEY_BUDGET` | `monetary` |
| `max_core_steps` | `BudgetExhausted` | `P_CORE_STEP_BUDGET` | `core_steps` |

The record is the same whether the budget stopped the run on the way or the run finished over it. Usage is settled once the plan has completed, and a plan can finish over a grant: a reply can cost more than was reserved for its call, a node that ignores cancellation can return after the deadline, and the steps of a subgraph node count toward the run's Core steps without stopping the graph that contains it. When a run is over several budgets, the witness has an entry for each, and the status and code are those of the first in the order of the table (the order of the budget fields). Wall time comes first because the deadline ends a run `TimedOut` whatever else the run used. A priced call that is refused before it is sent keeps the evidence of the refusal under `monetary`: the reservation, the `ceiling` and what was `used`.

The reservation for a priced call prices the call's output cap at the dearest output rate, which bounds the spend only if the model's upstream stops at the cap. NeoGraph sends the cap and cannot check that an upstream applies it, so the reply of an upstream that ignores it can cost more than its reservation. The call is charged what its response reports, the run's remaining `monetary_microunits` shrinks by that amount, and a run whose charges pass its grant ends `BudgetExhausted` with `P_MONEY_BUDGET`, as above. Within the grant the run goes on. NeoGraph reports each such call as a `P_MONEY_OVER_HOLD` notice on the run's handle (`handle.notices`, read after `wait()`), and the call's record in `result.monetary_records` has `microunits` above its `reserved_microunits`. Both name the upstream that served the call (`served_by`) when the response names one. The notice changes nothing else, and NeoGraph does not refuse a route after it: that is the host's policy. A host that relies on the cap can route its OpenRouter calls only to upstreams it knows to honour it (`only` or `order` in the provider's routing), act on the notices and the records after each run, and grant less than it can afford to lose. The paragraph on the output cap in [Pricing evidence and the model catalogue](reference-en.md#pricing-evidence-and-the-model-catalogue) has the details.

## Where a Program's work runs (C++ hosts)

A C++ host builds a `ProgramRuntime` from a `RuntimeConfig`. Unless the host says otherwise, the runtime runs attempts on a pool of `scheduler_threads` threads and deadline timers on one more thread, which is what `LocalProgramHost(scheduler_threads=...)` sets. A host with an executor of its own sets `RuntimeConfig::executor`: a single-thread `io_context`, a `thread_pool` it shares with other work, or `async::HostLoop::executor()` on a WebAssembly host. The runtime then starts no thread; every attempt, deadline timer and waiter runs on that executor, and nothing runs inline in the call that started it.

- **Every operation has an awaitable twin.** `start_async`, `reconnect_async`, `resume_handle_async`, `fork_async`, `replace_async`, `migrate_graph_async` and the rest run on the runtime's executor and complete on the executor of the coroutine that awaits them. `run_async` and `resume_async` wait for the result.
- **Synchronous calls and `ProgramHandle::wait` can refuse.** On an executor that a host's event loop pumps, and in a build without threads, they would block the thread that has to run the Program, so they throw `ProgramBlockingCallRefused` before doing anything. `operation()` names the call and `alternative()` the awaitable form. A wait made on a thread that runs the injected executor is refused too. A native runtime without an executor is unchanged.
- **A node body must not block its thread** when the host gave an executor: a node attempt that needs its own worker runs on a strand of it, where a body that blocks holds every run that shares the executor.
- **Destroying the runtime** cancels its runs and, on an injected executor, does not wait for them. Keep running the executor until the handles are terminal.

The WebAssembly side, with a runnable check, is in [`wasm/README.md`](../wasm/README.md).

## Where a Program's state is kept (C++ hosts)

A `ProgramRuntime` keeps its run records in a `ProgramTransitionStore` and its versions in a `ProgramStore`, and, when the host uses them, task graph fragments, linked modules and Core tool grants in a `TaskGraphFragmentStore`, a `ModuleStore` and a `ProgramCoreToolGrantStore`. The interfaces return `asio::awaitable`, so a host can put a store on a database whose calls complete later, and the runtime awaits them instead of holding a thread.

- **A synchronous backend derives from the matching `Sync...Store` class** (`SyncProgramTransitionStore` and four more) and implements the `..._sync` operations (`load_sync`, `compare_publish_sync`, ...). The base completes each awaitable operation from them: at once on the calling thread, or on a thread of a bounded pool when the store overrides `offloads()` to return true. The in-memory and SQLite stores complete at once; PostgreSQL offloads, and the awaiting coroutine resumes on its own executor. The arguments of an awaited call, string views included, must stay alive until it completes.
- **A caller that cannot suspend uses a blocking view**: `BlockingProgramTransitionStore` and its four siblings have the signatures the interfaces had before. A view over a synchronous store calls its `..._sync` operation. Over any other store it drives the awaitable on a private `io_context` of the calling thread and waits, unless its `BlockingStorePolicy::refuse` says that waiting would hang the thread that has to complete the call. The runtime's policy refuses on a host-pumped executor and on a thread that runs its executor, and the call throws `ProgramBlockingCallRefused` before the store is touched.
- **What awaits and what waits.** Every operation of the runtime is one coroutine that awaits its stores: the entry points (`start`, `reconnect`, `resume`, `fork`, `replace` and the rest), the terminal publication of a run, the child launch and its recovery, the Core tool grant resolver when it is the awaitable one (`RuntimeConfig::core_tool_grant_awaitable_resolver`, built from a store by `make_durable_core_tool_grant_awaitable_resolver`), `ProgramCatalog` (`resolve_version_async`, `admit_async` and the other `_async` forms) and `ModuleResolver::resolve_async`. The `_async` form runs that body on the runtime's executor. The synchronous form runs the same body to completion on the calling thread, and is refused where waiting would hang that thread. A store that completes only in a later turn of a host's event loop therefore works on a host-pumped executor through the `_async` forms (`ProgramHandle::cancel_async`, `snapshot_async`, `latest_handoff_async` and `next_handoff_async` included), and the synchronous forms are refused inside a turn of that loop. What still blocks its caller: `ProgramHandoff::reference()`, `RuntimeInstructionController`, `inspect_program_replacement_chain` and a `hook_runtime`, and the runtime reads its `graph::CheckpointStore` with the store's synchronous calls.
- **A call that drops its awaitable does nothing.** The operations are `[[nodiscard]]`: `co_await` them, or use a blocking view.
- **The contract** every store meets (atomic publication, head compare-and-swap, fail closed) is run against the in-memory, SQLite and PostgreSQL stores, once through each form, by the conformance suites in `tests/test_program_transition_store.cpp` and `tests/test_program_catalog.cpp`.

## Where the behaviour is tested

Each step of the example corresponds to scenarios in `bindings/python/tests/program_e2e`: `test_07_main_programs.py` (`ng.all`, `race`, `quorum`, `await`), `test_10_failure_routing.py` (the failure route and the approval), `test_02_hitl.py` (interrupt and resume, in memory and after a restart), `test_04_budgets.py` (budgets), and `test_13_fork_replays_the_paused_step.py` and `test_20_javascript_fork.py` (forks). The [README of that directory](../bindings/python/tests/program_e2e/README.md) lists all of them. The Hooks, provider calls and child Programs that the example leaves out are covered in the [Python binding guide](python-binding.md) and in [recursive Harnesses](PROGRAM_RECURSIVE_HARNESSES.md).
