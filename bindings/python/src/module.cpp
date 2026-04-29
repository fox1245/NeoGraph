// Root pybind11 module — wires up the per-area bind_*() entry points.
//
// Each `init_*()` lives in its own TU so a build that only touched
// one binding source recompiles just that TU. Linker + pybind11 fold
// them all into a single `_neograph` shared object.

#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace neograph::pybind {
void init_provider(py::module_& m);
void init_state(py::module_& m);
void init_graph(py::module_& m);
void init_node(py::module_& m);
#ifdef NEOGRAPH_PYBIND_HAS_A2A
void init_a2a(py::module_& m);
#endif
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

    neograph::pybind::init_provider(m);
    neograph::pybind::init_state(m);
    neograph::pybind::init_graph(m);
    neograph::pybind::init_node(m);
#ifdef NEOGRAPH_PYBIND_HAS_A2A
    neograph::pybind::init_a2a(m);
#endif
}
