#pragma once

#include <neograph/types.h>
#include <neograph/provider.h>
#include <neograph/tool.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
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

// --- NodeInterrupt: throw from inside a node to trigger dynamic breakpoint ---
class NodeInterrupt : public std::runtime_error {
public:
    explicit NodeInterrupt(const std::string& reason)
        : std::runtime_error(reason), reason_(reason) {}
    const std::string& reason() const { return reason_; }
private:
    std::string reason_;
};

// --- Send: dynamic fan-out request (map-reduce pattern) ---
// A node can return Send objects to dispatch the same (or different) node
// multiple times with different inputs.
struct Send {
    std::string target_node;
    json        input;     // channel writes for that invocation
};

// --- Command: combined routing + state update ---
// A node returns a Command to simultaneously update state AND control routing.
struct Command {
    std::string                goto_node;   // next node (overrides edge routing)
    std::vector<ChannelWrite>  updates;     // state updates to apply
};

// --- Retry policy for node execution ---
struct RetryPolicy {
    int    max_retries      = 0;       // 0 = no retry
    int    initial_delay_ms = 100;     // first retry delay
    float  backoff_multiplier = 2.0f;  // exponential backoff
    int    max_delay_ms     = 5000;    // cap
};

// --- Stream mode flags (bitfield) ---
enum class StreamMode : uint8_t {
    EVENTS   = 0x01,   // NODE_START, NODE_END, INTERRUPT, ERROR
    TOKENS   = 0x02,   // LLM_TOKEN
    VALUES   = 0x04,   // full state after each step
    UPDATES  = 0x08,   // channel writes (deltas) per node
    DEBUG    = 0x10,    // internal debug info (retry attempts, routing decisions)
    ALL      = 0xFF
};

inline StreamMode operator|(StreamMode a, StreamMode b) {
    return static_cast<StreamMode>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline StreamMode operator&(StreamMode a, StreamMode b) {
    return static_cast<StreamMode>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
inline bool has_mode(StreamMode flags, StreamMode test) {
    return static_cast<uint8_t>(flags & test) != 0;
}

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

// --- Node Result: what a node returns ---
// Wraps writes + optional Command/Send directives.
struct NodeResult {
    std::vector<ChannelWrite> writes;
    std::optional<Command>    command;      // if set, overrides edge routing
    std::vector<Send>         sends;        // if non-empty, dynamic fan-out

    // Convenience: construct from plain writes (backward compatible)
    NodeResult() = default;
    NodeResult(std::vector<ChannelWrite> w) : writes(std::move(w)) {}
};

// --- Condition function ---
using ConditionFn = std::function<std::string(const GraphState&)>;

// --- Special node names ---
constexpr const char* START_NODE = "__start__";
constexpr const char* END_NODE   = "__end__";

} // namespace neograph::graph
