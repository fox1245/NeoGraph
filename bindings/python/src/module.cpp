// Root pybind11 module — wires up the per-area bind_*() entry points.
//
// Each `init_*()` lives in its own TU so a build that only touched
// one binding source recompiles just that TU. Linker + pybind11 fold
// them all into a single `_neograph` shared object.

#include "opaque_types.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl_bind.h>

namespace py = pybind11;

namespace neograph::pybind {
void init_provider(py::module_& m);
void init_state(py::module_& m);
void init_graph(py::module_& m);
void init_node(py::module_& m);
#ifdef NEOGRAPH_PYBIND_HAS_A2A
void init_a2a(py::module_& m);
#endif

// Register bind_vector classes for the OPAQUE vector types. Must run
// BEFORE any other init_* so call sites that use these vectors can
// resolve the bound class. See opaque_types.h for the rationale.
//
// ``py::implicitly_convertible<py::list, …>`` keeps the legacy
// build-then-assign pattern working: ``params.messages = [m1, m2]``
// constructs a ChatMessageList from the Python list automatically.
// The bound class itself supports ``.append()`` etc. with live
// mutation (closes the v0.4.0 silent no-op trap).
static void init_opaque_vectors(py::module_& m) {
    py::bind_vector<std::vector<neograph::ChatMessage>>(m, "ChatMessageList",
        "List of ChatMessage. Behaves like a Python list (append / "
        "extend / __getitem__ / __setitem__ / __iter__ / __len__) but "
        "writes pass through to the underlying C++ std::vector live — "
        "so ``params.messages.append(msg)`` mutates ``params``.");
    py::implicitly_convertible<py::list, std::vector<neograph::ChatMessage>>();

    py::bind_vector<std::vector<neograph::ChatTool>>(m, "ChatToolList",
        "List of ChatTool. Same live-mutation semantics as ChatMessageList.");
    py::implicitly_convertible<py::list, std::vector<neograph::ChatTool>>();

    py::bind_vector<std::vector<neograph::ToolCall>>(m, "ToolCallList",
        "List of ToolCall. Same live-mutation semantics as ChatMessageList.");
    py::implicitly_convertible<py::list, std::vector<neograph::ToolCall>>();

    py::bind_vector<std::vector<neograph::graph::ChannelWrite>>(m, "ChannelWriteList",
        "List of ChannelWrite. Same live-mutation semantics as ChatMessageList.");
    py::implicitly_convertible<py::list, std::vector<neograph::graph::ChannelWrite>>();

    py::bind_vector<std::vector<neograph::graph::Send>>(m, "SendList",
        "List of Send. Same live-mutation semantics as ChatMessageList.");
    py::implicitly_convertible<py::list, std::vector<neograph::graph::Send>>();
}
} // namespace neograph::pybind

PYBIND11_MODULE(_neograph, m) {
    m.doc() =
        "NeoGraph C++ engine — Python bindings.\n"
        "\n"
        "This module is the C extension; the public Python surface is\n"
        "the `neograph` package, which re-exports the symbols below.\n";

    // Version string is injected at compile time via NEOGRAPH_PY_VERSION
    // in bindings/python/CMakeLists.txt, which reads pyproject.toml's
    // [project].version. Single source of truth — the Python wrapper,
    // PyPI METADATA, and `import neograph_engine; ng.__version__` all
    // agree by construction. See bindings/python/CMakeLists.txt for the
    // cmake-side wiring.
#ifndef NEOGRAPH_PY_VERSION
    #define NEOGRAPH_PY_VERSION "0.0.0+unknown"
#endif
    m.attr("__version__") = NEOGRAPH_PY_VERSION;

    neograph::pybind::init_opaque_vectors(m);
    neograph::pybind::init_provider(m);
    neograph::pybind::init_state(m);
    neograph::pybind::init_graph(m);
    neograph::pybind::init_node(m);
#ifdef NEOGRAPH_PYBIND_HAS_A2A
    neograph::pybind::init_a2a(m);
#endif
}
