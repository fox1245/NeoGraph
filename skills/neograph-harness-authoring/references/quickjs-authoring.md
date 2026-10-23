# QuickJS source authoring

**Languages:** [English](quickjs-authoring.md) | [한국어](quickjs-authoring.ko.md) | [日本語](quickjs-authoring.ja.md) | [简体中文](quickjs-authoring.zh-CN.md)

## Take the names from the host

The request and the supplied manifest give every name you may use: node types,
reducers, conditions, the graph (Core) name, child bindings, import slots, the input
shape and what `main` must return. The manifest lists syntax only (signatures,
option keys, limits); it contains no application nodes. If a needed name is
missing, say which one. A name that is merely plausible is wrong.

The graph name is one of these. When the host gives it, pass exactly that string to
`ng.graph(...)` and to every `ng.callCore(...)`; do not make one up from the task.

## The module

An ES module with the global `ng`. It exports `define` and, when the task needs
runtime control, a generator `main`. There is no `require`, `module.exports` or
`export default`, and dynamic `import()` is rejected. A static `import` works only for
a module the host sealed and named in the task; any other `import` is rejected.

    // GRAPH, REDUCER and NODE_TYPE stand for names the host supplied.
    export function define() {
      const g = ng.graph("GRAPH");
      g.channel("value", {reducer: "REDUCER", initial: 0});
      g.node("step", {type: "NODE_TYPE"});
      g.entry("step");
      g.exit("step");
      return g;
    }

    export function* main(input) {
      const result = yield ng.callCore("GRAPH", {value: input.value}, "run");
      return {value: result.channels.value.value};  // a graph run's output
    }

Do not use `async`, Promises, timers, `eval`, `Date` or I/O: `Date` and `eval`
throw, `setTimeout` does not exist and `Math.random()` always returns 0. Anything
returned, and every command input, must be plain JSON: no `undefined`, functions,
`Date` or `Map`.

## define(): the graph

`define()` is synchronous and returns the builder from `ng.graph(name)`. Commands
such as `ng.callCore` do not exist here.

- Every builder method returns the same builder, never a node handle. Use the
  manifest's signatures and option keys exactly. Keys are snake_case
  (`initial_delay_ms`, not `initialDelay`). An unknown key is rejected in `channel`,
  `retryPolicy` and command options; extra keys in a `node` config go to the node
  type, and the host's registry decides whether it accepts them.
- A graph needs `entry`; without it nothing would run and the compiler rejects it.
  `exit(node)` marks where the run ends (a node with no outgoing edge ends it too).
  Every edge, barrier, interrupt and retry must name a declared node. `__start__` and
  `__end__` are never node names, but an edge or a route can target `__end__`.
- `node(name, {type, ...config})`: `type` is required and must be registered.
  `channel(name, {reducer, initial})`: name the registered reducer.
- `conditionalEdge(from, condition, routes)`: `routes` maps every label the
  registered condition can return, and no other label, to a node.
- Fan-out is several edges from one node. A join that must wait for all branches
  also needs `barrier(join, [branchNodes])`. Concurrent writes to one channel need
  a reducer that merges them.

## main(): runtime commands

`main` is a synchronous generator. Each `yield` hands the runtime exactly one
command built by an `ng` constructor and returns that command's result. A raw
array, a plain object, a Promise or `yield*` on a command is rejected. Ordinary JavaScript
(branches, loops, `map`) builds commands and arrays of commands; run many at
once by yielding one `ng.all(commands, {max_in_flight: N})`.

| Constructor | Meaning |
|---|---|
| `ng.callCore(name, input, site)` | Run the graph. `name` is the string given to `ng.graph`; `input` maps channel names to values. A graph run's output is read as `result.channels.NAME.value`. |
| `ng.all(commands, options, site)` | Run the commands together, `max_in_flight` at a time; returns their results in order. `ng.parallel` is the same. |
| `ng.race(commands, options, site)` | First result wins; needs at least two commands. |
| `ng.quorum(commands, k, site)` | Done after `k` succeed; `1 <= k <= commands.length`. |
| `ng.join(commands, mode, options, site)` | General form. `mode` is `"all"`, `"race"` or `"quorum"` (with `required_successes`). `failure_policy` is `"fail_fast"` or `"collect"`. |
| `ng.spawn(binding, input, site)` | Start the host-admitted child. Wrap it in `ng.await(spawnCommand, timeoutMs, site)` to get its result. |
| `ng.emit(value, site)`, `ng.checkpoint(value, site)` | Record a JSON value in the run's journal. |
| `ng.cancelScope(scope, reason, site)` | Cancel `"current"`, `"nested"`, `"root"` or `"run"`; another name fails the run. At the top level of `main` it ends the run as cancelled. |
| `ng.hostCapability(slot, input, site)` | Call the host-admitted import slot (an integer). |

- `site` is an optional last argument: a stable label such as `"item:" + id`.
  Derive it from ids or indexes, never from time or randomness. A recovered run
  re-runs `main`, so it must yield the same commands for the same results.
- Option keys are snake_case. `max_in_flight`, `required_successes` and the await
  timeout are positive integers.
- A failed command, including one member of `ng.all`, ends the run and `main` is
  never resumed, so `try`/`catch` around a `yield` cannot recover it. For a logical
  retry, loop on a field of the result (for example a `done` flag). Retrying a
  failing node belongs to `retryPolicy` in `define()`.
- Read a result in the shape the task states. If it says a response carries a field
  such as `status`, that field is `result.status`; the `channels` wrapper belongs
  only to a graph run's output, so do not add it to a response the task describes
  differently.
- A module with only `define()` returns the Core result. With `main`, the value
  it returns must match the output contract the host admitted.

## Reply and repair

For source evaluation, reply with one JSON object that has exactly one string
field, `source`, holding the whole module. JSON-escape it (`\n`, `\"`) and put
nothing before or after the braces: no Markdown fence, no prose, no patch, no
Program JSON.

    {"source": "export function define() {\n  ...\n}\n"}

For the Harness MCP, call `neograph_schema` first, then send the module as
`harness.mode: "javascript"` with `source_id`, `source` and every required
task, worker, budget and policy field. Source alone is not a request. Use
`neograph_compile` only if the tool is available.

A compiler rejection comes back as diagnostics with `code`, `message`, a JSON
`pointer` into the lowered definition and a `witness`; a check the host runs on the
result may return a plain error message instead. Fix the cause the rejection cites,
keep everything the task fixed, and return the complete module in the same envelope.
Do not widen grants, add unrelated operations, or claim success before the host's
checks accept it. A source that compiles can still build the wrong topology.
