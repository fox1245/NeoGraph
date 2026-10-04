// Opaque vector type registrations.
//
// These vector properties on ChatMessage and NodeResult support live mutation.
// The default stl caster instead returns copies, so appending to a property
// would mutate only a temporary. SDK provider messages intentionally use
// detached snapshots: build a list, then assign request.messages or parts.
//
// PYBIND11_MAKE_OPAQUE + py::bind_vector together replace the copy
// caster with a true Python wrapper class that holds a reference into
// the C++ vector. ``.append()``, ``__setitem__``, etc. all push through
// to the live std::vector; ``=[…]`` assignment still works because
// bind_vector's class accepts iterables in its constructor.
//
// Discipline:
//   - This header MUST be included BEFORE ``pybind11/stl.h`` in every
//     translation unit that uses these vector types via pybind11.
//     Otherwise stl.h's auto-caster registers first and the OPAQUE
//     declarations are silently shadowed (well, ODR-violated: same
//     type, different casters across TUs → undefined behaviour).
//   - ``std::vector<std::string>`` is deliberately NOT made opaque.
//     It's used in many places across the binding (channel names,
//     node names, debug strings) and making it opaque would require
//     either binding it as a public Python class or adjusting many
//     call sites. The .append() trap on ``ChatMessage.image_urls``
//     (the only ``vector<string>`` exposed via def_readwrite) is left
//     as a documented limitation — that field is rarely used and the
//     workaround (build-then-assign) is in the binding's docstring.

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl_bind.h>

#include <neograph/graph/types.h>
#include <neograph/types.h>

#include <vector>

PYBIND11_MAKE_OPAQUE(std::vector<neograph::ChatMessage>);
PYBIND11_MAKE_OPAQUE(std::vector<neograph::ChatTool>);
PYBIND11_MAKE_OPAQUE(std::vector<neograph::ToolCall>);
PYBIND11_MAKE_OPAQUE(std::vector<neograph::graph::ChannelWrite>);
PYBIND11_MAKE_OPAQUE(std::vector<neograph::graph::Send>);
