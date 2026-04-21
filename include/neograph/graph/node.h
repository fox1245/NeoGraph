/**
 * @file graph/node.h
 * @brief Graph node implementations: base class and built-in node types.
 *
 * Defines the abstract GraphNode base class and all built-in node types:
 * - LLMCallNode: makes LLM completion calls
 * - ToolDispatchNode: executes pending tool calls
 * - IntentClassifierNode: LLM-based intent routing
 * - SubgraphNode: hierarchical graph composition
 */
#pragma once

#include <neograph/graph/types.h>
#include <neograph/graph/state.h>

#include <asio/awaitable.hpp>

namespace neograph::graph {

/**
 * @brief Abstract base class for all graph nodes.
 *
 * Nodes are the building blocks of the graph. Each node reads from
 * the graph state and produces channel writes (and optionally Command
 * or Send directives). Override execute() for basic nodes, or
 * execute_full() to use Command/Send.
 *
 * ## Thread safety
 *
 * Node instances are owned by the GraphEngine and shared across **all**
 * concurrent `run()` invocations on that engine — including runs with
 * different `thread_id`s. Implementations MUST therefore be either
 * stateless (the recommended default — derive everything from the
 * `GraphState` argument) or fully self-synchronized. Storing per-run
 * scratch data in a member variable will silently corrupt parallel runs.
 *
 * Per-execution state belongs in the channels: read inputs from
 * `state.get(...)` and emit outputs as `ChannelWrite`s.
 */
class GraphNode {
public:
    virtual ~GraphNode() = default;

    /**
     * @brief Execute the node: read state, return channel writes.
     *
     * Stage 3 / Sem 3.4: defaults to bridging through `execute_async`
     * via `neograph::async::run_sync`. Subclasses written against the
     * sync path keep overriding this directly; async-native nodes (Sem
     * 3.5+) override `execute_async` and inherit this. Override at
     * least one — overriding neither yields infinite mutual recursion.
     *
     * @param state The current graph state (read-only access).
     * @return Vector of channel writes to apply to the state.
     */
    virtual std::vector<ChannelWrite> execute(const GraphState& state);

    /**
     * @brief Async peer for execute().
     *
     * Default body co_returns `execute(state)` — runs on whatever
     * thread resumes the coroutine, blocking it for the duration.
     * Override to issue non-blocking operations (typically
     * `co_await provider->complete_async(...)` for LLM nodes).
     *
     * @param state The current graph state.
     * @return Awaitable yielding the channel writes.
     */
    virtual asio::awaitable<std::vector<ChannelWrite>>
    execute_async(const GraphState& state);

    /**
     * @brief Streaming execution variant.
     *
     * Default implementation delegates to execute(). Override to emit
     * LLM_TOKEN events during execution.
     *
     * @param state The current graph state.
     * @param cb Callback for emitting streaming events.
     * @return Vector of channel writes.
     */
    virtual std::vector<ChannelWrite> execute_stream(
        const GraphState& state, const GraphStreamCallback& cb);

    /**
     * @brief Async peer of execute_stream — Sem 3.4b.
     *
     * Default body co_returns execute_stream(state, cb). Same crossover
     * shape as execute / execute_async. Override at least one of the
     * sync/async pair when adding a streaming-aware async node.
     */
    virtual asio::awaitable<std::vector<ChannelWrite>> execute_stream_async(
        const GraphState& state, const GraphStreamCallback& cb);

    /**
     * @brief Extended execute returning a full NodeResult.
     *
     * Default implementation wraps execute() output into NodeResult::writes.
     * Override this to return Command (routing override) or Send (dynamic fan-out).
     *
     * @param state The current graph state.
     * @return NodeResult with writes, optional Command, and optional Sends.
     */
    virtual NodeResult execute_full(const GraphState& state);

    /**
     * @brief Async peer of execute_full — Sem 3.4b.
     *
     * Default routes through execute_full() via run_sync, which in turn
     * wraps execute()'s writes into a NodeResult. Async-native nodes
     * that emit Command/Send override this directly to keep the
     * NodeResult assembly inside the coroutine.
     */
    virtual asio::awaitable<NodeResult> execute_full_async(
        const GraphState& state);

    /**
     * @brief Extended streaming execution returning a full NodeResult.
     * @param state The current graph state.
     * @param cb Callback for emitting streaming events.
     * @return NodeResult with writes, optional Command, and optional Sends.
     */
    virtual NodeResult execute_full_stream(
        const GraphState& state, const GraphStreamCallback& cb);

    /**
     * @brief Async peer of execute_full_stream — Sem 3.4b.
     */
    virtual asio::awaitable<NodeResult> execute_full_stream_async(
        const GraphState& state, const GraphStreamCallback& cb);

    /**
     * @brief Get the node's unique name within the graph.
     * @return Node name string.
     */
    virtual std::string get_name() const = 0;
};

/**
 * @brief Node that makes LLM completion calls.
 *
 * Reads the "messages" channel, builds completion parameters,
 * calls the LLM provider, and writes the response back to "messages".
 * Mirrors the Agent::run() LLM call logic.
 */
class LLMCallNode : public GraphNode {
public:
    /**
     * @brief Construct an LLM call node.
     * @param name Unique node name within the graph.
     * @param ctx Node context providing the LLM provider, tools, model, and instructions.
     */
    LLMCallNode(const std::string& name, const NodeContext& ctx);

    /// Async-native execute — co_awaits provider_->complete_async so a
    /// run on a shared io_context doesn't block during the LLM call.
    /// The sync execute() is inherited from GraphNode and routes
    /// through this via run_sync.
    asio::awaitable<std::vector<ChannelWrite>>
    execute_async(const GraphState& state) override;

    std::vector<ChannelWrite> execute_stream(
        const GraphState& state, const GraphStreamCallback& cb) override;
    std::string get_name() const override { return name_; }

private:
    std::string              name_;
    std::shared_ptr<Provider> provider_;
    std::vector<Tool*>       tools_;
    std::string              model_;
    std::string              instructions_;

    CompletionParams build_params(const GraphState& state) const;
};

/**
 * @brief Node that dispatches and executes pending tool calls.
 *
 * Reads the last assistant message from the "messages" channel,
 * executes each tool call, and writes tool result messages back.
 *
 * Deliberately does NOT override `execute_stream`: tool execution is
 * synchronous work against the user's `Tool` implementations and
 * produces no LLM token stream to emit. The default `execute_stream`
 * inherited from GraphNode falls through to `execute()`, which is the
 * intended behaviour. Emit your own progress events from inside your
 * Tool if you need fine-grained observability.
 */
class ToolDispatchNode : public GraphNode {
public:
    /**
     * @brief Construct a tool dispatch node.
     * @param name Unique node name within the graph.
     * @param ctx Node context providing available tools.
     */
    ToolDispatchNode(const std::string& name, const NodeContext& ctx);

    std::vector<ChannelWrite> execute(const GraphState& state) override;
    std::string get_name() const override { return name_; }

private:
    std::string        name_;
    std::vector<Tool*> tools_;
};

// Forward declaration
class GraphEngine;

/**
 * @brief Node that classifies user intent via LLM and routes execution.
 *
 * Calls the LLM with a classification prompt, writes the detected intent
 * to the "__route__" channel. Used with the "route_channel" condition
 * for dynamic routing based on user intent.
 */
class IntentClassifierNode : public GraphNode {
public:
    /**
     * @brief Construct an intent classifier node.
     * @param name Unique node name within the graph.
     * @param ctx Node context providing the LLM provider.
     * @param prompt Classification prompt template sent to the LLM.
     * @param valid_routes List of valid intent route names the LLM can return.
     */
    IntentClassifierNode(const std::string& name, const NodeContext& ctx,
                         const std::string& prompt,
                         std::vector<std::string> valid_routes);

    /// Async-native classify — same rationale as LLMCallNode.
    asio::awaitable<std::vector<ChannelWrite>>
    execute_async(const GraphState& state) override;

    std::vector<ChannelWrite> execute_stream(
        const GraphState& state, const GraphStreamCallback& cb) override;
    std::string get_name() const override { return name_; }

private:
    std::string               name_;
    std::shared_ptr<Provider>  provider_;
    std::string               model_;
    std::string               prompt_;
    std::vector<std::string>  valid_routes_;

    /// Shared setup: returns (params, route_tail_callback) common to
    /// the streaming and non-streaming execute paths.
    CompletionParams build_params(const GraphState& state) const;
    std::vector<ChannelWrite> route_from(const std::string& intent) const;
};

/**
 * @brief Node that runs a compiled GraphEngine as a single node (hierarchical composition).
 *
 * Enables the Supervisor pattern and nested workflows. Channels are
 * mapped between parent and child graphs via input_map and output_map.
 *
 * @code
 * // Map parent "messages" -> child "messages", child "result" -> parent "findings"
 * auto sub = std::make_shared<SubgraphNode>("inner", subgraph,
 *     {{"messages", "messages"}},   // input_map: parent -> child
 *     {{"result", "findings"}});    // output_map: child -> parent
 * @endcode
 */
class SubgraphNode : public GraphNode {
public:
    /**
     * @brief Construct a subgraph node.
     * @param name Unique node name within the parent graph.
     * @param subgraph Compiled GraphEngine to run as a sub-workflow.
     * @param input_map Mapping of parent_channel -> child_channel for input.
     * @param output_map Mapping of child_channel -> parent_channel for output.
     */
    SubgraphNode(const std::string& name,
                 std::shared_ptr<GraphEngine> subgraph,
                 std::map<std::string, std::string> input_map = {},
                 std::map<std::string, std::string> output_map = {});

    /// Async-native execute — co_awaits subgraph_->run_async so the
    /// parent run shares its io_context with the child run instead
    /// of stacking sync calls. Wires correctly through the engine
    /// thin wrapper (Sem 3.6 API surface); when the engine internals
    /// go coroutine-native, nested subgraphs benefit transparently.
    asio::awaitable<std::vector<ChannelWrite>>
    execute_async(const GraphState& state) override;

    std::vector<ChannelWrite> execute_stream(
        const GraphState& state, const GraphStreamCallback& cb) override;
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    std::shared_ptr<GraphEngine> subgraph_;
    std::map<std::string, std::string> input_map_;
    std::map<std::string, std::string> output_map_;

    json build_subgraph_input(const GraphState& state) const;
    std::vector<ChannelWrite> extract_output(const json& subgraph_output) const;
};

} // namespace neograph::graph
