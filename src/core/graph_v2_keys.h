/**
 * @file graph_v2_keys.h
 * @brief Internal: structural rules of the Program API v2 Core keys.
 *
 * The Core parser and the sealed-definition validator of Program bundles
 * (validate_core_definition_v1) must apply the same rules, so they live here
 * once. Internal header: it is not installed and carries no compatibility
 * promise; the functions are exported only for other NeoGraph libraries.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/compiler.h>

#include <cstddef>
#include <vector>

namespace neograph::graph::detail {

/// #362 limits on the "metadata" object.
inline constexpr std::size_t kMetadataMaxBytes   = 8 * 1024;
inline constexpr std::size_t kMetadataMaxDepth   = 4;
inline constexpr std::size_t kMetadataMaxMembers = 64;
inline constexpr std::size_t kMetadataMaxKeyBytes = 128;

/**
 * Append every rule `metadata` breaks, with pointers under "/metadata":
 * GC_FIELD_TYPE (not an object), GC_METADATA_LIMIT (canonical bytes, depth,
 * total members, key length), GC_METADATA_TEXT (an invisible character in a
 * key or string) and GC_FIELD_VALUE (an empty or reserved key).
 */
NEOGRAPH_API void append_metadata_diagnostics(const json&                      metadata,
                                              std::vector<CompilerDiagnostic>& diagnostics);

}  // namespace neograph::graph::detail
