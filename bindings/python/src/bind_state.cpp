// Plain-data graph types: NodeContext, ChannelWrite, Send, Command,
// StreamMode, GraphEvent. Plus the START_NODE / END_NODE constants.

#include "json_bridge.h"

#include <neograph/graph/types.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <string>
#include <vector>

namespace py = pybind11;

namespace neograph::pybind {

void init_state(py::module_& m) {
    using namespace neograph::graph;

    // ── NodeContext ──────────────────────────────────────────────────────
    //
    // py::dynamic_attr() lets the Python wrapper carry attributes that
    // aren't in the C++ struct — specifically `_pytools`, the list of
    // Python Tool instances. The Python `NodeContext` subclass (in
    // neograph_engine/__init__.py) sets `_pytools` in its __init__,
    // and GraphEngine.compile() reads it back to materialize the
    // PyToolOwner unique_ptrs that the engine takes ownership of.
    py::class_<NodeContext>(m, "NodeContext", py::dynamic_attr(),
        "Dependency-injection container for graph nodes: provider, "
        "tools, model, system instructions, plus an extra_config dict "
        "for node-type-specific settings.")
        .def(py::init([](std::shared_ptr<neograph::Provider> provider,
                         std::vector<std::shared_ptr<neograph::Tool>> tools,
                         const std::string& model,
                         const std::string& instructions,
                         py::object extra_config) {
            NodeContext ctx;
            ctx.provider = std::move(provider);
            // NodeContext stores raw Tool* (engine owns the unique_ptrs
            // separately). Commit 1 doesn't expose tool ownership transfer
            // from Python yet — keep tools empty here, add a setter once
            // the trampoline lands in commit 2.
            (void)tools;
            ctx.model = model;
            ctx.instructions = instructions;
            ctx.extra_config = py_to_json(extra_config);
            return ctx;
        }),
            py::arg("provider") = py::none(),
            py::arg("tools") = std::vector<std::shared_ptr<neograph::Tool>>{},
            py::arg("model") = "",
            py::arg("instructions") = "",
            py::arg("extra_config") = py::dict())
        .def_readwrite("provider", &NodeContext::provider)
        .def_readwrite("model", &NodeContext::model)
        .def_readwrite("instructions", &NodeContext::instructions)
        .def_property("extra_config",
            [](const NodeContext& c) { return json_to_py(c.extra_config); },
            [](NodeContext& c, py::object v) { c.extra_config = py_to_json(v); });

    // ── ChannelWrite ─────────────────────────────────────────────────────
    py::class_<ChannelWrite>(m, "ChannelWrite",
        "A write to a named state channel.")
        .def(py::init([](const std::string& channel, py::object value) {
            ChannelWrite w;
            w.channel = channel;
            w.value = py_to_json(value);
            return w;
        }), py::arg("channel"), py::arg("value"))
        .def_readwrite("channel", &ChannelWrite::channel)
        .def_property("value",
            [](const ChannelWrite& w) { return json_to_py(w.value); },
            [](ChannelWrite& w, py::object v) { w.value = py_to_json(v); })
        .def("__repr__", [](const ChannelWrite& w) {
            return "<ChannelWrite channel=" + w.channel + ">";
        });

    // ── Send (dynamic fan-out) ───────────────────────────────────────────
    py::class_<Send>(m, "Send",
        "Dynamic fan-out request. Returning Sends from execute_full() "
        "dispatches the target node N times in parallel with distinct "
        "input dicts.")
        .def(py::init([](const std::string& target_node, py::object input) {
            Send s;
            s.target_node = target_node;
            s.input = py_to_json(input);
            return s;
        }), py::arg("target_node"), py::arg("input") = py::dict())
        .def_readwrite("target_node", &Send::target_node)
        .def_property("input",
            [](const Send& s) { return json_to_py(s.input); },
            [](Send& s, py::object v) { s.input = py_to_json(v); });

    // ── Command (routing override + state update) ────────────────────────
    py::class_<Command>(m, "Command",
        "Routing override: simultaneously update state and pick the "
        "next node, bypassing edge-based routing.")
        .def(py::init([](const std::string& goto_node,
                         std::vector<ChannelWrite> updates) {
            Command c;
            c.goto_node = goto_node;
            c.updates = std::move(updates);
            return c;
        }), py::arg("goto_node"), py::arg("updates") = std::vector<ChannelWrite>{})
        .def_readwrite("goto_node", &Command::goto_node)
        .def_readwrite("updates",   &Command::updates);

    // ── StreamMode (bitfield enum) ───────────────────────────────────────
    //
    // C++ defines `operator|` / `operator&` as free functions over
    // ``enum class StreamMode``. pybind11's ``py::arithmetic()`` flag
    // only adds int-conversion sugar; it does NOT auto-bind bitwise
    // ops on a strongly-typed enum. We attach __or__ and __and__
    // explicitly so ``StreamMode.EVENTS | StreamMode.TOKENS`` works
    // as expected (and stays type-stable: the result is StreamMode,
    // not int).
    py::enum_<StreamMode> stream_mode(m, "StreamMode", py::arithmetic(),
        "Bitfield selecting which event types to emit during run_stream(). "
        "Combine with bitwise-or: StreamMode.EVENTS | StreamMode.TOKENS.");
    stream_mode
        .value("EVENTS",  StreamMode::EVENTS)
        .value("TOKENS",  StreamMode::TOKENS)
        .value("VALUES",  StreamMode::VALUES)
        .value("UPDATES", StreamMode::UPDATES)
        .value("DEBUG",   StreamMode::DEBUG)
        .value("ALL",     StreamMode::ALL)
        .def("__or__",  [](StreamMode a, StreamMode b) { return a | b; })
        .def("__and__", [](StreamMode a, StreamMode b) { return a & b; });

    // ── GraphEvent + GraphEvent.Type ─────────────────────────────────────
    py::class_<GraphEvent> graph_event(m, "GraphEvent",
        "An event emitted during run_stream(). The `type` field selects "
        "interpretation of `data`.");
    graph_event
        .def(py::init<>())
        .def_readwrite("type",      &GraphEvent::type)
        .def_readwrite("node_name", &GraphEvent::node_name)
        .def_property("data",
            [](const GraphEvent& e) { return json_to_py(e.data); },
            [](GraphEvent& e, py::object v) { e.data = py_to_json(v); });

    py::enum_<GraphEvent::Type>(graph_event, "Type")
        .value("NODE_START",    GraphEvent::Type::NODE_START)
        .value("NODE_END",      GraphEvent::Type::NODE_END)
        .value("LLM_TOKEN",     GraphEvent::Type::LLM_TOKEN)
        .value("CHANNEL_WRITE", GraphEvent::Type::CHANNEL_WRITE)
        .value("INTERRUPT",     GraphEvent::Type::INTERRUPT)
        .value("ERROR",         GraphEvent::Type::ERROR);

    // Special node-name constants. Strings, exposed for symmetry with
    // the C++ ``constexpr const char* START_NODE = "__start__"`` so
    // Python users don't hard-code the magic literals.
    m.attr("START_NODE") = py::str(START_NODE);
    m.attr("END_NODE")   = py::str(END_NODE);
}

} // namespace neograph::pybind
