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

#include <neograph/graph/checkpoint.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/types.h>

#ifdef NEOGRAPH_PYBIND_HAS_POSTGRES
#include <neograph/graph/postgres_checkpoint.h>
#endif

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
// If the asyncio loop has been closed (rare — happens if the user
// cancels everything and tears down the loop while a run is mid-
// flight), silently drop the result; trying to schedule on a closed
// loop raises RuntimeError.
template <typename T>
void resolve_future_async(std::shared_ptr<py::object> future,
                          std::shared_ptr<py::object> loop,
                          std::exception_ptr eptr,
                          T&& result) {
    py::gil_scoped_acquire g;
    try {
        // is_closed() short-circuit is the only safe way to detect a
        // dead loop without raising — call_soon_threadsafe on a
        // closed loop throws RuntimeError.
        if (loop->attr("is_closed")().template cast<bool>()) return;

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
                future->attr("set_exception"), exc);
        } else {
            loop->attr("call_soon_threadsafe")(
                future->attr("set_result"),
                py::cast(std::forward<T>(result)));
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

    // ── RunConfig ────────────────────────────────────────────────────────
    py::class_<RunConfig>(m, "RunConfig",
        "Per-run configuration: thread_id, input dict, max_steps, "
        "and stream_mode for run_stream().")
        .def(py::init([](const std::string& thread_id,
                         py::object input,
                         int max_steps,
                         StreamMode stream_mode) {
            RunConfig c;
            c.thread_id = thread_id;
            c.input = py_to_json(input);
            c.max_steps = max_steps;
            c.stream_mode = stream_mode;
            return c;
        }),
            py::arg("thread_id") = "",
            py::arg("input") = py::dict(),
            py::arg("max_steps") = 50,
            py::arg("stream_mode") = StreamMode::ALL)
        .def_readwrite("thread_id",   &RunConfig::thread_id)
        .def_property("input",
            [](const RunConfig& c) { return json_to_py(c.input); },
            [](RunConfig& c, py::object v) { c.input = py_to_json(v); })
        .def_readwrite("max_steps",   &RunConfig::max_steps)
        .def_readwrite("stream_mode", &RunConfig::stream_mode);

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
            "checkpoint has been written.")

        .def("update_state",
            [](GraphEngine& self,
               const std::string& thread_id,
               py::object channel_writes,
               const std::string& as_node) {
                self.update_state(thread_id, py_to_json(channel_writes), as_node);
            },
            py::arg("thread_id"),
            py::arg("channel_writes"),
            py::arg("as_node") = "",
            "Apply channel writes to the latest checkpoint for "
            "thread_id and save a new checkpoint. Useful for "
            "injecting external state from a UI.")

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
                auto cfg_h  = std::make_shared<RunConfig>(std::move(cfg));

                asio::co_spawn(
                    AsyncRuntime::instance().executor(),
                    self->run_async(*cfg_h),
                    [self, cfg_h, fut_h, loop_h]
                    (std::exception_ptr e, RunResult result) {
                        resolve_future_async(fut_h, loop_h, e,
                                             std::move(result));
                    });

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
            "asyncio.gather to fan out across thread_ids.")

        .def("run_stream_async",
            [](std::shared_ptr<GraphEngine> self,
               RunConfig cfg,
               py::function py_cb) {
                py::object asyncio = py::module_::import("asyncio");
                py::object loop = asyncio.attr("get_running_loop")();
                py::object future = loop.attr("create_future")();

                auto fut_h  = hold_py(future);
                auto loop_h = hold_py(loop);
                auto cfg_h  = std::make_shared<RunConfig>(std::move(cfg));

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
                    [self, cfg_h, engine_cb_h, fut_h, loop_h]
                    (std::exception_ptr e, RunResult result) {
                        resolve_future_async(fut_h, loop_h, e,
                                             std::move(result));
                    });

                return future;
            },
            py::arg("config"),
            py::arg("callback"),
            "Awaitable peer of run_stream(). The callback is invoked "
            "for each GraphEvent — events are hopped onto the asyncio "
            "loop, so the callback runs on the user's Python thread, "
            "not on the engine worker.")

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
}

} // namespace neograph::pybind
