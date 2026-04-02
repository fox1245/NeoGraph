#pragma once

#include <neograph/graph/types.h>
#include <unordered_map>
#include <memory>

namespace neograph::graph {

class GraphNode;

// --- Reducer Registry (singleton) ---
class ReducerRegistry {
public:
    static ReducerRegistry& instance();

    void register_reducer(const std::string& name, ReducerFn fn);
    ReducerFn get(const std::string& name) const;

private:
    ReducerRegistry();
    std::unordered_map<std::string, ReducerFn> registry_;
};

// --- Condition Registry (singleton) ---
class ConditionRegistry {
public:
    static ConditionRegistry& instance();

    void register_condition(const std::string& name, ConditionFn fn);
    ConditionFn get(const std::string& name) const;

private:
    ConditionRegistry();
    std::unordered_map<std::string, ConditionFn> registry_;
};

// --- Node Factory (singleton) ---
using NodeFactoryFn = std::function<std::unique_ptr<GraphNode>(
    const std::string& name,
    const json& config,
    const NodeContext& ctx)>;

class NodeFactory {
public:
    static NodeFactory& instance();

    void register_type(const std::string& type, NodeFactoryFn fn);
    std::unique_ptr<GraphNode> create(const std::string& type,
                                       const std::string& name,
                                       const json& config,
                                       const NodeContext& ctx) const;

private:
    NodeFactory();
    std::unordered_map<std::string, NodeFactoryFn> registry_;
};

} // namespace neograph::graph
