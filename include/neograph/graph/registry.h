#pragma once

#include <neograph/api.h>
#include <neograph/graph/loader.h>

#include <optional>
#include <string>
#include <unordered_map>

namespace neograph::graph {

/**
 * @brief Per-engine registry overlay with process-global fallback.
 *
 * Entries registered here take precedence over the legacy singleton
 * registries. Missing entries fall back to ReducerRegistry,
 * ConditionRegistry, and NodeFactory, so built-ins and existing global
 * registrations remain available. Configure a registry before passing it to
 * EngineResources; runtime mutation is intentionally unsupported by contract.
 */
class NEOGRAPH_API GraphRegistry {
public:
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

private:
    std::unordered_map<std::string, ReducerFn>     reducers_;
    std::unordered_map<std::string, ConditionFn>   conditions_;
    std::unordered_map<std::string, ConditionSpec> condition_specs_;
    std::unordered_map<std::string, NodeFactoryFn> node_factories_;
    std::unordered_map<std::string, json>          node_schemas_;
    std::unordered_map<std::string, json>          node_effects_;
};

}  // namespace neograph::graph
