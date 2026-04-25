// Bidirectional marshalling between Python objects and neograph::json.
//
// neograph::json is a thin wrapper around yyjson (C). We don't want
// pybind11 to know about yyjson, so the bridge lives in its own TU
// and is the only place that handles type discrimination.
//
// Conversions are deep copies. The Python layer doesn't observe the
// yyjson_mut_doc lifetime — round-tripping a large dict through the
// bridge is O(N) in node count.

#pragma once

#include <neograph/json.h>
#include <neograph/tool.h>
#include <pybind11/pybind11.h>

#include <memory>
#include <vector>

namespace neograph::pybind {

namespace py = ::pybind11;

/// Convert a Python object (dict / list / tuple / str / bool / int /
/// float / None) into a neograph::json value. Tuples and lists both
/// map to JSON arrays. Anything else falls through to ``str()``.
neograph::json py_to_json(py::handle obj);

/// Convert a neograph::json value into a fresh Python object. Returns
/// ``None`` for null, ``bool`` / ``int`` / ``float`` / ``str`` for
/// scalars, ``list`` for arrays, ``dict`` for objects.
py::object json_to_py(const neograph::json& j);

/// Wrap each item in a Python list as a unique_ptr<Tool> backed by a
/// PyToolOwner trampoline. Defined in bind_node.cpp where the
/// PyToolOwner class itself lives (anonymous-namespace local).
/// GraphEngine.compile() in bind_graph.cpp uses this to materialize
/// the `_pytools` attribute attached to the Python NodeContext wrapper.
std::vector<std::unique_ptr<neograph::Tool>>
wrap_python_tools(py::handle tools_list);

} // namespace neograph::pybind
