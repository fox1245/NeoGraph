/**
 * @file graph/node_cache.h
 * @brief Per-node result cache (opt-in).
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/types.h>

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

namespace neograph::graph {

struct RunContext;

/**
 * @brief Lifetime boundary for a cached node result.
 *
 * Execution is the safe default: results never cross a `run()` or `resume()`
 * call. Reusable is an explicit assertion that the node is pure with respect
 * to every run context dependency. A context key may further partition a
 * Reusable cache by declared, stable context identity.
 */
enum class CacheScope {
    Execution,
    Reusable,
};

/**
 * @brief Explicit cache identity policy for one node.
 *
 * `context_key` is only used with CacheScope::Reusable. It must return a
 * deterministic, non-secret logical identifier (for example a tenant ID or
 * provider/model revision), never a pointer address, token, or credential.
 * The value is retained only in the in-process cache key and is not emitted by
 * cache diagnostics.
 */
struct CacheKeyPolicy {
    CacheScope scope = CacheScope::Execution;
    std::function<std::string(const RunContext&)> context_key;
};

/**
 * @brief Per-node result cache. Opt-in per node — callers must
 * explicitly enable caching via `set_enabled(name, true)` so semantics
 * never silently change for non-deterministic nodes.
 *
 * Cache key = node name + state hash + an explicit scope key. The default
 * scope key is a unique execution ID, so cache entries never cross tenant,
 * Store, ToolGate, provider/model, resume, or other RunContext boundaries.
 * Reusable caching is an explicit policy decision. Cache
 * values are full `NodeResult`s (writes + Command + Sends), so a
 * cached hit replays the original outcome without re-running the
 * node. Hit / miss counters are exposed for tests + observability.
 *
 * Only safe for pure nodes — deterministic, no external side effects,
 * no time-dependent behavior. Streaming nodes are skipped because
 * cached hits cannot replay token events.
 *
 * Thread-safe: protected by an internal mutex. Callers should
 * remember that fan-out branches see the same cache instance, so the
 * first parallel branch to compute a (node, state) pair populates
 * the entry the rest hit.
 */
class NEOGRAPH_API NodeCache {
public:
    NodeCache() = default;

    /// Enable / disable caching for a specific node.
    void set_enabled(const std::string& node_name, bool enabled);

    /// True iff `set_enabled(node_name, true)` was the last call for it.
    bool is_enabled(const std::string& node_name) const;

    /// Set a node's cache identity policy. The default is execution-local.
    void set_policy(const std::string& node_name, CacheKeyPolicy policy);

    /// Return a node's policy, or the safe execution-local default.
    CacheKeyPolicy policy_for(const std::string& node_name) const;

    /// Lookup. nullopt if no cached result for this (node, state_hash).
    std::optional<NodeResult> lookup(const std::string& node_name,
                                     const std::string& state_hash) const;

    /// Lookup with the execution or caller-declared reusable scope key.
    std::optional<NodeResult> lookup(const std::string& node_name,
                                     const std::string& state_hash,
                                     const std::string& scope_key) const;

    /// Store the NodeResult for this (node, state_hash).
    void store(const std::string& node_name,
               const std::string& state_hash,
               NodeResult result);

    /// Store with the execution or caller-declared reusable scope key.
    void store(const std::string& node_name,
               const std::string& state_hash,
               const std::string& scope_key,
               NodeResult result);

    /// Drop all cached entries (per-node enable/disable state preserved).
    void clear();

    /// Number of entries currently held.
    std::size_t size() const;

    /// Lifetime hit count (incremented on lookup that returns a value).
    std::size_t hit_count() const;
    /// Lifetime miss count (incremented on lookup that returns nullopt
    /// for an enabled node).
    std::size_t miss_count() const;

private:
    static std::string make_key(const std::string& node_name,
                                const std::string& state_hash,
                                const std::string& scope_key);

    mutable std::mutex mu_;
    std::set<std::string> enabled_nodes_;
    std::unordered_map<std::string, CacheKeyPolicy> policies_;
    std::unordered_map<std::string, NodeResult> entries_;
    mutable std::size_t hits_   = 0;
    mutable std::size_t misses_ = 0;
};

/// Stable hash over the JSON state used as the cache-key suffix.
/// Concrete impl is FNV-1a over the canonical (sorted-key) dump —
/// fine for in-process caches where collision risk is negligible.
NEOGRAPH_API std::string hash_state_for_cache(const json& state_value);

} // namespace neograph::graph
