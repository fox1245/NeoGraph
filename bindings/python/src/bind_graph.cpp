// Engine surface: GraphEngine.compile(), .run(), .run_stream() with
// Python callback, .get_state(), .update_state(), .set_worker_count().
//
// Plus the async surface (commit 5):
//   .run_async(cfg)               -> awaitable[RunResult]
//   .run_stream_async(cfg, cb)    -> awaitable[RunResult]
//   .resume_async(thread_id, ...) -> awaitable[RunResult]
//
// Async bridge architecture:
//
//   Python side                C++ side
//   -----------                ---------
//   await engine.run_async(cfg)
//       │
//       ▼
//   asyncio.Future <───────── loop.call_soon_threadsafe(fut.set_result, …)
//       ▲                      ▲
//       │                      │ completion callback
//       │                      │ (runs on asio worker, GIL acquired)
//       │                      │
//       │                  ┌───┴────────────┐
//       │                  │ asio coroutine │
//       │                  │ engine.run_async(cfg)
//       │                  └───────▲────────┘
//       │                          │ asio::co_spawn
//       │                          │
//       │                  ┌───────┴───────────┐
//       │                  │ AsyncRuntime      │
//       │                  │ (singleton:       │
//       │                  │  io_context +     │
//       │                  │  worker thread)   │
//       │                  └───────────────────┘
//       │
//   loop.run_until_complete(...) ─── asyncio loop on user's Python thread
//
// Two threads per call: the user's asyncio thread (where the await
// happens) and the asio worker thread (where the engine coroutine
// runs). The bridge uses `loop.call_soon_threadsafe` to hop the
// completion back. GIL is acquired on the asio side before any
// Python ops, including freeing the captured Future / loop refs.

#include "json_bridge.h"

#include <neograph/graph/cancel.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/state.h>
#include <neograph/graph/types.h>

#ifdef NEOGRAPH_PYBIND_HAS_POSTGRES
#include <neograph/graph/postgres_checkpoint.h>
#endif

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>

#include <exception>
#include <memory>
#include <string>
#include <thread>

namespace py = pybind11;

namespace neograph::pybind {

namespace {

// Singleton asio runtime that drives all `run_async` calls. One
// io_context, one worker thread, work-guard prevents premature exit
// even when there are no coroutines in flight.
//
// Why one shared runtime instead of per-engine: keeps the binding
// simple, and the engine itself is thread-safe across distinct
// thread_ids. CPU-bound parallel fan-out within a run is still
// served by the engine's own thread_pool (set_worker_count), so
// the AsyncRuntime mostly just orchestrates I/O suspension points.
//
// At process exit: ~AsyncRuntime resets the work-guard, joins the
// worker thread. No Python ops in the destructor, so no shutdown
// crash analogous to NodeFactory's was.
class AsyncRuntime {
public:
    static AsyncRuntime& instance() {
        static AsyncRuntime r;
        return r;
    }

    asio::io_context::executor_type executor() {
        return io_.get_executor();
    }

private:
    AsyncRuntime()
        : guard_(asio::make_work_guard(io_)),
          worker_([this]() { io_.run(); }) {}

    ~AsyncRuntime() {
        guard_.reset();
        io_.stop();
        if (worker_.joinable()) worker_.join();
    }

    AsyncRuntime(const AsyncRuntime&) = delete;
    AsyncRuntime& operator=(const AsyncRuntime&) = delete;

    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> guard_;
    std::thread worker_;
};

// Hold a py::object in a shared_ptr with a GIL-acquiring deleter —
// same pattern as commit 1's run_stream callback wrapper. Lets us
// pass the asyncio Future/loop into asio's completion lambda where
// they may be copied/destroyed on the asio worker thread without
// the GIL (until the deleter is invoked, which acquires it).
std::shared_ptr<py::object> hold_py(py::object obj) {
    return std::shared_ptr<py::object>(
        new py::object(std::move(obj)),
        [](py::object* p) {
            py::gil_scoped_acquire g;
            delete p;
        });
}

// Drain whatever the asio coroutine produced (RunResult or exception)
// onto the captured asyncio.Future. Runs on the asio worker thread;
// hops back to the asyncio loop thread via call_soon_threadsafe so
// `Future.set_result` runs where asyncio expects.
//
// Two failure modes guarded:
//   1. Loop closed — is_closed() short-circuits before scheduling.
//      call_soon_threadsafe on a closed loop raises RuntimeError on
//      the worker thread; we'd lose the GIL guarantee, so eat early.
//   2. Future cancelled (or already resolved) — set_result/set_exception
//      raise InvalidStateError, but the raise happens later on the
//      asyncio loop thread (call_soon_threadsafe just schedules), so
//      no C++ try/catch can catch it. The asyncio default handler
//      logs the unhandled exception to stderr — visible as a flood of
//      "Exception in callback ... InvalidStateError: invalid state"
//      under any UI that cancels long streams (FastAPI SSE + frontend
//      AbortController is the textbook case).
//
//      Fix: route through `_safe_set_future_result` /
//      `_safe_set_future_exception` Python helpers (defined via
//      m.def in init_graph below), which `if not fut.done():` guard
//      before calling set_result/set_exception. The helpers run on
//      the loop thread under its GIL, exactly where the original
//      raise would have happened.
template <typename T>
void resolve_future_async(std::shared_ptr<py::object> future,
                          std::shared_ptr<py::object> loop,
                          std::exception_ptr eptr,
                          T&& result) {
    py::gil_scoped_acquire g;
    try {
        if (loop->attr("is_closed")().template cast<bool>()) return;

        py::module_ mod =
            py::module_::import("neograph_engine._neograph");

        if (eptr) {
            std::string msg;
            try {
                std::rethrow_exception(eptr);
            } catch (const std::exception& ex) {
                msg = ex.what();
            } catch (...) {
                msg = "neograph: unknown C++ exception in async run";
            }
            py::object exc =
                py::module_::import("builtins").attr("RuntimeError")(msg);
            loop->attr("call_soon_threadsafe")(
                mod.attr("_safe_set_future_exception"),
                *future, exc);
        } else {
            loop->attr("call_soon_threadsafe")(
                mod.attr("_safe_set_future_result"),
                *future, py::cast(std::forward<T>(result)));
        }
    } catch (const py::error_already_set&) {
        // Loop probably racing into closure — nothing we can do
        // without breaking a Python guarantee. Eat the error.
        PyErr_Clear();
    }
}

} // namespace

void init_graph(py::module_& m) {
    using namespace neograph::graph;

    // ── Async-bridge safe-resolve helpers ────────────────────────────────
    //
    // Hand these to `loop.call_soon_threadsafe` instead of raw
    // `future.set_result` / `future.set_exception`. They guard against
    // resolving a future that the user already cancelled — the textbook
    // case is a frontend AbortController firing every keystroke (300 ms
    // debounce) under FastAPI SSE: `request.is_disconnected()` cancels
    // the asyncio task, the C++ engine worker still finishes the run,
    // and the completion lambda races to set_result on a now-cancelled
    // future, raising InvalidStateError on the loop thread.
    //
    // Bound here (rather than in a `_async_bridge.py` module) so the
    // resolve path stays inside the `_neograph` extension — one fewer
    // import to keep in sync, and the helpers are available before any
    // user code can call run_async.
    m.def("_safe_set_future_result",
        [](py::object fut, py::object value) {
            if (!fut.attr("done")().cast<bool>()) {
                fut.attr("set_result")(value);
            }
        },
        py::arg("future"),
        py::arg("value"),
        "Internal: cancel-safe peer of Future.set_result. No-op when "
        "the future is already done/cancelled (raised "
        "InvalidStateError pre-fix).");

    m.def("_safe_set_future_exception",
        [](py::object fut, py::object exc) {
            if (!fut.attr("done")().cast<bool>()) {
                fut.attr("set_exception")(exc);
            }
        },
        py::arg("future"),
        py::arg("exception"),
        "Internal: cancel-safe peer of Future.set_exception. No-op "
        "when the future is already done/cancelled.");

    // ── RunConfig ────────────────────────────────────────────────────────
    py::class_<RunConfig>(m, "RunConfig",
        "Per-run configuration: thread_id, input dict, max_steps, "
        "stream_mode for run_stream(), and resume_if_exists for "
        "multi-turn chat semantics.")
        .def(py::init([](const std::string& thread_id,
                         py::object input,
                         int max_steps,
                         StreamMode stream_mode,
                         bool resume_if_exists) {
            RunConfig c;
            c.thread_id = thread_id;
            c.input = py_to_json(input);
            c.max_steps = max_steps;
            c.stream_mode = stream_mode;
            c.resume_if_exists = resume_if_exists;
            return c;
        }),
            py::arg("thread_id") = "",
            py::arg("input") = py::dict(),
            py::arg("max_steps") = 50,
            py::arg("stream_mode") = StreamMode::ALL,
            py::arg("resume_if_exists") = false)
        .def_readwrite("thread_id",   &RunConfig::thread_id)
        .def_property("input",
            [](const RunConfig& c) { return json_to_py(c.input); },
            [](RunConfig& c, py::object v) { c.input = py_to_json(v); })
        .def_readwrite("max_steps",        &RunConfig::max_steps)
        .def_readwrite("stream_mode",      &RunConfig::stream_mode)
        .def_readwrite("resume_if_exists", &RunConfig::resume_if_exists,
            "Opt-in: when True and a checkpoint store is configured, "
            "engine.run/run_async/run_stream loads the latest checkpoint "
            "for thread_id (if any) and applies input on top before "
            "running. Default False preserves the historical fresh-start "
            "semantics. For HITL resume from an interrupted run, use "
            "engine.resume() instead.");

    // ── RunResult ────────────────────────────────────────────────────────
    py::class_<RunResult>(m, "RunResult",
        "Run completion record: serialized final state, interrupt info, "
        "checkpoint id, and execution trace.")
        .def(py::init<>())
        .def_property_readonly("output",
            [](const RunResult& r) { return json_to_py(r.output); })
        .def_readonly("interrupted",     &RunResult::interrupted)
        .def_readonly("interrupt_node",  &RunResult::interrupt_node)
        .def_property_readonly("interrupt_value",
            [](const RunResult& r) { return json_to_py(r.interrupt_value); })
        .def_readonly("checkpoint_id",   &RunResult::checkpoint_id)
        .def_readonly("execution_trace", &RunResult::execution_trace);

    // ── CheckpointStore (abstract base) + InMemoryCheckpointStore ────────
    //
    // Required for engine.update_state() / engine.fork() to work — both
    // mutate the checkpoint store, and refuse to run when one isn't
    // configured. The in-memory store is the simplest option; SQLite /
    // Postgres backends are NeoGraph-side targets (binding deferred).
    py::class_<CheckpointStore, std::shared_ptr<CheckpointStore>>(m,
        "CheckpointStore",
        "Abstract checkpoint persistence interface. Construct a "
        "concrete subclass like InMemoryCheckpointStore and pass it "
        "to engine.set_checkpoint_store().");

    py::class_<InMemoryCheckpointStore, CheckpointStore,
               std::shared_ptr<InMemoryCheckpointStore>>(m,
        "InMemoryCheckpointStore",
        "Process-lifetime checkpoint store. State survives across "
        "runs sharing the same engine + thread_id, but is lost when "
        "the engine is destroyed. Pick PostgresCheckpointStore for "
        "durable, multi-process state.")
        .def(py::init<>());

#ifdef NEOGRAPH_PYBIND_HAS_POSTGRES
    // ── PostgresCheckpointStore (NEOGRAPH_BUILD_POSTGRES=ON) ──────────
    //
    // libpq-backed durable store, schema mirrors LangGraph's
    // PostgresSaver (with a `neograph_` table prefix to coexist).
    // Eagerly opens the connection pool in the constructor and runs
    // ensure_schema(); raises RuntimeError on bad credentials or DDL
    // failure rather than waiting until first save().
    //
    // Available only when the binding wheel was built with
    // NEOGRAPH_BUILD_POSTGRES=ON (default OFF in cibw config — the
    // shipped PyPI wheel doesn't include this; install from source
    // with -DNEOGRAPH_BUILD_POSTGRES=ON to use). Wheel inclusion
    // pending a libpq-bundling cibw setup.
    py::class_<PostgresCheckpointStore, CheckpointStore,
               std::shared_ptr<PostgresCheckpointStore>>(m,
        "PostgresCheckpointStore",
        "Durable, multi-process checkpoint store backed by PostgreSQL "
        "(libpq). Schema mirrors LangGraph's PostgresSaver under a "
        "`neograph_` prefix; the same database can host both. The "
        "constructor opens a fixed-size connection pool eagerly and "
        "runs DDL — credential or permission errors surface "
        "immediately rather than at first save().")
        .def(py::init<const std::string&, std::size_t>(),
             py::arg("conn_str"),
             py::arg("pool_size") = 8,
             "conn_str is any libpq connection URI "
             "(e.g. 'postgresql://user:pass@host:5432/dbname').\n"
             "pool_size controls how many libpq backends the store "
             "opens; default 8 matches a typical small worker pool. "
             "Set to 1 for embedded / single-thread use to save one "
             "PG backend per store.")
        .def_property_readonly("pool_size",
            &PostgresCheckpointStore::pool_size,
            "Number of connections in the pool (set at construction).")
        .def_property_readonly("reconnect_count",
            &PostgresCheckpointStore::reconnect_count,
            "Cumulative number of pool slots replaced after a broken-"
            "connection detection (PG restart, pgbouncer idle timeout, "
            "network blip). Useful as a Prometheus gauge.");
#endif

    // ── GraphEngine ──────────────────────────────────────────────────────
    //
    // Holder is shared_ptr so NodeContext sharing across engines stays
    // simple. compile() returns unique_ptr; we move into shared_ptr at
    // the binding boundary.
    py::class_<GraphEngine, std::shared_ptr<GraphEngine>>(m, "GraphEngine",
        "Compiled graph runtime. Construct via GraphEngine.compile(), "
        "then call run() or run_stream(). Single instance is safe to "
        "share across threads with distinct thread_ids.")
        .def_static("compile",
            [](py::object definition, py::object ctx_obj) {
                // Take the Python wrapper as py::object (rather than
                // const NodeContext&) so we can read the `_pytools`
                // dynamic attr carrying user-defined Python Tool
                // instances. The C++ NodeContext data still comes
                // through .cast<NodeContext>() — same struct, just
                // accessed via the Python side first.
                NodeContext ctx = ctx_obj.cast<NodeContext>();

                // Materialize the Python Tools as PyToolOwner-backed
                // unique_ptrs. After compile(), we transfer ownership
                // to the engine via own_tools() so the engine keeps
                // them alive for as long as it lives — and the raw
                // pointers we stash in ctx.tools below stay valid.
                std::vector<std::unique_ptr<neograph::Tool>> owned_tools;
                if (py::hasattr(ctx_obj, "_pytools")) {
                    owned_tools = wrap_python_tools(ctx_obj.attr("_pytools"));
                    ctx.tools.clear();
                    ctx.tools.reserve(owned_tools.size());
                    for (auto& up : owned_tools) {
                        ctx.tools.push_back(up.get());
                    }
                }

                auto j = py_to_json(definition);
                // GIL held: compile is fast (just walks the JSON).
                auto unique = GraphEngine::compile(j, ctx, nullptr);
                if (!owned_tools.empty()) {
                    unique->own_tools(std::move(owned_tools));
                }
                return std::shared_ptr<GraphEngine>(unique.release());
            },
            py::arg("definition"),
            py::arg("ctx"),
            "Compile a graph from a JSON-shaped Python dict. The "
            "context provides the LLM provider, tools, model, and "
            "system instructions used by built-in node types. Raises "
            "RuntimeError on bad shape.")

        .def("run",
            [](GraphEngine& self, const RunConfig& cfg) {
                // Release the GIL for the entire run. Built-in node
                // types do no Python callbacks; concurrent runs from
                // other Python threads stay live.
                py::gil_scoped_release release;
                return self.run(cfg);
            },
            py::arg("config"),
            "Run the graph synchronously to completion (or interrupt).")

        .def("run_stream",
            [](GraphEngine& self,
               const RunConfig& cfg,
               py::function py_cb) {
                // The engine internally copies the std::function<...>
                // (each fan-out worker can carry its own copy). If the
                // std::function holds a py::function directly, copying
                // it touches the CPython refcount — which is illegal
                // while the GIL is released. Wrap the callable in a
                // shared_ptr so copies are pure atomic shared_ptr
                // increments; the destructor explicitly acquires the
                // GIL before dropping the py::function.
                auto sp = std::shared_ptr<py::function>(
                    new py::function(std::move(py_cb)),
                    [](py::function* f) {
                        py::gil_scoped_acquire g;
                        delete f;
                    });
                auto cb = [sp](const GraphEvent& ev) {
                    py::gil_scoped_acquire acquire;
                    try {
                        (*sp)(ev);
                    } catch (const py::error_already_set&) {
                        // Don't let a Python callback crash an engine
                        // worker — print and swallow. The C++ callback
                        // contract is best-effort observability; we
                        // don't propagate to keep the run alive.
                        PyErr_Print();
                    }
                };
                py::gil_scoped_release release;
                return self.run_stream(cfg, cb);
            },
            py::arg("config"),
            py::arg("callback"),
            "Run the graph and invoke `callback(GraphEvent)` for every "
            "event admitted by config.stream_mode. Exceptions raised "
            "from the Python callback are printed to stderr and "
            "swallowed; they do not abort the run.")

        .def("get_state",
            [](const GraphEngine& self, const std::string& thread_id) -> py::object {
                auto opt = self.get_state(thread_id);
                if (!opt.has_value()) return py::none();
                return json_to_py(*opt);
            },
            py::arg("thread_id"),
            "Latest serialized state for thread_id, or None if no "
            "checkpoint has been written.\n\n"
            "**v0.4 soft-deprecated**: prefer "
            "``engine.get_state_view(thread_id)`` (PR 5 of "
            "ROADMAP_v1.md). The view exposes channels as flat "
            "attributes (``view.messages``) and supports typed "
            "Pydantic subclasses; this raw-dict accessor stays as the "
            "escape hatch for callers that need the nested "
            "``{'channels': {name: {value, version}}}`` shape "
            "verbatim. Slated for removal in v1.0 only if the "
            "deprecation is loud enough; otherwise stays available.")

        .def("update_state",
            [](GraphEngine& self,
               const std::string& thread_id,
               py::object channel_writes,
               const std::string& as_node) {
                // v0.3.2 (TODO_v0.3.md item #5): accept BOTH a
                // ``dict`` (channel_name → value) and a
                // ``list[ChannelWrite]`` form. Pre-fix only dict
                // worked — passing a list silently no-op'd because
                // the C++ engine's update_state checks
                // ``channel_writes.is_object()`` and skips otherwise.
                // The list form is symmetric with how every node
                // body builds writes, so it's the natural shape a
                // caller from a Python REPL or UI would try.
                json payload;
                if (py::isinstance<py::dict>(channel_writes)) {
                    payload = py_to_json(channel_writes);
                } else if (py::isinstance<py::list>(channel_writes) ||
                           py::isinstance<py::tuple>(channel_writes)) {
                    // Reduce list[ChannelWrite] → dict {channel: value}.
                    // If the same channel appears more than once, the
                    // last value wins — matches dict-literal semantics
                    // and keeps the engine's single-write-per-channel
                    // invariant intact. For multi-write per channel
                    // (e.g. appending two messages at once on an
                    // APPEND-reduced channel), bundle the values into
                    // a list yourself: {"messages": [m1, m2]}.
                    payload = json::object();
                    for (auto item : channel_writes) {
                        // Accept either an actual ChannelWrite instance
                        // or a duck-typed object exposing .channel /
                        // .value (so a SimpleNamespace works in tests).
                        if (!py::hasattr(item, "channel") ||
                            !py::hasattr(item, "value")) {
                            throw py::type_error(
                                "update_state: list items must be "
                                "ChannelWrite (or expose .channel / "
                                ".value attributes); got "
                                + py::str(item.attr("__class__")
                                              .attr("__name__"))
                                      .cast<std::string>());
                        }
                        std::string ch =
                            item.attr("channel").cast<std::string>();
                        payload[ch] = py_to_json(item.attr("value"));
                    }
                } else {
                    throw py::type_error(
                        "update_state: channel_writes must be a dict "
                        "{channel_name: value} or a list of ChannelWrite; "
                        "got " +
                        py::str(channel_writes.attr("__class__")
                                    .attr("__name__"))
                            .cast<std::string>());
                }
                self.update_state(thread_id, payload, as_node);
            },
            py::arg("thread_id"),
            py::arg("channel_writes"),
            py::arg("as_node") = "",
            "Apply channel writes to the latest checkpoint for "
            "thread_id and save a new checkpoint. Useful for injecting "
            "external state from a UI / REPL.\n\n"
            "``channel_writes`` accepts two equivalent shapes:\n"
            "  - ``dict``: ``{channel_name: value}`` — direct keyed form.\n"
            "  - ``list[ChannelWrite]``: ``[ChannelWrite('messages', [...])]``\n"
            "    — symmetric with the shape every node body emits.\n"
            "Duplicate channels in the list form are last-write-wins; "
            "for multi-write per channel (e.g. APPEND-reduced messages), "
            "bundle the values: ``{'messages': [m1, m2]}``. Other types "
            "raise TypeError so a silent no-op (the pre-v0.3.2 trap) "
            "can't reoccur.")

        .def("fork", &GraphEngine::fork,
            py::arg("source_thread_id"),
            py::arg("new_thread_id"),
            py::arg("checkpoint_id") = "",
            "Copy a checkpoint from source thread to a new thread, "
            "enabling branching execution paths.")

        .def("set_worker_count", &GraphEngine::set_worker_count,
            py::arg("n"),
            "Resize the worker pool used for parallel fan-out. "
            "compile() already sizes the pool to hardware_concurrency() "
            "so FANOUT > 1 workloads parallelize by default; call this "
            "only to override (e.g. set_worker_count(1) for nodes with "
            "non-thread-safe state, or a wider pool than core count). "
            "Must be called before any run(). Values < 1 clamp to 1.")

        .def("set_worker_count_auto", &GraphEngine::set_worker_count_auto,
            "Resize the worker pool back to hardware_concurrency() (the "
            "compile-time default). Useful after an explicit "
            "set_worker_count(N) override to return to the auto-sized "
            "pool. Must be called before any run().")

        .def("set_node_cache_enabled", &GraphEngine::set_node_cache_enabled,
            py::arg("node_name"), py::arg("enabled"),
            "Opt a node into result caching. The executor hashes the "
            "input state and replays a cached NodeResult on hit, "
            "skipping the node's execute() entirely. Off by default — "
            "only enable for pure nodes (deterministic, no side "
            "effects). Streaming runs (run_stream) bypass the cache "
            "for the affected node because cached hits cannot replay "
            "LLM_TOKEN events.")

        .def("clear_node_cache", &GraphEngine::clear_node_cache,
            "Drop all cached NodeResults. Per-node enable state is "
            "preserved. Useful between bench iterations or after "
            "external state changes the cached results would no "
            "longer reflect.")

        .def("node_cache_stats", [](const GraphEngine& self) {
            const auto& nc = self.node_cache();
            py::dict d;
            d["size"]   = nc.size();
            d["hits"]   = nc.hit_count();
            d["misses"] = nc.miss_count();
            return d;
        }, "Return a dict with current cache size, hit count, miss count.")

        .def("set_checkpoint_store", &GraphEngine::set_checkpoint_store,
            py::arg("store"),
            "Wire a CheckpointStore instance into the engine. Required "
            "before update_state() / fork(); without it those methods "
            "raise. Pass an InMemoryCheckpointStore() for ephemeral "
            "in-process state. Must be called before any run().")

        // ── Async surface (asyncio-compatible) ───────────────────────────
        //
        // Each method returns a freshly-created asyncio.Future bound
        // to the calling thread's running event loop. The C++
        // engine coroutine is spawned on the binding's AsyncRuntime
        // (a single io_context + worker thread shared across
        // engines); when the coroutine completes, its result is
        // hopped back to the asyncio loop via
        // `loop.call_soon_threadsafe(fut.set_result, …)`.
        //
        // Receiver type is std::shared_ptr<GraphEngine> rather than
        // GraphEngine& so we can capture the shared_ptr by value
        // into the asio completion lambda — keeps the engine alive
        // for the duration of the in-flight coroutine even if the
        // Python wrapper goes out of scope.
        //
        // Lifetime contract for coroutine parameters: run_async,
        // run_stream_async, and resume_async all take parameters by
        // const reference. Coroutine frames store the *reference*,
        // not a copy of the referent — when the binding lambda
        // returns, the local `cfg` (or `thread_id` / `rv`) is
        // destroyed and the coroutine's reference dangles. We
        // sidestep this by heap-copying inputs into a shared_ptr
        // that the completion lambda also captures, so the storage
        // outlives the coroutine.
        .def("run_async",
            [](std::shared_ptr<GraphEngine> self, RunConfig cfg) {
                py::object asyncio = py::module_::import("asyncio");
                py::object loop = asyncio.attr("get_running_loop")();
                py::object future = loop.attr("create_future")();

                auto fut_h  = hold_py(future);
                auto loop_h = hold_py(loop);

                // v0.3: ensure a cancel_token is always present on the
                // RunConfig the engine sees. If the user supplied one
                // we honour it; else allocate a fresh one so the asio
                // co_spawn slot wiring below has something to bind.
                if (!cfg.cancel_token) {
                    cfg.cancel_token = std::make_shared<CancelToken>();
                }
                auto cancel_tok = cfg.cancel_token;
                auto cfg_h  = std::make_shared<RunConfig>(std::move(cfg));

                // Wire asyncio.Future.cancel() → CancelToken.cancel().
                // add_done_callback fires once when the future
                // transitions to done, including the CANCELLED state.
                // The token then sets its atomic flag (engine super-
                // step polls it) AND emits its asio cancellation_signal
                // (propagates down through co_await chain to the
                // ConnPool::async_post socket op, killing the in-
                // flight LLM HTTP request — the v0.2.3 cost-leak fix).
                py::cpp_function on_done(
                    [cancel_tok](py::object fut) {
                        try {
                            if (fut.attr("cancelled")().cast<bool>()) {
                                cancel_tok->cancel();
                            }
                        } catch (const py::error_already_set&) {
                            PyErr_Clear();
                        }
                    });
                future.attr("add_done_callback")(on_done);

                // Bind the cancel slot at co_spawn so asio's per-
                // operation cancellation propagates through every
                // co_await down to socket I/O. Token's executor is
                // bound on first use inside the engine; the spawn
                // executor is the same AsyncRuntime singleton.
                cancel_tok->bind_executor(
                    AsyncRuntime::instance().executor());

                asio::co_spawn(
                    AsyncRuntime::instance().executor(),
                    self->run_async(*cfg_h),
                    asio::bind_cancellation_slot(
                        cancel_tok->slot(),
                        [self, cfg_h, fut_h, loop_h, cancel_tok]
                        (std::exception_ptr e, RunResult result) {
                            resolve_future_async(fut_h, loop_h, e,
                                                 std::move(result));
                        }));

                return future;
            },
            py::arg("config"),
            "Awaitable peer of run(). Use under an asyncio loop:\n\n"
            "    import asyncio\n"
            "    async def go():\n"
            "        result = await engine.run_async(cfg)\n"
            "    asyncio.run(go())\n"
            "\n"
            "Multiple concurrent run_async() calls overlap on the "
            "binding's internal asio worker; combine with "
            "asyncio.gather to fan out across thread_ids.\n\n"
            "v0.3+: cancelling the returned Future "
            "(`fut.cancel()`, asyncio task cancel, FastAPI "
            "request_disconnected) propagates through the engine "
            "super-step loop and into in-flight LLM HTTP requests "
            "via asio cancellation, so a cancelled run no longer "
            "burns LLM tokens until the upstream call finishes.")

        .def("run_stream_async",
            [](std::shared_ptr<GraphEngine> self,
               RunConfig cfg,
               py::function py_cb) {
                py::object asyncio = py::module_::import("asyncio");
                py::object loop = asyncio.attr("get_running_loop")();
                py::object future = loop.attr("create_future")();

                auto fut_h  = hold_py(future);
                auto loop_h = hold_py(loop);

                // v0.3 cancel propagation: same shape as run_async.
                if (!cfg.cancel_token) {
                    cfg.cancel_token = std::make_shared<CancelToken>();
                }
                auto cancel_tok = cfg.cancel_token;
                auto cfg_h  = std::make_shared<RunConfig>(std::move(cfg));

                py::cpp_function on_done(
                    [cancel_tok](py::object fut) {
                        try {
                            if (fut.attr("cancelled")().cast<bool>()) {
                                cancel_tok->cancel();
                            }
                        } catch (const py::error_already_set&) {
                            PyErr_Clear();
                        }
                    });
                future.attr("add_done_callback")(on_done);

                cancel_tok->bind_executor(
                    AsyncRuntime::instance().executor());

                // The user's py::function lives in a shared_ptr with
                // a GIL-acquiring deleter so std::function copies on
                // the asio side don't bump CPython refcounts under a
                // released GIL. (Same idiom as the run_stream binding
                // in commit 1.)
                auto cb_h = std::shared_ptr<py::function>(
                    new py::function(std::move(py_cb)),
                    [](py::function* p) {
                        py::gil_scoped_acquire g;
                        delete p;
                    });

                // Trampoline lambda hops events from the asio worker
                // thread onto the asyncio loop thread via
                // call_soon_threadsafe. ev is heap-copied + handed to
                // a Python-owned wrapper so the queued call sees a
                // stable object after the engine emit returns.
                auto cb_loop_h = loop_h;
                auto cb_lambda =
                    [cb_h, cb_loop_h](const GraphEvent& ev) {
                        py::gil_scoped_acquire g;
                        try {
                            if (cb_loop_h->attr("is_closed")()
                                    .cast<bool>()) return;
                            auto* ev_owned = new GraphEvent(ev);
                            py::object ev_py = py::cast(
                                ev_owned,
                                py::return_value_policy::take_ownership);
                            cb_loop_h->attr("call_soon_threadsafe")(
                                *cb_h, ev_py);
                        } catch (const py::error_already_set&) {
                            PyErr_Clear();
                        }
                    };

                // Heap-pin the GraphStreamCallback std::function. The
                // engine's run_stream_async takes `const
                // GraphStreamCallback&` and the coroutine frame stores
                // that reference; if we pass a stack-local
                // std::function, it dies when this lambda returns and
                // the coroutine reads dangling memory on the asio
                // worker — segfault deep inside the engine before the
                // node's Python execute() ever fires.
                //
                // (Caught the hard way: the engine's run_async path
                // sidesteps this because it constructs the empty
                // GraphStreamCallback at the call site itself.)
                auto engine_cb_h =
                    std::make_shared<GraphStreamCallback>(
                        std::move(cb_lambda));

                asio::co_spawn(
                    AsyncRuntime::instance().executor(),
                    self->run_stream_async(*cfg_h, *engine_cb_h),
                    asio::bind_cancellation_slot(
                        cancel_tok->slot(),
                        [self, cfg_h, engine_cb_h, fut_h, loop_h, cancel_tok]
                        (std::exception_ptr e, RunResult result) {
                            resolve_future_async(fut_h, loop_h, e,
                                                 std::move(result));
                        }));

                return future;
            },
            py::arg("config"),
            py::arg("callback"),
            "Awaitable peer of run_stream(). The callback is invoked "
            "for each GraphEvent — events are hopped onto the asyncio "
            "loop, so the callback runs on the user's Python thread, "
            "not on the engine worker.\n\n"
            "v0.3+: cancelling the returned Future propagates into "
            "in-flight LLM HTTP requests via asio cancellation, so "
            "a cancelled stream stops billable token generation.")

        .def("resume_async",
            [](std::shared_ptr<GraphEngine> self,
               std::string thread_id,
               py::object resume_value) {
                py::object asyncio = py::module_::import("asyncio");
                py::object loop = asyncio.attr("get_running_loop")();
                py::object future = loop.attr("create_future")();

                auto fut_h  = hold_py(future);
                auto loop_h = hold_py(loop);

                // Heap-pin both string and json — same coroutine-frame
                // dangling-reference concern as run_async.
                auto tid_h = std::make_shared<std::string>(std::move(thread_id));
                auto rv_h  = std::make_shared<json>(py_to_json(resume_value));

                asio::co_spawn(
                    AsyncRuntime::instance().executor(),
                    self->resume_async(*tid_h, *rv_h, nullptr),
                    [self, tid_h, rv_h, fut_h, loop_h]
                    (std::exception_ptr e, RunResult result) {
                        resolve_future_async(fut_h, loop_h, e,
                                             std::move(result));
                    });

                return future;
            },
            py::arg("thread_id"),
            py::arg("resume_value") = py::none(),
            "Awaitable peer of resume(). Restart an interrupted run "
            "from the most recent checkpoint, optionally injecting a "
            "value (typically the human's response in HITL flows).")

        .def_property_readonly("name", &GraphEngine::get_graph_name);

    // ── ReducerRegistry / ConditionRegistry — Python registration hooks ──
    //
    // Both are C++ process-lifetime singletons. Same Py_DECREF-after-
    // Py_Finalize hazard as NodeFactory (see bind_node.cpp), so we
    // mirror the pattern: stash the Python callable in a module-level
    // dict (Python-owned lifetime) and have the C++ closure capture
    // only the name string. Lookup happens at call time, under GIL.
    m.attr("_python_reducers")   = py::dict();
    m.attr("_python_conditions") = py::dict();

    m.def("_register_python_reducer_internal",
        [m](const std::string& name) {
            ReducerRegistry::instance().register_reducer(name,
                [name](const json& current,
                       const json& incoming) -> json {
                    py::gil_scoped_acquire g;
                    py::module_ mod =
                        py::module_::import("neograph_engine._neograph");
                    py::dict registry =
                        mod.attr("_python_reducers").cast<py::dict>();
                    if (!registry.contains(name.c_str())) {
                        throw std::runtime_error(
                            "neograph: reducer '" + name +
                            "' is registered with the C++ ReducerRegistry "
                            "but missing from the Python reducer "
                            "registry — did you call register_reducer "
                            "from a different interpreter?");
                    }
                    py::function fn =
                        registry[name.c_str()].cast<py::function>();
                    py::object out = fn(json_to_py(current),
                                        json_to_py(incoming));
                    return py_to_json(out);
                });
        },
        py::arg("name"),
        "Internal: wires the C++ ReducerRegistry slot for `name` to "
        "the Python callable in `_python_reducers[name]`. Called by "
        "ReducerRegistry.register_reducer.");

    m.def("_register_python_condition_internal",
        [m](const std::string& name) {
            ConditionRegistry::instance().register_condition(name,
                [name](const GraphState& state) -> std::string {
                    py::gil_scoped_acquire g;
                    py::module_ mod =
                        py::module_::import("neograph_engine._neograph");
                    py::dict registry =
                        mod.attr("_python_conditions").cast<py::dict>();
                    if (!registry.contains(name.c_str())) {
                        throw std::runtime_error(
                            "neograph: condition '" + name +
                            "' is registered with the C++ "
                            "ConditionRegistry but missing from the "
                            "Python condition registry — did you call "
                            "register_condition from a different "
                            "interpreter?");
                    }
                    py::function fn =
                        registry[name.c_str()].cast<py::function>();
                    // The Python callable receives the GraphState
                    // wrapper (already bound in bind_state.cpp), not
                    // a serialised dict — gives the user state.get(),
                    // state.get_messages() &c. just like @ng.node.
                    py::object out = fn(py::cast(&state,
                        py::return_value_policy::reference));
                    return out.cast<std::string>();
                });
        },
        py::arg("name"),
        "Internal: wires the C++ ConditionRegistry slot for `name` to "
        "the Python callable in `_python_conditions[name]`. Called by "
        "ConditionRegistry.register_condition.");

    py::class_<ReducerRegistry>(m, "ReducerRegistry",
        "Singleton registry mapping reducer names to merge functions. "
        "Built-in reducers: \"overwrite\", \"append\". Use "
        "register_reducer() to add custom Python reducers; the "
        "callable is invoked as `fn(current, incoming) -> merged` "
        "where current/incoming are Python objects (dict / list / "
        "scalar) decoded from the channel JSON.")
        .def_static("register_reducer",
            [m](const std::string& name, py::function py_reducer) {
                py::dict registry =
                    m.attr("_python_reducers").cast<py::dict>();
                registry[name.c_str()] = py_reducer;
                m.attr("_register_python_reducer_internal")(name);
            },
            py::arg("name"),
            py::arg("reducer"),
            "Register a Python callable as a reducer. The callable is "
            "invoked as `reducer(current, incoming)` — both are decoded "
            "from JSON into Python objects — and must return a JSON-"
            "serialisable result. Re-registering an existing name "
            "replaces the previous reducer.\n\n"
            "Example::\n\n"
            "    def sum_reducer(current, incoming):\n"
            "        return (current or 0) + incoming\n\n"
            "    ng.ReducerRegistry.register_reducer(\"sum\", sum_reducer)\n\n"
            "    # Now `\"reducer\": \"sum\"` works in your channels.")
        .def_static("instance",
            []() { return std::ref(ReducerRegistry::instance()); },
            py::return_value_policy::reference,
            "Return the singleton ReducerRegistry instance.");

    py::class_<ConditionRegistry>(m, "ConditionRegistry",
        "Singleton registry mapping conditional-edge condition names "
        "to predicate functions. Built-in conditions: "
        "\"has_tool_calls\", \"route_channel\". Use "
        "register_condition() to add custom Python conditions; the "
        "callable is invoked as `fn(state) -> str` and must return "
        "one of the keys in the conditional edge's `routes` map.")
        .def_static("register_condition",
            [m](const std::string& name, py::function py_condition) {
                py::dict registry =
                    m.attr("_python_conditions").cast<py::dict>();
                registry[name.c_str()] = py_condition;
                m.attr("_register_python_condition_internal")(name);
            },
            py::arg("name"),
            py::arg("condition"),
            "Register a Python callable as a condition. The callable "
            "is invoked as `condition(state)` where state is a "
            "GraphState wrapper (use state.get(channel) / "
            "state.get_messages() to inspect). Must return a string "
            "key matching one of the routes in the conditional edge.\n\n"
            "Example::\n\n"
            "    def is_long(state):\n"
            "        msgs = state.get(\"messages\") or []\n"
            "        return \"long\" if len(msgs) > 10 else \"short\"\n\n"
            "    ng.ConditionRegistry.register_condition(\"is_long\", is_long)")
        .def_static("instance",
            []() { return std::ref(ConditionRegistry::instance()); },
            py::return_value_policy::reference,
            "Return the singleton ConditionRegistry instance.");
}

} // namespace neograph::pybind
