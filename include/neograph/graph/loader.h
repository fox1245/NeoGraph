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
#include <optional>
#include <string>
#include <vector>

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
/// New code that needs isolation can register an EngineResources
/// `GraphRegistry`; entries there take precedence and this singleton remains
/// the built-in/default fallback layer.
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

    /**
     * @brief List all registered reducer names, sorted.
     *
     * Introspection accessor for external tooling (e.g. a visual
     * topology editor) that needs to enumerate the available reducer
     * palette without grepping engine source.
     * @return Sorted vector of reducer names.
     */
    std::vector<std::string> names() const;

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
/**
 * @brief Declared output-label contract of a condition function.
 *
 * Backs static route-completeness checking (GraphValidator E10): a
 * conditional edge over a *closed* condition must map exactly the
 * declared labels — an unmapped label would fall into the scheduler's
 * lexicographically-last-route fallback (an arbitrary target), and a
 * route key outside the label set is dead. Conditions registered
 * without a spec are skipped by the validator.
 */
struct ConditionSpec {
    /// Labels the condition is known to return ("known" for open specs,
    /// "exactly these" for closed specs).
    std::vector<std::string> labels;
    /// True when the condition can return values outside `labels`
    /// (e.g. route_channel returns arbitrary channel content) — the
    /// validator then only warns about uncovered *known* labels.
    bool open = false;
};

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
     * @brief Register a named condition function with a declared
     *        output-label contract (enables E10 route-completeness
     *        checking on conditional edges using this condition).
     */
    void register_condition(const std::string& name, ConditionFn fn,
                            ConditionSpec spec);

    /**
     * @brief Look up a condition by name.
     * @param name Condition name.
     * @return The ConditionFn, or throws if not found.
     */
    ConditionFn get(const std::string& name) const;

    /**
     * @brief Declared label contract for a condition, if any.
     * @return The ConditionSpec, or std::nullopt for conditions
     *         registered without one (validator skips those).
     */
    std::optional<ConditionSpec> condition_spec(const std::string& name) const;

    /**
     * @brief List all registered condition names, sorted.
     *
     * Introspection accessor for external tooling (e.g. a visual
     * topology editor) that needs to enumerate the available
     * conditional-routing palette.
     * @return Sorted vector of condition names.
     */
    std::vector<std::string> names() const;

private:
    ConditionRegistry();
    std::unordered_map<std::string, ConditionFn> registry_;
    std::unordered_map<std::string, ConditionSpec> specs_;
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
     *
     * The node's config schema defaults to a permissive
     * `{"type":"object"}` (any config object accepted). Existing
     * callers keep working unchanged; external tooling that consumes
     * export_schema() will simply render a free-form config for such
     * a type. To declare a concrete config schema, use the 3-arg
     * overload below.
     */
    void register_type(const std::string& type, NodeFactoryFn fn);

    /**
     * @brief Register a custom node type with a declared config schema.
     * @param type Node type name (referenced in JSON node definitions).
     * @param fn Factory function that creates the node.
     * @param config_schema JSON Schema (Draft 2020-12) fragment
     *        describing this node type's accepted `config` fields.
     *        Used only by export_schema() for external tooling; the
     *        engine does not validate config against it at compile
     *        time. Additive: callers using the 2-arg overload are
     *        unaffected.
     */
    void register_type(const std::string& type, NodeFactoryFn fn,
                       json config_schema);

    /**
     * @brief Register a custom node type with a declared config schema
     *        AND a declared channel-effect contract.
     * @param effects `{"reads": ["ch", ...], "writes": ["ch", ...]}` —
     *        the channels instances of this type touch at runtime.
     *        Enables static effect checking (GraphValidator E4/E5/E6:
     *        writes to undeclared channels, overwrite races, dead
     *        channels). Effect analysis only runs on graphs where
     *        EVERY node's type declared effects — one unknown type
     *        disables it (soundness of the checker over coverage).
     */
    void register_type(const std::string& type, NodeFactoryFn fn,
                       json config_schema, json effects);

    /**
     * @brief Declared channel effects for a node type.
     * @return `{"reads":[...],"writes":[...]}`, or null json for types
     *         registered without an effect contract.
     */
    json node_effects(const std::string& type) const;

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

    /**
     * @brief List all registered node type names, sorted.
     * @return Sorted vector of node type names.
     */
    std::vector<std::string> registered_types() const;

    /**
     * @brief Declared config schema for a node type.
     *
     * Returns the schema passed to the 3-arg register_type overload,
     * or the permissive `{"type":"object"}` default for types
     * registered without one (never throws on unknown types — returns
     * the permissive default; create() is where unknown types fail).
     * Backs strict-mode consumed-key accounting in GraphCompiler and
     * per-type introspection for external tooling.
     */
    json config_schema(const std::string& type) const;

    /**
     * @brief Export a machine-readable description of the topology
     *        JSON format this engine accepts.
     *
     * Intended for external tooling — notably a visual block-coding
     * topology editor — so the editor's node palette and the engine
     * cannot drift apart across NeoGraph versions. The returned
     * document is JSON, shaped as:
     *
     * @code
     * {
     *   "neograph_version": "0.6.0",
     *   "$schema": "https://json-schema.org/draft/2020-12/schema",
     *   "topology": { ...JSON Schema for the top-level envelope... },
     *   "node_types": { "<type>": { ...config JSON Schema... }, ... },
     *   "reducers":   ["append", "overwrite", ...],
     *   "conditions": ["has_tool_calls", "route_channel", ...]
     * }
     * @endcode
     *
     * `neograph_version` lets a tool detect when its cached schema
     * is older than the engine and warn the user. The `topology`
     * fragment is fixed (defined by the graph compiler); per-type
     * config schemas come from register_type's 3-arg overload, or
     * default to a permissive object for types registered without one.
     *
     * @return The schema document as json.
     */
    json export_schema() const;

private:
    NodeFactory();
    std::unordered_map<std::string, NodeFactoryFn> registry_;
    std::unordered_map<std::string, json> schemas_;
    std::unordered_map<std::string, json> effects_;
};

} // namespace neograph::graph
