#pragma once

namespace neograph::graph::detail {

// Added only after translation validation to preserve schema-v0 behavior.
// Declarative topology, schema export, and to_json() must never expose it.
inline constexpr const char* kLegacyDefaultRoute =
    "\x1fneograph-legacy-default";

} // namespace neograph::graph::detail
