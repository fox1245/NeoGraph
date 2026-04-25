// Engine surface: GraphEngine.compile(), .run(), .run_stream() with
// Python callback, .get_state(), .update_state(), .set_worker_count().
//
// Async surface (run_async, run_stream_async, resume_async) is held
// for a later commit — needs an asio↔asyncio bridge.

#include "json_bridge.h"

#include <neograph/graph/engine.h>
#include <neograph/graph/types.h>

#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>

#include <memory>
#include <string>

namespace py = pybind11;

namespace neograph::pybind {

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
            [](py::object definition, const NodeContext& ctx) {
                auto j = py_to_json(definition);
                // GIL held: compile is fast (just walks the JSON).
                auto unique = GraphEngine::compile(j, ctx, nullptr);
                return std::shared_ptr<GraphEngine>(unique.release());
            },
            py::arg("definition"),
            py::arg("ctx"),
            "Compile a graph from a JSON-shaped Python dict. The "
            "context provides the LLM provider and (later) tools used "
            "by built-in node types. Raises RuntimeError on bad shape.")

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
            "Opt into a dedicated N-worker pool for parallel fan-out. "
            "Must be called before any run(). Values < 1 clamp to 1.")

        .def_property_readonly("name", &GraphEngine::get_graph_name);
}

} // namespace neograph::pybind
