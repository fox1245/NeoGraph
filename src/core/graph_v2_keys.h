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
#include <map>
#include <optional>
#include <string_view>
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

/// #364 failure routing keys of one definition.
struct FailureRoutingKeys {
    std::map<std::string, NodeRetryPolicySpec> node_retry_policies;
    std::vector<FailureEdge>                   failure_edges;
    std::map<std::string, FailureExit>         failure_exits;
};

/// The NG codes the failure classifier produces; the `NG.` prefix is reserved.
NEOGRAPH_API bool is_known_failure_code(std::string_view code) noexcept;

/**
 * Parse and check "node_retry_policies", "failure_edges" and "failure_exits"
 * of a strict definition. Appends GC_FIELD_TYPE, GC_FIELD_REQUIRED,
 * GC_FIELD_VALUE (bounds as {min, max, actual}; codes), GC_UNKNOWN_FIELD and
 * GC_FAILURE_EDGE ({from, to, reason}: sentinel_source, duplicate_from,
 * sentinel_target, self_target, join_target, implicit_end_target,
 * error_channel_reducer). Unknown node and channel names are left to the
 * validator (E3). The returned keys are canonical; a definition that already
 * is canonical prints back to the same bytes.
 */
NEOGRAPH_API FailureRoutingKeys parse_failure_routing_keys(
    const json& definition, std::vector<CompilerDiagnostic>& diagnostics);

/// The one printer of the three keys; each is null when unused.
NEOGRAPH_API json print_node_retry_policies(const std::map<std::string, NodeRetryPolicySpec>& policies);
NEOGRAPH_API json print_failure_edges(const std::vector<FailureEdge>& edges);
NEOGRAPH_API json print_failure_exits(const std::map<std::string, FailureExit>& exits);

}  // namespace neograph::graph::detail
