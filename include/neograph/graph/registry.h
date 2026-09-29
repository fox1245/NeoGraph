#pragma once

#include <neograph/api.h>
#include <neograph/graph/loader.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace neograph::graph {

/**
 * @brief Engine-owned registry of custom graph callables.
 *
 * A new registry sees immutable built-ins, but never process-global custom
 * registrations. GlobalFallback explicitly opts into legacy singleton entries.
 * Register before build; build takes a snapshot, so later edits cannot change
 * an existing engine. Individual operations and snapshots are synchronized.
 */
class NEOGRAPH_API GraphRegistry {
public:
    enum class Fallback { BuiltinsOnly, GlobalFallback };
    explicit GraphRegistry(Fallback fallback = Fallback::BuiltinsOnly);
    GraphRegistry(GraphRegistry&& other);
    GraphRegistry& operator=(GraphRegistry&& other);
    GraphRegistry(const GraphRegistry& other);
    GraphRegistry& operator=(const GraphRegistry& other);
    std::shared_ptr<const GraphRegistry> snapshot() const;

    static const GraphRegistry& global();

    void register_reducer(const std::string& name, ReducerFn fn);
    void register_condition(const std::string& name, ConditionFn fn);
    void register_condition(const std::string& name, ConditionFn fn, ConditionSpec spec);
    void register_type(const std::string& type, NodeFactoryFn fn);
    void register_type(const std::string& type, NodeFactoryFn fn, json config_schema);
    void register_type(const std::string& type, NodeFactoryFn fn, json config_schema, json effects);

    ReducerFn                    reducer(const std::string& name) const;
    ConditionFn                  condition(const std::string& name) const;
    std::optional<ConditionSpec> condition_spec(const std::string& name) const;
    std::unique_ptr<GraphNode>   create(const std::string& type,
                                        const std::string& name,
                                        const json&        config,
                                        const NodeContext& ctx) const;
    json                         config_schema(const std::string& type) const;
    json                         node_effects(const std::string& type) const;

    /** Exact local-only resolvers; never consult process-global registries. */
    ReducerFn                    local_reducer(const std::string& name) const;
    ConditionFn                  local_condition(const std::string& name) const;
    std::optional<ConditionSpec> local_condition_spec(const std::string& name) const;
    std::unique_ptr<GraphNode>   create_local(const std::string& type,
                                              const std::string& name,
                                              const json&        config,
                                              const NodeContext& ctx) const;
    json                         local_config_schema(const std::string& type) const;
    json                         local_node_effects(const std::string& type) const;

    /** Local-only membership used by sealed admission profiles. */
    bool contains_reducer(const std::string& name) const;
    bool contains_condition(const std::string& name) const;
    bool contains_type(const std::string& type) const;

    /**
     * Export only entries owned by this registry.
     *
     * Unlike reducer()/condition()/create(), this never includes the legacy
     * process-global fallback and is therefore suitable for a sealed palette.
     */
    json export_schema() const;
    /** Export exactly the names resolvable by this registry (including defaults). */
    json export_effective_schema() const;


private:
    explicit GraphRegistry(bool builtins);
    static const GraphRegistry& builtins();
    mutable std::mutex mutex_;
    Fallback fallback_ = Fallback::BuiltinsOnly;

    std::unordered_map<std::string, ReducerFn>     reducers_;
    std::unordered_map<std::string, ConditionFn>   conditions_;
    std::unordered_map<std::string, ConditionSpec> condition_specs_;
    std::unordered_map<std::string, NodeFactoryFn> node_factories_;
    std::unordered_map<std::string, json>          node_schemas_;
    std::unordered_map<std::string, json>          node_effects_;
};

}  // namespace neograph::graph
