/**
 * @file types.h
 * @brief Graph engine type definitions: channels, edges, nodes, events, and control flow.
 *
 * Contains all the foundational types used by the graph execution engine,
 * including reducers, channels, edges, Send/Command control flow,
 * retry policies, stream modes, and graph events.
 */
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

// Windows SDK (pulled in transitively via asio on Win32) defines a
// bunch of ALL-CAPS macros — notably ERROR (wingdi.h) and DEBUG —
// that silently clobber enumerator names. `enum class Type { ERROR }`
// below becomes `enum class Type { 0 }` and every downstream parse
// collapses. Scrub the ones we actually use before declaring the
// enums. No-op outside of Windows.
#ifdef _WIN32
#  ifdef ERROR
#    undef ERROR
#  endif
#  ifdef DEBUG
#    undef DEBUG
#  endif
#  ifdef IN
#    undef IN
#  endif
#  ifdef OUT
#    undef OUT
#  endif
#endif

namespace neograph::graph {

// Forward declaration
class GraphState;

/**
 * @brief Strategy for merging values when writing to a channel.
 */
enum class ReducerType {
    OVERWRITE,  ///< Replace the current value entirely.
    APPEND,     ///< Append to the current value (arrays are concatenated).
    CUSTOM      ///< Use a custom reducer function.
};

/**
 * @brief Custom reducer function signature.
 * @param current The current value in the channel.
 * @param incoming The new value being written.
 * @return The merged result.
 */
using ReducerFn = std::function<json(const json& current, const json& incoming)>;

/**
 * @brief A state channel that holds a value with a reducer and version tracking.
 *
 * Channels are the primary state storage mechanism in the graph engine.
 * Each channel has a name, a reducer that controls how new values are
 * merged with existing ones, and a version counter.
 */
struct Channel {
    std::string name;                                ///< Channel name.
    ReducerType reducer_type = ReducerType::OVERWRITE; ///< Merge strategy.
    ReducerFn   reducer;                             ///< Custom reducer (when type == CUSTOM).
    json        value;                               ///< Current channel value.
    uint64_t    version = 0;                         ///< Version counter (incremented on each write).
};

/**
 * @brief Output of a node: a write to a named channel.
 */
struct ChannelWrite {
    std::string channel;  ///< Target channel name.
    json        value;    ///< Value to write through the channel's reducer.
};

/**
 * @brief Exception thrown from inside a node to trigger a dynamic breakpoint.
 *
 * When a node throws NodeInterrupt, the graph engine saves a checkpoint
 * and pauses execution, allowing Human-in-the-Loop (HITL) intervention.
 * Execution can be resumed with GraphEngine::resume().
 *
 * @code
 * throw NodeInterrupt("Need human approval for this action");
 * @endcode
 */
class NodeInterrupt : public std::runtime_error {
public:
    /// @brief Construct a NodeInterrupt with a reason message.
    /// @param reason Human-readable explanation of why the interrupt was triggered.
    explicit NodeInterrupt(const std::string& reason)
        : std::runtime_error(reason), reason_(reason) {}

    /// @brief Get the interrupt reason.
    /// @return The reason string provided at construction.
    const std::string& reason() const { return reason_; }
private:
    std::string reason_;
};

/**
 * @brief Dynamic fan-out request (map-reduce pattern).
 *
 * A node can return Send objects to dispatch the same (or different) node
 * multiple times with different inputs. All Send targets execute in parallel
 * via Taskflow.
 *
 * ## Isolation semantics
 *
 * Each multi-Send worker runs on an **isolated GraphState copy**: channel
 * writes made inside a Send target are scoped to that worker and only
 * merged back (via each channel's reducer) after all siblings finish.
 * Concurrent workers therefore cannot corrupt each other's channel view.
 *
 * This isolation does **NOT** extend to out-of-state side effects. If two
 * Send workers call the same LLM provider instance, hit the same HTTP
 * client, or write to the same file descriptor, they do so concurrently
 * — whatever thread safety those external objects offer is what governs
 * the outcome. Make sure any resource a Send target touches is either
 * thread-safe or scoped per-invocation. (NeoGraph's own `SchemaProvider`
 * and `OpenAIProvider` are both safe for concurrent use.)
 *
 * @code
 * NodeResult result;
 * for (auto& item : work_items) {
 *     result.sends.push_back(Send{"worker_node", {{"item", item}}});
 * }
 * @endcode
 */
struct Send {
    std::string target_node;  ///< Node to dispatch.
    json        input;        ///< Channel writes for that invocation.
};

/**
 * @brief Combined routing override + state update.
 *
 * A node returns a Command to simultaneously update state AND control
 * the next node to execute, overriding normal edge-based routing.
 *
 * @code
 * NodeResult result;
 * result.command = Command{"specific_node", {{"status", "approved"}}};
 * @endcode
 */
struct Command {
    std::string                goto_node;  ///< Next node to execute (overrides edge routing).
    std::vector<ChannelWrite>  updates;    ///< State updates to apply before routing.
};

/**
 * @brief Retry policy for node execution with exponential backoff.
 *
 * When a node fails, the engine retries according to this policy.
 * Delay between retries grows exponentially: delay = initial_delay_ms * backoff_multiplier^attempt,
 * capped at max_delay_ms.
 */
struct RetryPolicy {
    int    max_retries      = 0;       ///< Maximum retry attempts. 0 = no retry.
    int    initial_delay_ms = 100;     ///< Delay before the first retry (milliseconds).
    float  backoff_multiplier = 2.0f;  ///< Multiplier applied to delay after each retry.
    int    max_delay_ms     = 5000;    ///< Maximum delay cap (milliseconds).
};

/**
 * @brief Bitfield flags for selecting which events to stream during execution.
 *
 * Combine with bitwise OR to select multiple event types:
 * @code
 * StreamMode mode = StreamMode::EVENTS | StreamMode::TOKENS;
 * @endcode
 */
enum class StreamMode : uint8_t {
    EVENTS   = 0x01,  ///< NODE_START, NODE_END, INTERRUPT, ERROR events.
    TOKENS   = 0x02,  ///< LLM_TOKEN events (streaming tokens).
    VALUES   = 0x04,  ///< Full state snapshot after each super-step.
    UPDATES  = 0x08,  ///< Channel write deltas per node.
    DEBUG    = 0x10,  ///< Internal debug info (retry attempts, routing decisions).
    ALL      = 0xFF   ///< All event types.
};

/// @brief Bitwise OR for StreamMode flags.
/// @param a Left operand.
/// @param b Right operand.
/// @return Combined StreamMode flags.
inline StreamMode operator|(StreamMode a, StreamMode b) {
    return static_cast<StreamMode>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
/// @brief Bitwise AND for StreamMode flags.
/// @param a Left operand.
/// @param b Right operand.
/// @return Intersection of StreamMode flags.
inline StreamMode operator&(StreamMode a, StreamMode b) {
    return static_cast<StreamMode>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
/// @brief Check if a specific stream mode flag is set.
/// @param flags The combined StreamMode bitfield.
/// @param test The flag to test for.
/// @return True if the flag is set.
inline bool has_mode(StreamMode flags, StreamMode test) {
    return static_cast<uint8_t>(flags & test) != 0;
}

/**
 * @brief A static edge connecting two nodes.
 */
struct Edge {
    std::string from;  ///< Source node name.
    std::string to;    ///< Destination node name.
};

/**
 * @brief A conditional edge with runtime routing based on state.
 *
 * The condition function is looked up in ConditionRegistry by name,
 * and its return value is matched against the routes map.
 */
struct ConditionalEdge {
    std::string from;                                ///< Source node name.
    std::string condition;                           ///< Condition function name in ConditionRegistry.
    std::map<std::string, std::string> routes;       ///< Mapping of condition result to destination node.
};

/**
 * @brief Dependency injection context passed to nodes during construction.
 *
 * Provides the LLM provider, available tools, model name, and any
 * extra configuration a node might need.
 */
struct NodeContext {
    std::shared_ptr<Provider> provider;    ///< LLM provider for making completions.
    /// Non-owning tool pointers consumed by node factories at compile()
    /// time. **Lifetime contract**: the pointees must outlive the
    /// GraphEngine. Typical pattern: hand the engine ownership via
    /// `engine.own_tools(std::move(unique_ptr_vec))` after compile, or
    /// keep the owning unique_ptrs alive in the caller's scope for at
    /// least as long as the engine. Constructing NodeContext from a
    /// throwaway unique_ptr collection that goes out of scope before
    /// `engine->run()` returns leaves these pointers dangling and is UB.
    std::vector<Tool*>        tools;
    std::string               model;       ///< Model name override (empty = use provider default).
    std::string               instructions; ///< System prompt / instructions for the LLM.
    json                      extra_config; ///< Additional configuration (node-type specific).
};

/**
 * @brief An event emitted during graph execution for streaming.
 */
struct GraphEvent {
    /// Event type enumeration.
    enum class Type {
        NODE_START,     ///< A node began execution.
        NODE_END,       ///< A node finished execution.
        LLM_TOKEN,      ///< A token was received from the LLM.
        CHANNEL_WRITE,  ///< A value was written to a channel.
        INTERRUPT,      ///< Execution was interrupted (HITL).
        ERROR           ///< An error occurred during execution.
    };
    Type        type;       ///< The event type.
    std::string node_name;  ///< Name of the node that generated this event.
    json        data;       ///< Event-specific data payload.
};

/// Callback for receiving graph execution events.
/// @param event The graph event to process.
using GraphStreamCallback = std::function<void(const GraphEvent&)>;

/**
 * @brief Extended result returned by node execution.
 *
 * Wraps channel writes with optional Command (routing override) and
 * Send (dynamic fan-out) directives. Backward-compatible: constructing
 * from a plain vector of ChannelWrite produces a NodeResult with no
 * Command or Send.
 */
struct NodeResult {
    std::vector<ChannelWrite> writes;          ///< Channel writes from this node.
    std::optional<Command>    command;          ///< If set, overrides edge-based routing.
    std::vector<Send>         sends;           ///< If non-empty, triggers dynamic fan-out.

    NodeResult() = default;

    /// @brief Construct from plain channel writes (backward compatible).
    /// @param w Vector of channel writes.
    NodeResult(std::vector<ChannelWrite> w) : writes(std::move(w)) {}
};

/**
 * @brief Condition function signature for conditional edge routing.
 * @param state The current graph state to evaluate.
 * @return A string key that is matched against ConditionalEdge::routes.
 */
using ConditionFn = std::function<std::string(const GraphState&)>;

/// Special node name representing the graph entry point.
constexpr const char* START_NODE = "__start__";
/// Special node name representing the graph exit point.
constexpr const char* END_NODE   = "__end__";

} // namespace neograph::graph
