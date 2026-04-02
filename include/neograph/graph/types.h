#pragma once

#include <neograph/types.h>
#include <neograph/provider.h>
#include <neograph/tool.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace neograph::graph {

// Forward declaration
class GraphState;

// --- Reducer ---
enum class ReducerType { OVERWRITE, APPEND, CUSTOM };

using ReducerFn = std::function<json(const json& current, const json& incoming)>;

// --- Channel ---
struct Channel {
    std::string name;
    ReducerType reducer_type = ReducerType::OVERWRITE;
    ReducerFn   reducer;
    json        value;
    uint64_t    version = 0;
};

// --- Channel Write (output of a node) ---
struct ChannelWrite {
    std::string channel;
    json        value;
};

// --- Edges ---
struct Edge {
    std::string from;
    std::string to;
};

struct ConditionalEdge {
    std::string from;
    std::string condition;                       // name in ConditionRegistry
    std::map<std::string, std::string> routes;   // condition_result -> node_name
};

// --- Node Context (dependency injection for nodes) ---
struct NodeContext {
    std::shared_ptr<Provider> provider;
    std::vector<Tool*>        tools;   // non-owning; engine owns the unique_ptrs
    std::string               model;
    std::string               instructions;
    json                      extra_config;
};

// --- Graph Events (for streaming) ---
struct GraphEvent {
    enum class Type {
        NODE_START,
        NODE_END,
        LLM_TOKEN,
        CHANNEL_WRITE,
        INTERRUPT,
        ERROR
    };
    Type        type;
    std::string node_name;
    json        data;
};

using GraphStreamCallback = std::function<void(const GraphEvent&)>;

// --- Condition function ---
using ConditionFn = std::function<std::string(const GraphState&)>;

// --- Special node names ---
constexpr const char* START_NODE = "__start__";
constexpr const char* END_NODE   = "__end__";

} // namespace neograph::graph
