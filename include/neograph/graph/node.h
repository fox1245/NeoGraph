/**
 * @file graph/node.h
 * @brief Graph node implementations: base class and built-in node types.
 *
 * Defines the abstract GraphNode base class and all built-in node types:
 * - LLMCallNode: makes LLM completion calls
 * - ToolDispatchNode: executes pending tool calls
 * - IntentClassifierNode: LLM-based intent routing
 * - SubgraphNode: hierarchical graph composition
 *
 * # Custom node API
 *
 * Custom nodes implement the single coroutine-native
 * `run(NodeInput) -> awaitable<NodeOutput>` virtual. The pre-v1
 * `execute*` virtual cross-product has been removed; see
 * `docs/migration-v0.4-to-v1.0.md` for before/after examples.
 *
 * The engine threads `RunContext` through `NodeInput::ctx`. Nodes can use
 * cancellation, usage accounting, thread/step/stream metadata, Store access,
 * and resume values. The deadline and trace-id fields are reserved extension
 * slots and are not populated by current `RunConfig` APIs.
 *
 * Example overrides that match the v0.4.x surface:
 *   - `examples/01_react_agent.cpp` — basic ReAct agent
 *   - `examples/02_custom_graph.cpp` — custom node subclass
 *   - `examples/05_parallel_fanout.cpp` — Send / Command pattern
 *
 * @see docs/migration-v0.4-to-v1.0.md
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/run_context.h>
#include <neograph/graph/types.h>
#include <neograph/graph/state.h>

#include <asio/awaitable.hpp>

#include <stdexcept>

namespace neograph::graph {

/**
 * @brief Per-call input bundle for the unified ``run()`` virtual.
 *
 * Replaces the old ``(state[, cb])`` parameter pair. Nodes override
 * ``GraphNode::run(NodeInput) -> awaitable<NodeOutput>`` and read
 * everything they need (state, per-run metadata, optional streaming
 * sink) from this struct. Trivially constructed at the dispatch site;
 * copied into the coroutine frame so its references survive suspension.
 */
struct NodeInput {
    /// Snapshot of the channel state visible to this node.
    const GraphState&  state;

    /// Per-run metadata threaded by the engine — cancel token, usage,
    /// thread_id, current super-step, stream mode, Store, and resume value.
    /// Deadline and trace-id members are reserved and currently unpopulated.
    const RunContext&  ctx;

    /// Streaming sink. ``nullptr`` for non-streaming runs (the engine
    /// passes a pointer to its callback only when the caller used
    /// ``run_stream`` / ``run_stream_async`` and ``StreamMode``
    /// requests events). New nodes that emit ``LLM_TOKEN`` events
    /// dereference this when non-null and ignore it otherwise.
    const GraphStreamCallback* stream_cb = nullptr;
};

/// Output of the unified ``run()`` virtual. Alias for ``NodeResult``:
/// channel writes plus optional Command and Send directives.
using NodeOutput = NodeResult;

/**
 * @brief Abstract base class for all graph nodes.
 *
 * Nodes are the building blocks of the graph. Each node reads from
 * the graph state and produces channel writes (and optionally Command
 * or Send directives). Override ``run(NodeInput)`` for every node shape.
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
class NEOGRAPH_API GraphNode {
public:
    virtual ~GraphNode() = default;

    /**
     * @brief Unified dispatch entry for sync work, async work, and streaming.
     *
     * **New nodes override THIS method.** Read ``in.state`` for channel
     * inputs, read ``in.ctx`` for available per-run metadata (cancel token,
     * thread ID, current step, …), check ``in.stream_cb`` for an
     * optional ``LLM_TOKEN`` sink, and return a ``NodeOutput`` (alias
     * for ``NodeResult``) populated with channel writes plus optional
     * ``Command`` / ``Send`` directives.
     *
     * The engine (``NodeExecutor::execute_node_with_retry_async``)
     * dispatches only through this method. Sync-vs-async is no longer
     * a public virtual cross-product: perform immediate work before
     * ``co_return``, or ``co_await`` asynchronous dependencies.
     *
     * @code
     * class MyNode : public GraphNode {
     *     asio::awaitable<NodeOutput> run(NodeInput in) override {
     *         auto messages = in.state.get_messages();
     *         auto reply = co_await provider_->complete_async({...});
     *         NodeOutput out;
     *         out.writes.push_back({"messages", json::array({reply})});
     *         co_return out;
     *     }
     *     std::string get_name() const override { return "my_node"; }
     * };
     * @endcode
     *
     * @see ``NodeInput`` and ``NodeOutput`` above.
     *
     * **Lifetime note**: ``in`` is taken **by value** so the coroutine
     * frame owns its own copy of the struct. The ``state`` / ``ctx``
     * references inside still point to objects on the engine's
     * suspended-frame stack (which outlive the run by construction),
     * but the struct itself is no longer a reference parameter.
     * Coroutine-reference-parameter UAF (per
     * ``feedback_async_bridge_required.md`` / the v0.2.0 RunConfig
     * crash) does not apply.
     */
    virtual asio::awaitable<NodeOutput> run(NodeInput in) = 0;

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
class NEOGRAPH_API LLMCallNode : public GraphNode {
public:
    /**
     * @brief Construct an LLM call node.
     * @param name Unique node name within the graph.
     * @param ctx Node context providing the LLM provider, tools, model, and instructions.
     */
    LLMCallNode(const std::string& name, const NodeContext& ctx);

    /// Unified ``run`` override. Builds completion params,
    /// calls ``provider_->complete_async`` (or ``complete_stream_async``
    /// when ``in.stream_cb`` is non-null and bridges per-token events
    /// to GraphEvent), writes the assistant message to the
    /// ``messages`` channel. Passes ``in.ctx.cancel_token`` directly
    /// into ``params.cancel_token`` so cancellation reaches the LLM
    /// HTTP socket without the legacy thread-local smuggling.
    asio::awaitable<NodeOutput> run(NodeInput in) override;

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
 * Tool dispatch does not emit LLM token events because it executes tool calls,
 * not a model stream. Tools can emit their own progress through application
 * observability when finer-grained reporting is needed.
 */
class NEOGRAPH_API ToolDispatchNode : public GraphNode {
public:
    /**
     * @brief Construct a tool dispatch node.
     * @param name Unique node name within the graph.
     * @param ctx Node context providing available tools.
     */
    ToolDispatchNode(const std::string& name, const NodeContext& ctx);

    /// Unified ``run`` — finds the latest assistant
    /// message with tool_calls, dispatches each call to the matching
    /// Tool, writes tool result messages back to the ``messages``
    /// channel.
    asio::awaitable<NodeOutput> run(NodeInput in) override;
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
class NEOGRAPH_API IntentClassifierNode : public GraphNode {
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

    /// v0.4 PR 9a: unified ``run`` — calls the LLM with the
    /// classification prompt, parses the result against
    /// ``valid_routes``, writes the chosen route to ``__route__``.
    /// Streams per-token events when ``in.stream_cb`` is non-null.
    asio::awaitable<NodeOutput> run(NodeInput in) override;
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
class NEOGRAPH_API SubgraphNode : public GraphNode {
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

    /// v0.4 PR 9a: unified ``run`` — drives the child engine via
    /// ``run_async`` (or ``run_stream_async`` when ``in.stream_cb``
    /// is non-null), maps channels through ``input_map`` /
    /// ``output_map``. ``in.ctx.cancel_token`` flows through into
    /// the child run's ``RunConfig`` so a parent cancel cascades to
    /// the subgraph.
    asio::awaitable<NodeOutput> run(NodeInput in) override;
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
