/**
 * @file types.h
 * @brief Graph engine type definitions: channels, edges, nodes, events, and control flow.
 *
 * Contains all the foundational types used by the graph execution engine,
 * including reducers, channels, edges, Send/Command control flow,
 * retry policies, stream modes, and graph events.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/provider.h>
#include <neograph/tool.h>
#include <neograph/types.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

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
    /// What the write means for the channel's current value (issue #91).
    enum class Mode {
        /// Feed the value through the channel's reducer. The default, and what
        /// every write did before this existed.
        Reduce,
        /// Replace the channel's value outright, ignoring the reducer. The only
        /// way to shrink an accumulating channel — trim a conversation history,
        /// drop a poisoned message, reset a scratchpad.
        Overwrite,
    };

    std::string channel;              ///< Target channel name.
    json        value;                ///< Value to write.
    Mode        mode = Mode::Reduce;  ///< Default preserves the pre-#91 behavior.
};

/**
 * @brief Exception thrown from inside a node to trigger a dynamic breakpoint.
 *
 * When a node throws NodeInterrupt, the graph engine saves a checkpoint and
 * pauses execution, allowing Human-in-the-Loop (HITL) intervention. Execution
 * continues with GraphEngine::resume(), whose @c resume_value comes back to
 * the node as RunContext::resume_value.
 *
 * Unlike @c interrupt_before / @c interrupt_after in the graph definition,
 * which pause at a node chosen when the graph was written, this pauses on a
 * decision the node makes about what it is holding — which is what an
 * approval prompt actually needs:
 *
 * @code
 * // Out: why we stopped, and what needs approving.
 * throw NodeInterrupt("shell command needs approval",
 *                     json{{"tool", "shell"}, {"cmd", "rm -rf build/"}});
 *
 * // In (on the resumed call): what the human said.
 * if (in.ctx.resume_value->value("approved", false)) { ... }
 * @endcode
 *
 * The caller sees the reason at @c RunResult::interrupt_value["reason"], the
 * payload at @c ["value"], and the node that paused at
 * @c RunResult::interrupt_node.
 */
class NEOGRAPH_API NodeInterrupt : public std::runtime_error {
public:
    /// @brief Construct a NodeInterrupt with a reason message.
    /// @param reason Human-readable explanation of why the interrupt was triggered.
    explicit NodeInterrupt(const std::string& reason)
        : std::runtime_error(reason), reason_(reason) {}

    /// @brief Construct a NodeInterrupt carrying a structured payload.
    /// @param reason Human-readable explanation, for logs and for humans.
    /// @param value  Machine-readable payload the caller can branch on
    ///               without parsing prose (which tool, which arguments, ...).
    NodeInterrupt(const std::string& reason, json value)
        : std::runtime_error(reason), reason_(reason),
          value_(std::move(value)) {}

    /// @brief Get the interrupt reason.
    /// @return The reason string provided at construction.
    const std::string& reason() const { return reason_; }

    /// @brief Get the structured payload.
    /// @return The payload, or a null json when the node attached none.
    const json& value() const { return value_; }

    /// @brief The node that threw this interrupt.
    ///
    /// Stamped by the executor, which is the only layer that knows the name —
    /// a node body does not know what the graph definition called it. Empty
    /// on an interrupt that has not yet passed through the executor.
    const std::string& node() const { return node_; }

    /// @brief Record the throwing node's name. Called by the executor.
    void set_node(std::string node) { node_ = std::move(node); }

private:
    std::string reason_;
    json        value_;
    std::string node_;
};

/**
 * @brief A node failure annotated by the executor with dispatch context.
 *
 * The original exception remains available through @c cause() and can be
 * rethrown with @c std::rethrow_exception. Control-flow exceptions such as
 * NodeInterrupt and CancelledException are never wrapped.
 */
class NEOGRAPH_API NodeExecutionError : public std::runtime_error {
public:
    NodeExecutionError(std::string node_name, int attempts,
                       std::exception_ptr cause)
        : std::runtime_error(make_message(node_name, attempts, cause)),
          node_name_(std::move(node_name)), attempts_(attempts),
          cause_(std::move(cause)) {}

    const std::string& node_name() const noexcept { return node_name_; }
    int attempts() const noexcept { return attempts_; }
    const std::exception_ptr& cause() const noexcept { return cause_; }

private:
    static std::string make_message(const std::string& node_name, int attempts,
                                    const std::exception_ptr& cause) {
        std::string detail = "unknown exception";
        try {
            if (cause) std::rethrow_exception(cause);
        } catch (const std::exception& e) {
            detail = e.what();
        } catch (...) {}
        return "Node '" + node_name + "' failed after "
            + std::to_string(attempts) + " attempt(s): " + detail;
    }

    std::string        node_name_;
    int                attempts_;
    std::exception_ptr cause_;
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
    /// Random jitter as a fraction of the computed delay, in [0, 1].
    /// The actual sleep is `delay * (1 + uniform(-jitter_pct, +jitter_pct))`.
    /// Default 0.0 = deterministic backoff (back-compat). Set to ~0.2 in
    /// fan-out scenarios where N parallel branches hit the same upstream
    /// rate-limit; without jitter they all retry in lockstep, producing
    /// thundering-herd 429 second waves.
    float  jitter_pct       = 0.0f;
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
    /// GraphEngine. New code should pass an owned ToolSet through
    /// EngineResources; compatibility code can call
    /// `engine.own_tools(std::move(unique_ptr_vec))` after compile, or keep the
    /// owning unique_ptrs alive in the caller's scope for at least as long as
    /// the engine. Constructing NodeContext from a
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

/// Typed view of a node-start event. `data` preserves extension fields.
struct NodeStartEvent {
    std::string        node_name;
    std::optional<int> retry_attempt;
    json               data;
};

/// Typed view of a node-end event. `data` preserves extension fields.
struct NodeEndEvent {
    std::string                node_name;
    std::optional<std::string> command_goto;
    std::size_t                send_count = 0;
    json                       data;
};

/// Typed LLM token event.
struct LlmTokenEvent {
    std::string node_name;
    std::string token;
};

/// Typed channel update event.
struct ChannelWriteEvent {
    std::string node_name;
    std::string channel;
    json        value;
};

/// Full serialized state emitted for StreamMode::VALUES.
struct StateSnapshotEvent {
    json state;
};

/// Routing decision emitted for StreamMode::DEBUG.
struct RoutingEvent {
    std::optional<std::string> command_goto;
    std::vector<std::string>   next_nodes;
    std::optional<int>         step;
    json                       data;
};

/// Dynamic Send batch emitted for StreamMode::DEBUG.
struct SendDispatchEvent {
    std::vector<Send> sends;
};

/// Typed HITL interrupt event. `data` remains available for custom metadata.
struct InterruptEvent {
    std::string                node_name;
    std::optional<std::string> phase;
    std::optional<std::string> checkpoint_id;
    json                       data;
};

/// Typed execution error event. `data` preserves retry diagnostics.
struct ErrorEvent {
    std::string node_name;
    std::string message;
    json        data;
};

/// Escape hatch for malformed input or payload shapes added by future versions.
struct RawGraphEvent {
    GraphEvent event;
};

using TypedGraphEvent = std::variant<NodeStartEvent,
                                     NodeEndEvent,
                                     LlmTokenEvent,
                                     ChannelWriteEvent,
                                     StateSnapshotEvent,
                                     RoutingEvent,
                                     SendDispatchEvent,
                                     InterruptEvent,
                                     ErrorEvent,
                                     RawGraphEvent>;

/**
 * @brief Convert the stable GraphEvent wire shape to an owning typed event.
 *
 * Existing JSON and callback contracts remain canonical. Sentinel debug events
 * (`__state__`, `__routing__`, and `__send__`) receive dedicated alternatives;
 * malformed or unknown payloads are returned as RawGraphEvent instead of
 * throwing from a streaming callback.
 */
NEOGRAPH_API TypedGraphEvent to_typed_event(const GraphEvent& event);

/// Callback for receiving graph execution events.
/// @param event The graph event to process.
using GraphStreamCallback = std::function<void(const GraphEvent&)>;

/// Callback for the additive typed event view.
using TypedGraphStreamCallback = std::function<void(const TypedGraphEvent&)>;

/// @brief Adapt a typed callback to the existing GraphStreamCallback surface.
inline GraphStreamCallback adapt_typed_stream(TypedGraphStreamCallback callback) {
    if (!callback) return {};
    return [callback = std::move(callback)](const GraphEvent& event) {
        callback(to_typed_event(event));
    };
}

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
