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
#include <optional>
#include <set>
#include <string>
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

/// #363: slots in canonical order (alternatives by UTF-8 bytes, slots by
/// their smallest source).
NEOGRAPH_API std::vector<std::set<std::string>> canonical_slots(
    std::vector<std::set<std::string>> slots);

/// True when every slot is a singleton, i.e. the spec is a legacy barrier.
NEOGRAPH_API bool legacy_slots(const std::vector<std::set<std::string>>& slots);

/// The one printer of a join's wait_for: a singleton slot prints as its name,
/// any other slot as {"any_of": [sorted names]}, in canonical slot order.
NEOGRAPH_API json print_wait_for(const std::vector<std::set<std::string>>& slots);

/// True when a wait_for array uses the v2 slot syntax (any object entry).
NEOGRAPH_API bool uses_slot_syntax(const json& wait_for);

/**
 * Parse a strict barrier's wait_for that uses slot syntax. Entries are node
 * names or {"any_of": [node names]}. Appends GC_BARRIER for an empty or
 * non-array any_of, a non-string or empty alternative, a key next to any_of,
 * a nested any_of, and a source repeated within or across slots, and returns
 * nullopt then. Otherwise returns the canonical slots.
 */
NEOGRAPH_API std::optional<std::vector<std::set<std::string>>> parse_wait_for_slots(
    const json&                      wait_for,
    const std::string&               display,
    const std::string&               pointer,
    std::vector<CompilerDiagnostic>& diagnostics);

}  // namespace neograph::graph::detail
