// Custom Python graph nodes with native coroutine and callback ownership.
//
// Pattern: a thin C++ wrapper class (PyGraphNodeOwner) holds a
// reference to the Python user's node instance and forwards every
// virtual call across the GIL boundary. Python users subclass the
// pure-Python `neograph_engine.GraphNode` base —
// no pybind11 trampoline holder, because the engine consumes nodes
// as `unique_ptr<GraphNode>` and pybind11's trampoline holders don't
// compose cleanly with that.
//
// The engine calls GraphNode::run(NodeInput). PyGraphNodeOwner keeps the
// coroutine boundary in C++ and invokes the user's regular Python
// `run(self, input)` method while holding the GIL.
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
//     GIL-acquiring deleter so concurrent compiles do not race Python cleanup.

#include "interpreter_exit.h"
#include "json_bridge.h"
#include "node_failure_bridge.h"
#include "opaque_types.h"
#include "provider_bridge.h"

#include <neograph/graph/engine.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>
#include <neograph/graph/store.h>
#include <neograph/graph/types.h>
#include <neograph/hook_outbox.h>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>

#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
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
                        "run(input) returned multiple Command "
                        "instances; only one is allowed per node.");
                }
                r.command = item.cast<Command>();
            } else {
                throw py::type_error(
                    "run(input) result list must contain "
                    "ChannelWrite / Send / Command instances; got " +
                    std::string(py::str(item.get_type()).cast<std::string>()));
            }
        }
        return r;
    }

    throw py::type_error(
        "run(input) must return an iterable of "
        "ChannelWrite, a NodeResult, a Command, a Send, or None; got " +
        std::string(py::str(obj.get_type()).cast<std::string>()));
}

// Translate a Python `neograph_engine.NodeInterrupt` into the C++ exception
// the engine understands (issue #94).
//
// Without this, a Python node had no way to pause a run: the exception fell
// through as a generic Python error, the engine treated it as a failure, and
// there was no checkpoint and nothing to resume.
//
// The translation happens *here*, inside the trampoline, rather than by
// registering an exception translator and letting things propagate: a C++
// exception crossing the pybind11 boundary trips ASan's __cxa_throw
// interceptor (see the sanitizer CI job's deselect list). Catching the Python
// error, reading its fields under the GIL, and throwing a fresh C++ exception
// afterwards keeps each exception on one side of the boundary.
//
// Narrow on purpose: only NodeInterrupt is converted. Every other exception
// stays an error, because a bug in a node must fail the run loudly instead of
// looking like a graceful "waiting for a human".
std::optional<neograph::graph::NodeInterrupt>
translate_node_interrupt(py::error_already_set& e) {
    py::object type;
    try {
        type = py::module_::import("neograph_engine").attr("NodeInterrupt");
    } catch (const py::error_already_set&) {
        return std::nullopt;   // module half-initialised; treat as a plain error
    }
    if (!e.matches(type)) return std::nullopt;

    py::object exc = e.value();
    std::string reason;
    if (py::hasattr(exc, "reason")) {
        reason = py::str(exc.attr("reason")).cast<std::string>();
    }
    json value;   // stays null when the node attached no payload
    if (py::hasattr(exc, "value")) {
        py::object v = exc.attr("value");
        if (!v.is_none()) value = py_to_json(v);
    }
    return neograph::graph::NodeInterrupt(reason, std::move(value));
}

// True for the Program diagnostic exceptions (#364): a configuration verdict.
bool is_program_diagnostic(const py::object& exc) {
    try {
        const auto module = py::module_::import("neograph_engine._neograph");
        for (const char* name :
             {"ProgramDiagnosticError", "ProgramCompileError", "ProgramAdmissionError"}) {
            if (!py::hasattr(module, name)) continue;
            const int matched = PyObject_IsInstance(exc.ptr(), module.attr(name).ptr());
            if (matched == 1) return true;
            if (matched < 0) PyErr_Clear();
        }
    } catch (const py::error_already_set&) {
        PyErr_Clear();
    }
    return false;
}

// Translate any other Python exception from a node body into a typed node
// failure (#364). The failure classifier cannot read Python attributes, so they
// are read here, under the GIL:
//
//   - `code`: a string author code (`app.compile_error`; see
//     is_author_failure_code) names the failure. Anything else, such as an
//     HTTP status in `code` or a vendor code, is ignored: the failure is
//     NG.TaskFailed.
//   - `retryable`: a bool; False stops the node's retry loop.
//   - `public_message`: a str, the only text a failure record may carry.
//   - A Program diagnostic exception (ProgramDiagnosticError and friends) is a
//     configuration verdict: NG.ConfigError, never retried or routed.
//
// The original exception rides along and is what Python callers receive.
// Must be called inside the catch block that holds @p e, with the GIL held.
std::exception_ptr translate_node_failure(py::error_already_set& e) {
    std::string code;
    std::string public_message;
    std::optional<bool> retryable;
    py::object exc = e.value();
    // Reading an attribute runs Python code (properties, __getattr__); any
    // error there leaves the attribute unread.
    const auto attribute = [&exc](const char* name) -> py::object {
        try {
            if (py::hasattr(exc, name)) return exc.attr(name);
        } catch (const py::error_already_set&) {
        }
        return py::none();
    };
    if (const auto value = attribute("code"); py::isinstance<py::str>(value)) {
        auto text = value.cast<std::string>();
        if (neograph::graph::is_author_failure_code(text)) code = std::move(text);
    }
    if (const auto value = attribute("retryable"); py::isinstance<py::bool_>(value))
        retryable = value.cast<bool>();
    if (const auto value = attribute("public_message"); py::isinstance<py::str>(value))
        public_message = value.cast<std::string>();
    if (is_program_diagnostic(exc))
        return std::make_exception_ptr(PythonNodeConfigurationFailure(
            e.what(), std::move(code), std::move(public_message), retryable,
            std::current_exception()));
    return std::make_exception_ptr(PythonNodeFailure(e.what(), std::move(code),
                                                     std::move(public_message), retryable,
                                                     std::current_exception()));
}

// The run control signals a Python tool or node can raise, as the C++
// exceptions the tool failure contract recognises: a NodeInterrupt pauses the run and a
// HookBoundaryBlocked (typically escaping a nested engine run inside the tool)
// stops it, exactly as they would from a C++ tool. Everything else stays the
// Python error, which the dispatcher turns into a tool message. Same
// discipline as translate_node_interrupt: read under the GIL, throw after it.
struct PythonToolControlSignal {
    std::optional<neograph::graph::NodeInterrupt> interrupt;
    std::optional<neograph::HookBoundaryBlocked> hook_blocked;
    explicit operator bool() const { return interrupt || hook_blocked; }
    [[noreturn]] void raise() const {
        if (interrupt) throw *interrupt;
        throw *hook_blocked;
    }
};

PythonToolControlSignal translate_tool_control_signal(py::error_already_set& e) {
    PythonToolControlSignal signal;
    signal.interrupt = translate_node_interrupt(e);
    if (signal.interrupt) return signal;
    py::object type;
    try {
        type = py::module_::import("neograph_engine").attr("HookBoundaryBlocked");
    } catch (const py::error_already_set&) {
        return signal;
    }
    if (!e.matches(type)) return signal;
    py::object exc = e.value();
    std::string invocation_id;
    auto state = neograph::HookExecutionState::Failed;
    if (py::hasattr(exc, "invocation_id") && !exc.attr("invocation_id").is_none()) {
        invocation_id = py::str(exc.attr("invocation_id")).cast<std::string>();
    }
    if (py::hasattr(exc, "state") && !exc.attr("state").is_none()) {
        const auto name = py::str(exc.attr("state")).cast<std::string>();
        for (auto candidate : {neograph::HookExecutionState::Triggered,
                               neograph::HookExecutionState::Pending,
                               neograph::HookExecutionState::Dispatched,
                               neograph::HookExecutionState::Succeeded,
                               neograph::HookExecutionState::Failed,
                               neograph::HookExecutionState::TimedOut,
                               neograph::HookExecutionState::ReconciliationRequired,
                               neograph::HookExecutionState::Cancelled}) {
            if (neograph::to_string(candidate) == name) state = candidate;
        }
    }
    signal.hook_blocked.emplace(std::move(invocation_id), state,
                                py::str(exc).cast<std::string>());
    return signal;
}

// PyToolOwner: bridges the C++ Tool interface to a held Python user
// object. Created at GraphEngine.compile() time from the
// `_pytools` attribute on the Python NodeContext wrapper, then retained
// by the compiled graph's ToolSet. Pattern parallels PyGraphNodeOwner —
// same GIL handshake, same destructor caveat.

// The exit hook stops both worker pools below (close_worker_pools) so that
// their threads do not take the GIL while the interpreter finalizes
// (interpreter_exit.h). The pools are only stopped if they were started, and
// after that nothing is offloaded to them: the body runs on the calling thread,
// as it does for a tool that is not an AsyncTool.
std::atomic<asio::thread_pool*>& async_tool_pool_started() {
    static std::atomic<asio::thread_pool*> started{nullptr};
    return started;
}
std::atomic<asio::thread_pool*>& python_node_pool_started() {
    static std::atomic<asio::thread_pool*> started{nullptr};
    return started;
}
std::atomic<bool>& worker_pools_are_closed() {
    static std::atomic<bool> closed{false};
    return closed;
}

// Worker pool for AsyncTool bodies (issue #96).
//
// Overlapping Python tools means running their bodies on separate threads —
// there is no other way, since a Python function holds the GIL while it runs
// and only lets go when it blocks on I/O. These threads therefore spend their
// lives blocked on sockets, so the pool is sized generously rather than to the
// core count: it bounds how many tool calls can be in flight, not how much CPU
// is used.
asio::thread_pool& async_tool_pool() {
    static asio::thread_pool pool([] {
        if (const char* env = std::getenv("NEOGRAPH_TOOL_THREADS")) {
            long n = std::strtol(env, nullptr, 10);
            if (n > 0 && n <= 1024) return static_cast<std::size_t>(n);
        }
        return static_cast<std::size_t>(32);
    }());
    async_tool_pool_started().store(&pool, std::memory_order_release);
    return pool;
}

// Worker pool for Python node bodies on a shared host scheduler (#367, #368).
//
// A Program runtime runs join members and await timers on one scheduler. A
// Python node body holds its thread until it returns, so it runs here instead;
// the GIL is still taken for the call itself. NEOGRAPH_PYTHON_NODE_THREADS
// bounds how many bodies can be in flight.
asio::thread_pool& python_node_pool() {
    static asio::thread_pool pool([] {
        if (const char* env = std::getenv("NEOGRAPH_PYTHON_NODE_THREADS")) {
            long n = std::strtol(env, nullptr, 10);
            if (n > 0 && n <= 1024) return static_cast<std::size_t>(n);
        }
        return static_cast<std::size_t>(32);
    }());
    python_node_pool_started().store(&pool, std::memory_order_release);
    return pool;
}

// Lets a Python-owned C++ tool (an MCPTool, say) satisfy the engine's
// `unique_ptr<Tool>` ownership while Python keeps its shared_ptr. Forwards
// everything, and — the point — forwards `execute_async` to the real tool, so a
// tool that genuinely suspends still does (issue #95).
class SharedToolRef final : public neograph::Tool,
                            public neograph::ContextualAsyncTool {
public:
    explicit SharedToolRef(std::shared_ptr<neograph::Tool> tool)
        : tool_(std::move(tool)) {}

    neograph::ChatTool get_definition() const override {
        return tool_->get_definition();
    }

    std::string execute(const json& arguments) override {
        return tool_->execute(arguments);
    }

    // Plain forwarding, NOT a coroutine: `co_await tool_->execute_async(args)`
    // here would build a second frame for no reason, and `arguments` is a
    // reference the caller keeps alive across the await either way.
    asio::awaitable<std::string> execute_async(const json& arguments) override {
        return tool_->execute_async(arguments);
    }

    asio::awaitable<std::string> execute_async(
        const json& arguments, neograph::ToolExecutionContext execution) override {
        if (auto* contextual = dynamic_cast<neograph::ContextualAsyncTool*>(tool_.get())) {
            co_return co_await contextual->execute_async(arguments, std::move(execution));
        }
        if (execution.cancel_token) {
            execution.cancel_token->throw_if_cancelled("before tool execution");
        }
        auto result = co_await tool_->execute_async(arguments);
        if (execution.cancel_token) {
            execution.cancel_token->throw_if_cancelled("after tool execution");
        }
        co_return result;
    }

    std::string get_name() const override { return tool_->get_name(); }

private:
    std::shared_ptr<neograph::Tool> tool_;
};

class PyToolOwner final : public neograph::Tool {
public:
    explicit PyToolOwner(py::object py_obj)
        : py_obj_(std::move(py_obj)) {
        // Does this tool opt in to running concurrently? Decided once, here,
        // rather than on every call — the answer cannot change, and the check
        // needs the GIL.
        py::gil_scoped_acquire g;
        try {
            py::object base =
                py::module_::import("neograph_engine").attr("AsyncTool");
            concurrent_ = py::isinstance(py_obj_, base);
        } catch (const py::error_already_set&) {
            concurrent_ = false;   // conservative: serial is always correct
        }
    }

    ~PyToolOwner() override { release_with_gil(py_obj_); }

    neograph::ChatTool get_definition() const override {
        py::gil_scoped_acquire g;
        return py_obj_.attr("get_definition")()
            .cast<neograph::ChatTool>();
    }

    std::string execute(const json& arguments) override {
        PythonToolControlSignal signal;
        {
            EngineGil g;
            try {
                return call_python(arguments);
            } catch (py::error_already_set& e) {
                signal = translate_tool_control_signal(e);
                if (!signal) throw;   // an ordinary failure: the model sees it
            }
        }
        signal.raise();
    }

    // The whole of issue #96 is this override.
    //
    // Tool::execute_async's default body is `co_return execute(arguments)` —
    // it runs to completion before the coroutine's first suspension, so a
    // sync tool never overlaps with its siblings even inside the dispatcher's
    // parallel group. That is not an accident; it is the guarantee that let
    // #87 land concurrency without racing anybody's existing stateful tool.
    //
    // A tool that subclassed ng.AsyncTool has said it is safe to overlap. For
    // those, and only those, we hop onto a worker thread, which is where the
    // GIL can actually be released while the tool waits on I/O.
    asio::awaitable<std::string> execute_async(const json& arguments) override {
        if (!concurrent_ || worker_pools_are_closed().load(std::memory_order_acquire)) {
            co_return execute(arguments);   // unchanged: serial, as before
        }
        co_return co_await asio::co_spawn(
            async_tool_pool().get_executor(),
            offload(this, arguments),
            asio::use_awaitable);
    }

    std::string get_name() const override {
        py::gil_scoped_acquire g;
        return py_obj_.attr("get_name")().cast<std::string>();
    }

private:
    /// GIL must be held.
    std::string call_python(const json& arguments) {
        py::object args_py = json_to_py(arguments);
        py::object result = py_obj_.attr("execute")(args_py);
        // A str result crosses as UTF-8; lone surrogates become U+FFFD, the
        // rule every tool result follows. bytes pass through, and the
        // execution boundary repairs any ill-formed UTF-8 in them.
        if (py::isinstance<py::str>(result)) return py_to_utf8(result);
        return result.cast<std::string>();
    }

    // `arguments` is a coroutine PARAMETER, taken by value, and not a lambda
    // capture: a lambda's closure dies as soon as it has handed back the
    // awaitable, so a captured argument would dangle. A parameter lives in the
    // coroutine frame. (`self` is a raw pointer on purpose — the tool outlives
    // the run, and copying a py::object here would touch a CPython refcount
    // without the GIL.)
    static asio::awaitable<std::string> offload(PyToolOwner* self, json arguments) {
        co_return self->execute(arguments);
    }

    py::object py_obj_;
    bool       concurrent_ = false;
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
        // refcount; dropping it MUST happen with the GIL held, and it is
        // left alone when the interpreter can no longer be entered.
        release_with_gil(py_obj_);
    }

    // ── Required: name lookup ────────────────────────────────────────
    std::string get_name() const override {
        py::gil_scoped_acquire g;
        return py_obj_.attr("get_name")().cast<std::string>();
    }

    // Python nodes implement run(input), returning writes or a NodeResult.
    // They receive cancellation explicitly through input.ctx.cancel_token
    // and attach that handle to any provider request they issue.
    asio::awaitable<NodeResult> run(NodeInput in) override {
        // On a shared host scheduler the body would hold the scheduler thread
        // for its whole duration, stalling sibling join members and await
        // timers (#367, #368). Elsewhere the body keeps running inline.
        if (in.ctx.shared_scheduler && !worker_pools_are_closed().load(std::memory_order_acquire)) {
            co_return co_await asio::co_spawn(python_node_pool().get_executor(),
                                              run_on_worker(this, &in), asio::use_awaitable);
        }
        co_return call_python(in);
    }

private:
    // `in` lives in the awaiting run() frame, which outlives this call.
    static asio::awaitable<NodeResult> run_on_worker(PyGraphNodeOwner* self, NodeInput* in) {
        co_return self->call_python(*in);
    }

    NodeResult call_python(NodeInput& in) {
        NodeResult result;
        // Captured under the GIL, thrown after releasing it — see
        // translate_node_interrupt.
        // The run control signals a node can raise are the ones a tool can:
        // a NodeInterrupt pauses the run and a HookBoundaryBlocked (from a
        // nested engine run) stops it, never retried or routed, exactly as
        // from a C++ node.
        PythonToolControlSignal signal;
        std::exception_ptr failure;
        {
            EngineGil g;
            py::object py_input = py::cast(&in,
                py::return_value_policy::reference);
            try {
                py::object res = py_obj_.attr("run")(py_input);
                result = coerce_to_node_result(res);
            } catch (py::error_already_set& e) {
                signal = translate_tool_control_signal(e);
                // A real error, not a pause: a typed node failure.
                if (!signal) failure = translate_node_failure(e);
            }
        }
        if (signal) signal.raise();
        if (failure) std::rethrow_exception(failure);
        return result;
    }

    py::object py_obj_;
};

// Wrap an arbitrary Python callable in a shared_ptr with a
// GIL-acquiring deleter, so std::function copies (registry storage,
// engine internal hand-offs) don't touch the CPython refcount.
template <typename PyCallable>
std::shared_ptr<PyCallable> make_gil_safe(PyCallable&& fn) {
    return std::shared_ptr<PyCallable>(new PyCallable(std::forward<PyCallable>(fn)),
                                       &delete_with_gil<PyCallable>);
}

} // namespace

void close_worker_pools() {
    // Stop accepting work first, so that nothing is offloaded to a pool that
    // is about to stop; then let the bodies that run finish (they need the GIL,
    // which the caller has released) and join the threads.
    worker_pools_are_closed().store(true, std::memory_order_release);
    for (auto* started : {&async_tool_pool_started(), &python_node_pool_started()}) {
        if (auto* pool = started->load(std::memory_order_acquire)) {
            pool->stop();
            pool->join();
        }
    }
}

void register_scoped_python_node(graph::GraphRegistry& registry, const std::string& type,
                                 py::function factory) {
    auto held = make_gil_safe(std::move(factory));
    registry.register_type(
        type,
        [held](const std::string& name, const json& config,
               const NodeContext& ctx) -> std::unique_ptr<GraphNode> {
            py::gil_scoped_acquire g;
            return std::make_unique<PyGraphNodeOwner>(
                (*held)(name, json_to_py(config), ctx));
        });
}

// Wrap Python and native tools into a single owning ToolSet before compilation.
// The compiled graph and its engine independently retain the exact collection.
std::vector<std::unique_ptr<neograph::Tool>>
wrap_python_tools(py::handle tools_list) {
    std::vector<std::unique_ptr<neograph::Tool>> out;
    if (tools_list.is_none()) return out;
    for (auto t : tools_list) {
        // A tool that is ALREADY a C++ Tool — an MCPTool from
        // MCPClient.get_tools(), say (issue #95) — must pass through as what it
        // is. Wrapping it in a PyToolOwner would compile, run, and pass a
        // functional test, while losing its native async execution path: a
        // PyToolOwner only forwards real async work when it holds a Python
        // ng.AsyncTool. A host may then explicitly widen the preserved C++
        // tool's named policy after verifying the remote server is re-entrant.
        if (py::isinstance<neograph::Tool>(t)) {
            auto shared = t.cast<std::shared_ptr<neograph::Tool>>();
            out.push_back(std::make_unique<SharedToolRef>(std::move(shared)));
            continue;
        }
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
        .def("get_provider_messages", &GraphState::get_provider_messages,
            py::arg("channel") = "messages", "Full typed history with authentic native replay ownership.")
        .def("captured_provider_messages", &GraphState::captured_provider_messages,
            py::arg("channel") = "messages")
        .def("provider_outcomes", &GraphState::provider_outcomes)
        .def("serialize",
            [](const GraphState& s) { return json_to_py(s.serialize()); },
            "Whole-state JSON snapshot (channels + global version). "
            "Mostly useful for debugging custom nodes.")
        .def("channel_names", &GraphState::channel_names,
            "List of all channel names declared by the graph "
            "definition.")
        .def("channel_version", &GraphState::channel_version,
            py::arg("channel"),
            "Version of a channel: the value of the state's write counter at the "
            "channel's last write, 0 if it was never written. All channels share the "
            "counter, so versions order writes; they do not count a channel's writes.")
        .def("global_version", &GraphState::global_version,
            "The state's write counter: every write to any channel adds 1 and stamps "
            "the written channel with the new value. Useful for detecting any change "
            "since a given snapshot.");

    // ── NodeResult (Command + Send + writes union) ──────────────────────
    py::class_<NodeResult>(m, "NodeResult",
        "Full node return shape: writes + optional Command (routing "
        "override) + optional Sends (dynamic fan-out). Construct one "
        "explicitly when emitting Command/Send; for writes-only nodes, "
        "just return a list of ChannelWrite from run(input).")
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

    // ── Cooperative cancellation ────────────────────────────────────────
    //
    // Python callers can construct a token and attach it to
    // RunConfig.cancel_token to stop a synchronous run from another thread.
    // Nodes can also read the active token through input.ctx.cancel_token.
    py::class_<CancelToken, std::shared_ptr<CancelToken>>(m, "CancelToken",
        "Cooperative cancel handle for an in-flight run. Callers may attach "
        "one to ``RunConfig.cancel_token``; nodes read "
        "``input.ctx.cancel_token`` and "
        "pass it as ``request.cancel_token`` to a typed provider request "
        "or poll ``is_cancelled()`` for "
        "their own loops.")
        .def(py::init<>())
        .def("is_cancelled", &CancelToken::is_cancelled,
            "Lock-free polling read of the cancel flag.")
        .def("cancel", &CancelToken::cancel,
            "Request cancellation. Thread-safe, idempotent.");

    // ── Per-run dispatch metadata ──────────────────────────────────────
    //
    // Exposed read-only — the engine constructs and owns the RunContext;
    // Python users read it from ``NodeInput.ctx`` inside their ``run()``
    // Provider outcomes, history and usage retain their native owners. These
    // are observations from the current engine invocation, not imported JSON.
    py::class_<RunContext>(m, "RunContext",
        "Per-run dispatch metadata threaded by the engine. New nodes "
        "read this from ``input.ctx`` inside their ``run(input)`` "
        "override. Read-only — the engine constructs and owns it.")
        .def_property_readonly("cancel_token",
            [](const RunContext& c) -> py::object {
                if (c.cancel_token) return py::cast(c.cancel_token);
                return py::none();
            },
            "The active CancelToken for this run. Assign it to "
            "``request.cancel_token`` before provider invocation.")
        .def_readonly("step", &RunContext::step,
            "Current super-step index, updated by the engine at the "
            "top of each super-step iteration.")
        .def_readonly("thread_id", &RunContext::thread_id,
            "Mirrors ``RunConfig.thread_id``.")
        .def_readonly("run_id", &RunContext::run_id,
            "Durable Program run identity supplied through RunMetadata.")
        .def_readonly("trace_id", &RunContext::trace_id,
            "Trace correlator supplied through RunMetadata.")
        .def_readonly("model_token_budget", &RunContext::model_token_budget)
        .def_readonly("usage", &RunContext::usage)
        .def_readonly("provider_outcomes", &RunContext::provider_outcomes)
        .def_readonly("provider_loop_history", &RunContext::provider_loop_history)
        .def_readonly("native_history_archive", &RunContext::native_history_archive)
        .def_property_readonly("on_provider_event", [](const RunContext& c) {
            return provider_observer_function(c.on_provider_event);
        })
        .def_property_readonly("budget_cancel_token",
            [](const RunContext& c) -> py::object {
                return c.budget_cancel_token ? py::cast(c.budget_cancel_token) : py::none();
            })
        .def_property_readonly("has_deadline",
            [](const RunContext& c) { return c.deadline.has_value(); })
        .def_property_readonly("deadline_remaining_ms",
            [](const RunContext& c) -> py::object {
                if (!c.deadline) return py::none();
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    *c.deadline - std::chrono::steady_clock::now()).count();
                return py::int_(std::max<std::int64_t>(0, remaining));
            })
        .def_property_readonly("stream_mode",
            [](const RunContext& c) {
                return static_cast<uint8_t>(c.stream_mode);
            },
            "Mirrors ``RunConfig.stream_mode`` as the underlying flag bits.")
        .def_property_readonly("store",
            [](const RunContext& c) -> py::object {
                if (!c.store) return py::none();
                return py::cast(c.store);
            },
            "The engine's Store, or None when none was installed. This is how "
            "a node body remembers something across conversations (issue #97):\n\n"
            "    seen = input.ctx.store.get([\"users\", uid], \"prefs\")")
        .def_property_readonly("resume_value",
            [](const RunContext& c) -> py::object {
                if (!c.resume_value) return py::none();
                return json_to_py(*c.resume_value);
            },
            "The value passed to ``engine.resume(thread_id, value)`` — the "
            "human's answer to a NodeInterrupt. ``None`` on a fresh run, so a "
            "node can tell \"nobody has answered yet\" from \"the answer was "
            "no\" (issue #94). Only the node or Send the resume answers sees "
            "it, in the step the resume re-enters; every other node, and every "
            "node of a later step, sees ``None`` (#393).")
        .def_property_readonly("resume_interrupt_id",
            [](const RunContext& c) -> py::object {
                if (c.resume_interrupt_id.empty()) return py::none();
                return py::str(c.resume_interrupt_id);
            },
            "The id of the interrupt ``resume_value`` answers, when it answers "
            "this node's own ``NodeInterrupt``: the id ``result.interrupt_id`` "
            "and ``result.interrupts`` report (``\"s3:review\"``), whether the "
            "resume named it with ``interrupt_id=`` or answered the first one. "
            "``None`` when ``resume_value`` is the approval of the "
            "``interrupt_before`` gate that held this node, and whenever "
            "``resume_value`` is ``None`` (#406). A gated node that also asks its "
            "own question tells the two apart by it.");

    // ── Per-call input bundle ──────────────────────────────────────────
    py::class_<NodeInput>(m, "NodeInput",
        "Per-call input bundle for ``run(input)``. Nodes override "
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
            "neograph_engine.GraphNode subclass instance.")
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
