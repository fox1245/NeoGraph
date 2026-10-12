# NeoGraph documentation

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

This page lists the documents in this directory, with the main READMEs elsewhere in the repository, and says who each one is for. If you are new, read the first table in order. If you are looking for a class or a function, go to the reference. The last two tables hold detailed Program contracts and measurements; read them when you need that depth, not to get started.

## Start here

| Read | Why |
|---|---|
| [README](../README.md) | What NeoGraph is, a Python and a C++ quickstart that run without an API key, and the build options. |
| [Concepts](concepts.md) | Channels, reducers, nodes, edges, `Send`, `Command`, checkpoints, interrupts and streaming, in the order you use them to build a graph. |
| [C++ examples](../examples/README.md) and [Python examples](../bindings/python/examples/README.md) | Runnable programs. Most need no API key; each index says which do. |
| [Troubleshooting](troubleshooting.md) | Build, install, runtime and Python errors, with the cause and the fix for each. |

## Guides

| Read | If you want to |
|---|---|
| [Python binding](python-binding.md) | Use NeoGraph from Python: graphs, typed provider requests, Programs, Hooks, checkpoints, protocol clients. |
| [Program guide](program-guide.md) | Write a restricted JavaScript program that runs graphs in parallel, handles a failed node, waits for a person, resumes and stays inside a budget. |
| [Program graph features](program-graphs.md) | Look up what a Program's `define()` graph can express: metadata, `any_of` joins, per-node retry, failure edges and exits, attempt timeouts, the soundness check. |
| [Diagnostics](diagnostics.md) | Look up a compiler or runtime diagnostic code (`P_JS_*`, `P_CORE_*`, `P_RUNTIME_*`) and its fix. |
| [MCP](mcp.md) | Expose a graph as MCP tools, or call tools on an MCP server from a graph. |
| [A2A](a2a.md) | Expose a graph as an Agent2Agent agent, or call a remote A2A agent. |
| [ACP](acp.md) | Run a graph as an Agent Client Protocol agent that an editor can start over stdio. |
| [Harness MCP](HARNESS_MCP.md) | Use the Harness: six MCP tools that compile and run bounded multi-worker workflows, with replay and review. |
| [Async guide](ASYNC_GUIDE.md) and [Concurrency](concurrency.md) | Choose an executor, share one engine between sessions, write async providers and tools, and cancel a run. |
| [Runtime context and strict interposition](STRICT_RUNTIME_INTERPOSITION.md) | Make required context, Hooks and provider dispatch receipts mandatory instead of leaving them to the model. |
| [Migration guide](migration-v0.4-to-v1.0.md) | Move code written for the older node API (`execute` and its variants) to `run(NodeInput)`, and code written for `CompletionParams` and `complete` to typed provider requests. |
| [Valgrind and sanitizers](VALGRIND.md) | Run NeoGraph's memory and sanitizer checks. |

## Reference

| Read | If you want to |
|---|---|
| [C++ reference](reference-en.md) | Look up a type, a function or a failure code. It also holds the protocol failure mapping table for MCP, gRPC, A2A and ACP. |
| [Generated API reference](doxygen-mainpage.md) | Start from the Doxygen main page of the public headers in `include/neograph/`. |
| [ABI policy](ABI_POLICY.md) | Know when a NeoGraph upgrade needs a rebuild of your code. |
| [Schemas](../schemas) | Validate JSON that NeoGraph exchanges: Core definitions, Hooks, context artifacts and receipts. |
| [Changelog](../CHANGELOG.md) and [security policy](../SECURITY.md) | See what changed, or report a vulnerability. |

## Program contracts

These documents describe parts of the Program layer in detail. Read them when you extend the runtime or need the exact contract; you do not need them to use it.

| Document | Subject |
|---|---|
| [Program capability context](PROGRAM_CAPABILITY_CONTEXT.md) | How a node sees only the capabilities its admitted configuration declares. |
| [Recursive Harnesses](PROGRAM_RECURSIVE_HARNESSES.md) | A parent, child and grandchild agent tree, each with its own Program version and budget. |
| Child synthesis: [contract](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md), [persistence](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md), [recovery](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md) | How a Program asks the host to admit generated child source, and how that survives a restart. |
| [DSL capability evaluation](DSL_CAPABILITY_EVAL.md) | A test that measures whether a model can write valid Programs. |

## Measurements

| Document | Subject |
|---|---|
| [Performance deep dive](performance-deep-dive.md) and [benchmarks](../benchmarks/README.md) | Measured engine overhead and concurrency, with the configuration of each run. They are not guarantees. |
| [Program cost measurement](PROGRAM_COST_MEASUREMENT.md) | How Program bookkeeping and store costs are measured. |
