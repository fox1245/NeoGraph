/**
 * @file graph/loader.h
 * @brief Singleton registries for custom reducers, conditions, and node types.
 *
 * These registries are used during graph compilation (GraphEngine::compile)
 * to resolve JSON-defined reducer names, condition names, and node type names
 * into their corresponding functions and factories.
 *
 * Built-in entries are registered automatically. Users can add custom entries
 * before compiling a graph.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/types.h>
#include <unordered_map>
#include <memory>

namespace neograph::graph {

class GraphNode;

/**
 * @brief Singleton registry for channel reducer functions.
 *
 * Maps reducer names (used in JSON graph definitions) to ReducerFn
 * implementations. Built-in reducers: "overwrite", "append".
 *
 * @code
 * ReducerRegistry::instance().register_reducer("sum",
 *     [](const json& current, const json& incoming) {
 *         return current.get<int>() + incoming.get<int>();
 *     });
 * @endcode
 */
/// @note Process-wide singleton.
///
/// `ReducerRegistry`, `ConditionRegistry`, and `NodeFactory` are global
/// state shared across every GraphEngine in the process. This is fine
/// for most embeddings (a single host process compiling its graphs
/// once), but it has caveats:
///   - Two embedders coexisting in the same process can't register
///     conflicting reducer/condition/node-type names without stepping
///     on each other.
///   - Test isolation: the registries persist across test cases, so
///     a node-type registered in one test is visible in subsequent
///     tests; tests must use unique type names or deregister.
///   - Pybind users: state survives pytest-case boundaries.
///
/// A future major version (v1.0) is expected to thread per-engine
/// `Registry` instances through `NodeContext`/`compile()`, with the
/// global singleton kept as a default-fallback layer. Until then,
/// avoid duplicate registrations across test cases / embedders.
class NEOGRAPH_API ReducerRegistry {
public:
    /// @brief Get the singleton instance.
    /// @return Reference to the global ReducerRegistry.
    static ReducerRegistry& instance();

    /**
     * @brief Register a named reducer function.
     * @param name Reducer name (referenced in JSON channel definitions).
     * @param fn Reducer function that merges current and incoming values.
     */
    void register_reducer(const std::string& name, ReducerFn fn);

    /**
     * @brief Look up a reducer by name.
     * @param name Reducer name.
     * @return The ReducerFn, or throws if not found.
     */
    ReducerFn get(const std::string& name) const;

private:
    ReducerRegistry();
    std::unordered_map<std::string, ReducerFn> registry_;
};

/**
 * @brief Singleton registry for conditional edge routing functions.
 *
 * Maps condition names (used in JSON edge definitions) to ConditionFn
 * implementations. Built-in conditions: "has_tool_calls", "route_channel".
 *
 * @code
 * ConditionRegistry::instance().register_condition("is_long",
 *     [](const GraphState& state) -> std::string {
 *         return state.get("messages").size() > 10 ? "true" : "false";
 *     });
 * @endcode
 */
class NEOGRAPH_API ConditionRegistry {
public:
    /// @brief Get the singleton instance.
    /// @return Reference to the global ConditionRegistry.
    static ConditionRegistry& instance();

    /**
     * @brief Register a named condition function.
     * @param name Condition name (referenced in JSON edge definitions).
     * @param fn Condition function that evaluates state and returns a route key.
     */
    void register_condition(const std::string& name, ConditionFn fn);

    /**
     * @brief Look up a condition by name.
     * @param name Condition name.
     * @return The ConditionFn, or throws if not found.
     */
    ConditionFn get(const std::string& name) const;

private:
    ConditionRegistry();
    std::unordered_map<std::string, ConditionFn> registry_;
};

/**
 * @brief Factory function signature for creating graph nodes.
 * @param name Node name within the graph.
 * @param config Node-specific configuration from the JSON definition.
 * @param ctx Node context with provider, tools, and model.
 * @return A unique_ptr to the created GraphNode.
 */
using NodeFactoryFn = std::function<std::unique_ptr<GraphNode>(
    const std::string& name,
    const json& config,
    const NodeContext& ctx)>;

/**
 * @brief Singleton factory for creating graph nodes by type name.
 *
 * Maps node type names (used in JSON node definitions) to factory
 * functions. Built-in types: "llm_call", "tool_dispatch",
 * "intent_classifier", "subgraph".
 *
 * @code
 * NodeFactory::instance().register_type("my_custom_node",
 *     [](const std::string& name, const json& config, const NodeContext& ctx) {
 *         return std::make_unique<MyCustomNode>(name, ctx);
 *     });
 * @endcode
 */
class NEOGRAPH_API NodeFactory {
public:
    /// @brief Get the singleton instance.
    /// @return Reference to the global NodeFactory.
    static NodeFactory& instance();

    /**
     * @brief Register a custom node type.
     * @param type Node type name (referenced in JSON node definitions).
     * @param fn Factory function that creates the node.
     */
    void register_type(const std::string& type, NodeFactoryFn fn);

    /**
     * @brief Create a node by type name.
     * @param type Node type name.
     * @param name Unique node name within the graph.
     * @param config Node-specific configuration from JSON.
     * @param ctx Node context with provider, tools, and model.
     * @return A unique_ptr to the created GraphNode, or throws if type not found.
     */
    std::unique_ptr<GraphNode> create(const std::string& type,
                                       const std::string& name,
                                       const json& config,
                                       const NodeContext& ctx) const;

private:
    NodeFactory();
    std::unordered_map<std::string, NodeFactoryFn> registry_;
};

} // namespace neograph::graph
