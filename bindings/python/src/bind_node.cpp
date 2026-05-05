// Custom Python graph nodes — commit 2 of the pybind11 plan.
//
// Pattern: a thin C++ wrapper class (PyGraphNodeOwner) holds a
// reference to the Python user's node instance and forwards every
// virtual call across the GIL boundary. Python users subclass the
// pure-Python `neograph.GraphNode` base (see neograph/__init__.py) —
// no pybind11 trampoline holder, because the engine consumes nodes
// as `unique_ptr<GraphNode>` and pybind11's trampoline holders don't
// compose cleanly with that.
//
// Engine entry points (graph_executor.cpp:140-143):
//
//   execute_full_async(state)         ← every async run
//   execute_full_stream_async(s, cb)  ← every streaming run
//
// Both are coroutines. We override them to `co_return` the sync
// counterparts so a Python user only has to provide `execute()` (or
// `execute_full()`) without thinking about coroutines.
//
// GIL contract:
//   - Every Python attribute access acquires the GIL via
//     `py::gil_scoped_acquire`. Pybind11's GIL guard is re-entrant,
//     so nested acquires (e.g. when Python calls back into C++ which
//     calls Python again) are safe.
//   - The destructor explicitly acquires the GIL before dropping the
//     held `py::object` — engine cleanup may run on whichever thread
//     happens to drop the unique_ptr, not necessarily the Python
//     main thread.
//   - The factory callable is wrapped in a shared_ptr with a
//     GIL-acquiring deleter (same pattern as the run_stream callback
//     in commit 1) so concurrent compile()s don't race the deleter.

#include "json_bridge.h"

#include <neograph/api.h>  // NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>
#include <neograph/graph/types.h>

#include <asio/awaitable.hpp>
#include <asio/use_awaitable.hpp>

#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>

#include <memory>
#include <string>
#include <utility>

namespace py = pybind11;

namespace neograph::pybind {

using neograph::graph::CancelToken;
using neograph::graph::ChannelWrite;
using neograph::graph::Command;
using neograph::graph::GraphEvent;
using neograph::graph::GraphNode;
using neograph::graph::GraphState;
using neograph::graph::GraphStreamCallback;
using neograph::graph::NodeContext;
using neograph::graph::NodeFactory;
using neograph::graph::NodeInput;
using neograph::graph::NodeResult;
using neograph::graph::RunContext;
using neograph::graph::Send;

namespace {

// Coerce an arbitrary Python return value into a NodeResult.
//
// Accepts:
//   - a list of ChannelWrite           → NodeResult.writes
//   - an existing NodeResult instance  → returned as-is
//   - None                             → empty NodeResult
//   - a Command                        → NodeResult.command
//   - a list mixing ChannelWrite + Send + Command (any order) →
//     dispatched per type
//
// Anything else throws a TypeError. The aim is to let users write
// the natural shape (`return [...]`) without forcing them to wrap
// a single ChannelWrite in a NodeResult.
NodeResult coerce_to_node_result(py::handle obj) {
    if (obj.is_none()) return NodeResult{};

    // Direct NodeResult.
    if (py::isinstance<NodeResult>(obj)) {
        return obj.cast<NodeResult>();
    }
    // Single Command.
    if (py::isinstance<Command>(obj)) {
        NodeResult r;
        r.command = obj.cast<Command>();
        return r;
    }
    // Single Send.
    if (py::isinstance<Send>(obj)) {
        NodeResult r;
        r.sends.push_back(obj.cast<Send>());
        return r;
    }
    // Single ChannelWrite.
    if (py::isinstance<ChannelWrite>(obj)) {
        NodeResult r;
        r.writes.push_back(obj.cast<ChannelWrite>());
        return r;
    }
    // Iterable (list/tuple/generator) of mixed items.
    if (py::isinstance<py::sequence>(obj) ||
        py::isinstance<py::iterable>(obj)) {
        NodeResult r;
        for (auto item : obj) {
            if (py::isinstance<ChannelWrite>(item)) {
                r.writes.push_back(item.cast<ChannelWrite>());
            } else if (py::isinstance<Send>(item)) {
                r.sends.push_back(item.cast<Send>());
            } else if (py::isinstance<Command>(item)) {
                if (r.command.has_value()) {
                    throw py::type_error(
                        "execute_full() returned multiple Command "
                        "instances; only one is allowed per node.");
                }
                r.command = item.cast<Command>();
            } else {
                throw py::type_error(
                    "execute_full() result list must contain "
                    "ChannelWrite / Send / Command instances; got " +
                    std::string(py::str(item.get_type()).cast<std::string>()));
            }
        }
        return r;
    }

    throw py::type_error(
        "execute() / execute_full() must return a list of "
        "ChannelWrite, a NodeResult, a Command, a Send, or None; got " +
        std::string(py::str(obj.get_type()).cast<std::string>()));
}

// PyToolOwner: bridges the C++ Tool interface to a held Python user
// object. Created at GraphEngine.compile() time from the
// `_pytools` attribute on the Python NodeContext wrapper, then
// transferred to the engine via own_tools(). Pattern parallels
// PyGraphNodeOwner — same GIL handshake, same destructor caveat.
class PyToolOwner final : public neograph::Tool {
public:
    explicit PyToolOwner(py::object py_obj)
        : py_obj_(std::move(py_obj)) {}

    ~PyToolOwner() override {
        py::gil_scoped_acquire g;
        py_obj_ = py::object();
    }

    neograph::ChatTool get_definition() const override {
        py::gil_scoped_acquire g;
        return py_obj_.attr("get_definition")()
            .cast<neograph::ChatTool>();
    }

    std::string execute(const json& arguments) override {
        py::gil_scoped_acquire g;
        py::object args_py = json_to_py(arguments);
        return py_obj_.attr("execute")(args_py)
            .cast<std::string>();
    }

    std::string get_name() const override {
        py::gil_scoped_acquire g;
        return py_obj_.attr("get_name")().cast<std::string>();
    }

private:
    py::object py_obj_;
};

// PyGraphNodeOwner: bridges the C++ GraphNode interface to a held
// Python user object. One instance per node-in-the-graph; created
// by NodeFactory.register_type() factory callbacks.
class PyGraphNodeOwner final : public GraphNode {
public:
    explicit PyGraphNodeOwner(py::object py_obj)
        : py_obj_(std::move(py_obj)) {}

    ~PyGraphNodeOwner() override {
        // Engine destruction may run on a non-Python thread (e.g.
        // a thread_pool worker). The py::object holds a Python
        // refcount; dropping it MUST happen with the GIL held.
        py::gil_scoped_acquire g;
        py_obj_ = py::object();
    }

    // ── Required: name lookup ────────────────────────────────────────
    std::string get_name() const override {
        py::gil_scoped_acquire g;
        return py_obj_.attr("get_name")().cast<std::string>();
    }

    // ── Sync execute (legacy entrypoint, wrapped by execute_full) ────
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        py::gil_scoped_acquire g;
        py::object res = py_obj_.attr("execute")(
            py::cast(&state, py::return_value_policy::reference));
        if (res.is_none()) return {};
        return res.cast<std::vector<ChannelWrite>>();
    }

    // ── execute_full: where Command/Send live ────────────────────────
    NodeResult execute_full(const GraphState& state) override {
        py::gil_scoped_acquire g;
        // Prefer execute_full when the user defines it (so they can
        // emit Command/Send). Otherwise fall back to wrapping
        // execute()'s writes.
        if (has_user_method("execute_full")) {
            py::object res = py_obj_.attr("execute_full")(
                py::cast(&state, py::return_value_policy::reference));
            return coerce_to_node_result(res);
        }
        py::object res = py_obj_.attr("execute")(
            py::cast(&state, py::return_value_policy::reference));
        return coerce_to_node_result(res);
    }

    // ── v0.4 PR 7: unified run() entry, with Python-override priority
    //
    // The engine calls ``node->run(in)`` first (PR 2). PyGraphNodeOwner
    // overrides ``run`` so that:
    //
    //   1. If the Python user's class has a ``run`` method (defined
    //      strictly on a subclass of ``neograph_engine.GraphNode``),
    //      call it with a Python-side ``NodeInput`` object exposing
    //      ``state`` / ``ctx`` / ``stream_cb``. The user's body
    //      receives ``input.ctx.cancel_token`` directly — no need to
    //      rely on the smuggling thread-local. Coerce the return value
    //      via ``coerce_to_node_result``.
    //
    //   2. Otherwise, fall through to the legacy chain by forwarding
    //      to our own ``execute_full_async`` / ``execute_full_stream_async``
    //      override. Legacy Python users (those still overriding
    //      ``execute`` / ``execute_full`` etc.) keep working unchanged
    //      through the deprecation window — their ``provider.complete``
    //      sync calls still pick up the cancel token via
    //      ``current_cancel_token()`` thread-local installed in those
    //      legacy override bodies.
    //
    // PR 9 (v1.0) deletes the legacy chain entirely and run() becomes
    // the only path.
    asio::awaitable<NodeResult> run(NodeInput in) override {
        bool has_run = false;
        {
            py::gil_scoped_acquire g;
            has_run = has_user_method("run");
        }
        if (has_run) {
            py::gil_scoped_acquire g;
            // Cast NodeInput by value so the Python wrapper survives
            // even if the C++ frame moves on (NodeInput holds refs
            // into the engine's super-step locals which stay valid
            // through this co_await — but the wrapper itself can be
            // freed after the call returns).
            py::object py_input = py::cast(&in,
                py::return_value_policy::reference);
            py::object res = py_obj_.attr("run")(py_input);
            co_return coerce_to_node_result(res);
        }
        // Legacy fallback. NEOGRAPH_PUSH_IGNORE_DEPRECATED so the
        // legacy override calls don't trip our own deprecation
        // warnings — the user's deprecation visibility comes from
        // their override site, not from this internal forwarder.
        NEOGRAPH_PUSH_IGNORE_DEPRECATED
        if (in.stream_cb) {
            co_return co_await execute_full_stream_async(
                in.state, *in.stream_cb);
        }
        co_return co_await execute_full_async(in.state);
        NEOGRAPH_POP_IGNORE_DEPRECATED
    }

    // ── execute_full_async: the real engine entry point ──────────────
    //
    // The default GraphNode::execute_full_async chains through
    // execute_async → execute, dropping any Command/Send the user's
    // sync execute_full might emit. Override here to bridge straight
    // to execute_full (which dispatches to the Python user code).
    //
    // v0.3: install the run's CancelToken (carried on the GraphState)
    // as the thread-local ``current_cancel_token()`` so a sync Python
    // ``execute()`` calling ``ctx.provider.complete(params)`` picks
    // it up — Provider::complete reads the same thread-local and
    // binds it to its inner run_sync io_context, propagating cancel
    // into the LLM HTTP socket. The scope is set/reset around the
    // synchronous execute_full call only (no co_await spans), so
    // thread-local semantics stay safe across asio coroutine yields.
    asio::awaitable<NodeResult>
    execute_full_async(const GraphState& state) override {
        // Run the Python call directly in the coroutine — Python
        // execution is GIL-serialized regardless of executor, so
        // there's nothing to gain from co_awaiting on a different
        // executor here.
        neograph::graph::CurrentCancelTokenScope scope(
            state.run_cancel_token());
        co_return execute_full(state);
    }

    // ── Streaming variants (LLM_TOKEN events) ────────────────────────
    std::vector<ChannelWrite> execute_stream(
        const GraphState& state,
        const GraphStreamCallback& cb) override {
        py::gil_scoped_acquire g;
        if (has_user_method("execute_stream")) {
            // Wrap the C++ callback as a Python callable so the user
            // can opt into streaming.
            py::cpp_function py_cb([cb](const neograph::graph::GraphEvent& ev) {
                cb(ev);
            });
            py::object res = py_obj_.attr("execute_stream")(
                py::cast(&state, py::return_value_policy::reference),
                py_cb);
            if (res.is_none()) return {};
            return res.cast<std::vector<ChannelWrite>>();
        }
        // No streaming override → fall back to plain execute, ignore
        // the callback. Matches the C++ default.
        py::object res = py_obj_.attr("execute")(
            py::cast(&state, py::return_value_policy::reference));
        if (res.is_none()) return {};
        return res.cast<std::vector<ChannelWrite>>();
    }

    NodeResult execute_full_stream(
        const GraphState& state,
        const GraphStreamCallback& cb) override {
        py::gil_scoped_acquire g;
        if (has_user_method("execute_full_stream")) {
            py::cpp_function py_cb([cb](const neograph::graph::GraphEvent& ev) {
                cb(ev);
            });
            py::object res = py_obj_.attr("execute_full_stream")(
                py::cast(&state, py::return_value_policy::reference),
                py_cb);
            return coerce_to_node_result(res);
        }
        // v0.3.2 (TODO_v0.3.md item #10): a Python node that only
        // overrides ``execute_stream`` (no ``execute_full_stream``,
        // no ``execute`` / ``execute_full``) used to silently fall
        // through to the line below, hit the NotImplementedError in
        // GraphNode.execute, and never get its streaming variant
        // dispatched. The v0.3.1 hint message correctly pointed at
        // ``run_stream``, but ``run_stream`` itself still didn't
        // wire up. Now it does: prefer the user's ``execute_stream``
        // when no fuller override is present, wrap its writes into
        // a NodeResult, and the cb propagates so LLM_TOKEN events
        // flow through.
        if (has_user_method("execute_stream")) {
            py::cpp_function py_cb([cb](const neograph::graph::GraphEvent& ev) {
                cb(ev);
            });
            py::object res = py_obj_.attr("execute_stream")(
                py::cast(&state, py::return_value_policy::reference),
                py_cb);
            if (res.is_none()) return NodeResult{};
            return NodeResult{res.cast<std::vector<ChannelWrite>>()};
        }
        // No streaming override → execute_full() with cb dropped.
        // Same observable behaviour as the default GraphNode chain
        // but routes through the Python user's execute_full /
        // execute (so Command/Send still propagate).
        return execute_full(state);
    }

    asio::awaitable<NodeResult>
    execute_full_stream_async(
        const GraphState& state,
        const GraphStreamCallback& cb) override {
        // Same v0.3 cancel propagation as execute_full_async — the
        // streaming variant gets it too, otherwise long token streams
        // would leak cost when cancelled.
        neograph::graph::CurrentCancelTokenScope scope(
            state.run_cancel_token());
        co_return execute_full_stream(state, cb);
    }

private:
    bool has_user_method(const char* name) const {
        // Walk the actual __class__ MRO rather than just calling
        // hasattr — the Python GraphNode base supplies stubs that
        // hasattr would always see, masking whether the *subclass*
        // overrode the method. We're looking for a method that's
        // defined on a class strictly derived from `neograph.GraphNode`
        // (i.e. user code, not the base).
        if (!py::hasattr(py_obj_, name)) return false;
        py::object cls = py_obj_.attr("__class__");
        py::object base_module;
        try {
            base_module = py::module_::import("neograph_engine");
        } catch (const py::error_already_set&) {
            // Should never happen — if neograph isn't importable we
            // wouldn't be here. Conservative fallback: hasattr.
            return true;
        }
        py::object base = base_module.attr("GraphNode");
        // Iterate the MRO; the first class that defines `name` in its
        // own __dict__ wins. If that class IS the base, the user did
        // not override.
        py::tuple mro = cls.attr("__mro__");
        for (auto klass : mro) {
            py::object dict = klass.attr("__dict__");
            if (py::cast<py::dict>(dict).contains(name)) {
                return !klass.is(base);
            }
        }
        return false;
    }

    py::object py_obj_;
};

// Wrap an arbitrary Python callable in a shared_ptr with a
// GIL-acquiring deleter, so std::function copies (registry storage,
// engine internal hand-offs) don't touch the CPython refcount.
// Same pattern as run_stream's callback wrapper in commit 1.
template <typename PyCallable>
std::shared_ptr<PyCallable> make_gil_safe(PyCallable&& fn) {
    return std::shared_ptr<PyCallable>(
        new PyCallable(std::forward<PyCallable>(fn)),
        [](PyCallable* p) {
            py::gil_scoped_acquire g;
            delete p;
        });
}

} // namespace

// Public helper exposed via json_bridge.h. Wraps each item in the
// list as a unique_ptr<Tool> backed by a PyToolOwner trampoline.
// Caller (GraphEngine.compile binding) is responsible for transferring
// ownership to the engine via own_tools().
std::vector<std::unique_ptr<neograph::Tool>>
wrap_python_tools(py::handle tools_list) {
    std::vector<std::unique_ptr<neograph::Tool>> out;
    if (tools_list.is_none()) return out;
    for (auto t : tools_list) {
        out.push_back(std::make_unique<PyToolOwner>(
            py::reinterpret_borrow<py::object>(t)));
    }
    return out;
}

void init_node(py::module_& m) {
    using namespace neograph::graph;

    // ── GraphState (read-only view, no constructor exposed) ─────────────
    //
    // GraphState owns a shared_mutex (non-copyable, non-movable). We
    // never construct one from Python — they're handed in by the
    // engine via py::cast(&state, reference). The bound surface is
    // intentionally read-only: nodes communicate state changes by
    // RETURNING ChannelWrite objects, not by mutating GraphState
    // directly. Mutating helpers (`init_channel`, `write`,
    // `apply_writes`) are deliberately omitted.
    py::class_<GraphState>(m, "GraphState",
        "Read-only view of graph state during node execution. "
        "Nodes communicate state changes by returning ChannelWrite "
        "objects — do not look for setters here.")
        .def("get",
            [](const GraphState& s, const std::string& channel) {
                return json_to_py(s.get(channel));
            },
            py::arg("channel"),
            "Read a channel's current value. Returns None for missing "
            "channels.")
        .def("get_messages", &GraphState::get_messages,
            "Convenience: parse the 'messages' channel as a list of "
            "ChatMessage objects.")
        .def("serialize",
            [](const GraphState& s) { return json_to_py(s.serialize()); },
            "Whole-state JSON snapshot (channels + global version). "
            "Mostly useful for debugging custom nodes.")
        .def("channel_names", &GraphState::channel_names,
            "List of all channel names declared by the graph "
            "definition.")
        .def("channel_version", &GraphState::channel_version,
            py::arg("channel"),
            "Per-channel version counter (incremented on each write).")
        .def("global_version", &GraphState::global_version,
            "Sum of all channel versions; useful for detecting any "
            "change since a given snapshot.");

    // ── NodeResult (Command + Send + writes union) ──────────────────────
    py::class_<NodeResult>(m, "NodeResult",
        "Full node return shape: writes + optional Command (routing "
        "override) + optional Sends (dynamic fan-out). Construct one "
        "explicitly when emitting Command/Send; for writes-only nodes, "
        "just return a list of ChannelWrite from execute().")
        .def(py::init([](std::vector<ChannelWrite> writes,
                         py::object command,
                         std::vector<Send> sends) {
            NodeResult r;
            r.writes = std::move(writes);
            if (!command.is_none()) {
                r.command = command.cast<Command>();
            }
            r.sends = std::move(sends);
            return r;
        }),
            py::arg("writes") = std::vector<ChannelWrite>{},
            py::arg("command") = py::none(),
            py::arg("sends") = std::vector<Send>{})
        .def_readwrite("writes", &NodeResult::writes)
        .def_property("command",
            [](const NodeResult& r) -> py::object {
                if (r.command.has_value()) return py::cast(*r.command);
                return py::none();
            },
            [](NodeResult& r, py::object v) {
                if (v.is_none()) {
                    r.command.reset();
                } else {
                    r.command = v.cast<Command>();
                }
            })
        .def_readwrite("sends", &NodeResult::sends);

    // ── CancelToken (v0.4 PR 7: exposed so Python users can read /
    // cancel through ``input.ctx.cancel_token``) ────────────────────────
    //
    // Construction from Python is intentionally not exposed — the engine
    // owns lifecycle. ``RunConfig.cancel_token`` already accepts a
    // shared_ptr<CancelToken> via the existing wiring; this just makes
    // the *read* side usable from a node body.
    py::class_<CancelToken, std::shared_ptr<CancelToken>>(m, "CancelToken",
        "Cooperative cancel handle for an in-flight run. Engine "
        "constructs these; nodes read ``input.ctx.cancel_token`` and "
        "either pass it to ``provider.complete(params)`` (so an LLM "
        "HTTP socket aborts on cancel) or poll ``is_cancelled()`` for "
        "their own loops.")
        .def("is_cancelled", &CancelToken::is_cancelled,
            "Lock-free polling read of the cancel flag.")
        .def("cancel", &CancelToken::cancel,
            "Request cancellation. Thread-safe, idempotent.");

    // ── RunContext (per-run dispatch metadata, v0.4 PR 7) ───────────────
    //
    // Exposed read-only — the engine constructs and owns the RunContext;
    // Python users read it from ``NodeInput.ctx`` inside their ``run()``
    // override. Currently exposes the four fields a user is most likely
    // to consume: cancel_token (so they can pass it explicitly to
    // provider.complete instead of relying on the smuggling thread-local),
    // step (super-step counter), thread_id (RunConfig.thread_id), and
    // stream_mode. ``deadline`` and ``trace_id`` are reserved for
    // future RunConfig fields and stay default-constructed for now.
    py::class_<RunContext>(m, "RunContext",
        "Per-run dispatch metadata threaded by the engine. New nodes "
        "read this from ``input.ctx`` inside their ``run(input)`` "
        "override. Read-only — the engine constructs and owns it.")
        .def_property_readonly("cancel_token",
            [](const RunContext& c) -> py::object {
                if (c.cancel_token) return py::cast(c.cancel_token);
                return py::none();
            },
            "The active CancelToken for this run, or None when the "
            "caller didn't opt in. Pass to provider.complete via "
            "``CompletionParams(cancel_token=input.ctx.cancel_token)``.")
        .def_readonly("step", &RunContext::step,
            "Current super-step index, updated by the engine at the "
            "top of each super-step iteration.")
        .def_readonly("thread_id", &RunContext::thread_id,
            "Mirrors ``RunConfig.thread_id``.")
        .def_property_readonly("stream_mode",
            [](const RunContext& c) {
                return static_cast<uint8_t>(c.stream_mode);
            },
            "Mirrors ``RunConfig.stream_mode`` as the underlying flag bits.");

    // ── NodeInput (per-call bundle, v0.4 PR 7) ──────────────────────────
    py::class_<NodeInput>(m, "NodeInput",
        "Per-call input bundle for the v0.4 unified ``run()`` "
        "override. New Python nodes override "
        "``def run(self, input)`` and read ``input.state`` / "
        "``input.ctx`` / ``input.stream_cb``.")
        .def_property_readonly("state",
            [](const NodeInput& in) {
                return py::cast(&in.state, py::return_value_policy::reference);
            },
            "Read-only ``GraphState`` for this dispatch.")
        .def_property_readonly("ctx",
            [](const NodeInput& in) {
                return py::cast(&in.ctx, py::return_value_policy::reference);
            },
            "``RunContext`` carrying cancel_token, step, thread_id, etc.")
        .def_property_readonly("stream_cb",
            [](const NodeInput& in) -> py::object {
                if (in.stream_cb) {
                    // Wrap the C++ callback as a Python callable so the
                    // node body can invoke it directly.
                    return py::cpp_function(
                        [cb = *in.stream_cb](const GraphEvent& ev) { cb(ev); });
                }
                return py::none();
            },
            "Streaming sink — a callable that takes a GraphEvent, or "
            "None when the caller used a non-streaming ``run`` / "
            "``run_async`` entry. Emit ``LLM_TOKEN`` events through "
            "this when non-null.");

    // ── NodeFactory (custom-node-type registry) ─────────────────────────
    //
    // Lifetime model — important: NodeFactory is a C++ process-
    // lifetime singleton. If we stored py::function refs directly
    // inside its std::function entries, the destruction order at
    // process exit would be:
    //
    //     1. Python interpreter finalizes (Py_Finalize)
    //     2. C++ static destructors run during dlclose / process exit
    //     3. ~std::function tries to drop the captured py::function
    //        → calls Py_DECREF → tstate is gone → fatal abort
    //
    // To dodge that, the factory's C++ closure captures ONLY the
    // type name string. The actual Python callable lives in a
    // module-level Python dict (``_python_factories``) that's torn
    // down naturally during Python GC — well before C++ statics get
    // poked. At call time (inside compile(), under GIL) the closure
    // looks up the callable by type name.
    //
    // Bonus: the C++ closure is now POD-ish and the std::function
    // copy/move is cheap (just the string).
    m.attr("_python_factories") = py::dict();

    m.def("_register_python_factory_internal",
        [](const std::string& type) {
            NodeFactory::instance().register_type(type,
                [type](const std::string& name,
                       const json& config,
                       const NodeContext& ctx)
                        -> std::unique_ptr<GraphNode> {
                    py::gil_scoped_acquire g;
                    // Resolve the registered Python callable from the
                    // module-level dict each invocation. Cheap dict
                    // lookup, GIL held throughout.
                    py::module_ mod =
                        py::module_::import("neograph_engine._neograph");
                    py::dict registry =
                        mod.attr("_python_factories").cast<py::dict>();
                    if (!registry.contains(type.c_str())) {
                        throw std::runtime_error(
                            "neograph: node type '" + type +
                            "' is registered with the C++ NodeFactory "
                            "but missing from the Python factory "
                            "registry — did you call register_type "
                            "from a different interpreter?");
                    }
                    py::function fn =
                        registry[type.c_str()].cast<py::function>();
                    py::object obj = fn(name,
                                        json_to_py(config),
                                        ctx);
                    return std::make_unique<PyGraphNodeOwner>(
                        std::move(obj));
                });
        },
        py::arg("type"),
        "Internal: wires the C++ NodeFactory entry for `type` to the "
        "Python callable in `_python_factories[type]`. Called by "
        "NodeFactory.register_type after the dict slot is populated.");

    py::class_<NodeFactory> node_factory(m, "NodeFactory",
        "Registry of node-type factories used by GraphEngine.compile() "
        "to instantiate nodes by their JSON \"type\". Built-in types: "
        "\"llm_call\", \"tool_dispatch\", \"intent_classifier\", "
        "\"subgraph\". Register custom Python node types via "
        "NodeFactory.register_type().");
    node_factory
        .def_static("register_type",
            [m](const std::string& type, py::function py_factory) {
                // Stash the callable on the module dict (Python-
                // owned lifetime) and wire the C++ shim.
                py::dict registry =
                    m.attr("_python_factories").cast<py::dict>();
                registry[type.c_str()] = py_factory;
                m.attr("_register_python_factory_internal")(type);
            },
            py::arg("type"),
            py::arg("factory"),
            "Register a Python callable as the factory for a node type. "
            "The callable is invoked as `factory(name, config_dict, "
            "ctx)` during GraphEngine.compile() and must return a "
            "neograph.GraphNode subclass instance.")
        .def_static("instance",
            // Returned for parity with the C++ singleton accessor;
            // there's nothing to do with it from Python beyond
            // calling register_type, which is already a classmethod.
            []() { return std::ref(NodeFactory::instance()); },
            py::return_value_policy::reference,
            "Return the singleton NodeFactory instance. Provided for "
            "API parity with the C++ side; equivalent to using the "
            "static `register_type` directly.");
}

} // namespace neograph::pybind
