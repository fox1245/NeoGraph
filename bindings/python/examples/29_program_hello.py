"""29 - Program hello: a short JavaScript Program defines one graph and runs it.

A Program is JavaScript with two exports. `define()` returns the one graph the Program
may run; its nodes are Python classes you registered. `main()` is optional and drives the
graph: `ng.callCore(name, input)` runs it and hands back its output. NeoGraph compiles the
text against an immutable registry of the node types it may use, admits it under a policy
ceiling, and only then runs it. This example does each step once and prints the fields of
the result a caller reads first. Later examples (30 and up) move this setup into `_program.py`.

Run:
    python 29_program_hello.py

smoke-expect: status: Completed
smoke-expect: value: 1
smoke-expect: trace: ['work']
smoke-expect: failure: None
smoke-forbid: [neograph] lint
"""

import hashlib

import neograph_engine as ng


# 1. A node is plain Python. It reads channels from `state` and returns channel writes.
@ng.node("hello.increment")
def increment(state):
    return [ng.ChannelWrite("value", (state.get("value") or 0) + 1)]


# 2. The registry names every executable a Program may use, with a version and a digest
#    of its implementation. A Program that names anything else does not compile.
def digest(label):
    return "sha256:" + hashlib.sha256(label.encode()).hexdigest()


registry = (
    ng.ProgramRegistryBuilder()
    .add_registered_node("hello.increment", "1.0.0", digest("node"))
    .add_registered_reducer("overwrite", "1.0.0", digest("reducer"))
    .build()
)

# 3. The Program: define() declares the graph, main() runs it once and returns its output.
SOURCE = r"""
export function define() {
  return ng.graph("hello")
    .channel("value", {reducer: "overwrite", initial: 0})
    .node("work", {type: "hello.increment"})
    .entry("work")
    .exit("work");
}
export function* main(input) {
  return yield ng.callCore("hello", input, "hello:main");
}
"""


# 4. A budget bounds one run. The host's ceiling bounds every budget it will admit.
def budget(**limits):
    value = ng.ProgramRunBudget()
    for name, amount in limits.items():
        setattr(value, name, amount)
    return value


run_budget = budget(wall_time_ms=10_000, max_concurrency=2, max_program_operations=16,
                    max_core_steps=20)
host_ceiling = budget(wall_time_ms=60_000, model_tokens=1, monetary_microunits=1,
                      max_concurrency=4, max_program_operations=64, max_core_steps=100,
                      max_dynamic_compiles=1)

# 5. compile_admit seals the budget into the Program version. run() must pass the same one.
host = ng.LocalProgramHost(registry, "tenant:example", host_ceiling)
version = host.compile_admit(ng.ProgramSource.from_javascript("hello.js", SOURCE), run_budget)
result = host.run(version, {}, run_budget)

# 6. The result. `output` is what main() returned: the Core's channels plus a copy of each
#    channel value under its own name. `execution_trace` lists the Core nodes that ran.
print("status:", result.status.name)
print("value:", result.output["value"])
print("trace:", result.execution_trace)
print("failure:", result.failure)
print("core steps used:", result.usage.core_steps, "of", run_budget.max_core_steps)

assert result.status == ng.ProgramTerminalStatus.Completed
assert result.output["value"] == 1 and result.output["channels"]["value"]["version"] == 1
assert result.execution_trace == ["work"] and result.failure is None
