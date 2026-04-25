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
} // namespace neograph::pybind

PYBIND11_MODULE(_neograph, m) {
    m.doc() =
        "NeoGraph C++ engine — Python bindings.\n"
        "\n"
        "This module is the C extension; the public Python surface is\n"
        "the `neograph` package, which re-exports the symbols below.\n";

    // Version string lives here so it travels with the binding rather
    // than the Python wrapper. Bumped per binding ABI break.
    m.attr("__version__") = "0.1.0";

    neograph::pybind::init_provider(m);
    neograph::pybind::init_state(m);
    neograph::pybind::init_graph(m);
    neograph::pybind::init_node(m);
}
