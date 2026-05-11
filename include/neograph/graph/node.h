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
 * # v0.4.x migration navigator (external authors)
 *
 * Writing a custom node subclass in v0.4.x? **Keep using the legacy
 * 8-virtual surface** (`execute` / `execute_async` / `execute_full` /
 * `execute_stream` and their `_async` pairs) — those still work and
 * the engine drives them on every dispatch path. The new unified
 * `run(NodeInput) -> awaitable<NodeOutput>` virtual is additive in
 * v0.4.0 (PR 2 in `ROADMAP_v1.md`): it forwards to the legacy chain
 * by default, so existing subclasses compile unchanged. PR 4 of the
 * same plan will mark the legacy virtuals `[[deprecated]]`; v1.0
 * deletes them.
 *
 * The `RunContext` field plumbed through `NodeInput::ctx` is
 * engine-internal for now — PR 1 (v0.4.0) only carries it through
 * the dispatch hops. User-overridable virtuals receive it once PR 2
 * lands. Until then, cancel token / deadline / trace_id propagate
 * via the legacy paths.
 *
 * Example overrides that match the v0.4.x surface:
 *   - `examples/01_react_agent.cpp` — basic ReAct agent
 *   - `examples/02_custom_graph.cpp` — custom node subclass
 *   - `examples/05_parallel_fanout.cpp` — Send / Command pattern
 *
 * @see ROADMAP_v1.md for the v0.4 → v1.0 PR sequence
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/types.h>
#include <neograph/graph/state.h>

#include <asio/awaitable.hpp>

#include <stdexcept>

namespace neograph::graph {

/// Per-run dispatch metadata. Defined in
/// ``neograph/graph/engine.h``; forward-declared here because
/// ``node.h`` is below ``engine.h`` in the include order (a full
/// ``#include`` would loop). The translation unit
/// (``graph_node.cpp``) pulls in ``engine.h`` to see the layout.
struct RunContext;

/**
 * @brief Per-call input bundle for the v0.4 unified ``run()`` virtual.
 *
 * Replaces the ``(state[, cb])`` parameter pair that the legacy 8-
 * virtual cross-product passed around. New nodes override
 * ``GraphNode::run(NodeInput) -> awaitable<NodeOutput>`` and read
 * everything they need (state, per-run metadata, optional streaming
 * sink) from this struct. Trivially constructed at the dispatch site;
 * cheap to pass by const reference.
 *
 * **PR 2 (v0.4.0)**: ``run()`` is additive — the default body
 * forwards to the legacy 8-virtual chain, so existing C++ subclasses
 * continue to compile and work unchanged. PR 4 marks the legacy
 * virtuals ``[[deprecated]]``; v1.0 deletes them.
 *
 * @see ROADMAP_v1.md "Execution plan" → PR 2
 */
struct NodeInput {
    /// Snapshot of the channel state visible to this node.
    const GraphState&  state;

    /// Per-run metadata threaded by the engine — cancel token,
    /// deadline, trace_id, thread_id, current super-step, stream
    /// mode. PR 1 plumbed this through every dispatch hop; PR 2 is
    /// the first place a user-overridable virtual receives it.
    const RunContext&  ctx;

    /// Streaming sink. ``nullptr`` for non-streaming runs (the engine
    /// passes a pointer to its callback only when the caller used
    /// ``run_stream`` / ``run_stream_async`` and ``StreamMode``
    /// requests events). New nodes that emit ``LLM_TOKEN`` events
    /// dereference this when non-null and ignore it otherwise.
    const GraphStreamCallback* stream_cb = nullptr;
};

/// Output of the v0.4 unified ``run()`` virtual. Same shape as the
/// legacy ``NodeResult`` (writes + optional Command + Sends), aliased
/// here so user code can use either name interchangeably during the
/// deprecation window. The ROADMAP names it ``NodeOutput`` because the
/// "input → output" pairing reads more naturally than "input → result"
/// at the call site; the underlying structure is unchanged.
using NodeOutput = NodeResult;

/// PR 4 (v0.4.0): deprecation marker for the legacy 8-virtual chain.
/// Centralised so the message stays consistent across every override
/// point. v0.4.x emits ``-Wdeprecated-declarations`` warnings; v1.0
/// removes the marked methods entirely. See ROADMAP_v1.md PR 9.
#define NEOGRAPH_DEPRECATED_VIRTUAL                              \
    [[deprecated(                                                \
        "v0.4: override run(NodeInput) -> awaitable<NodeOutput> " \
        "instead. The legacy 8-virtual chain is preserved for "   \
        "back-compat through v0.5 and removed in v1.0. See "      \
        "ROADMAP_v1.md.")]]

/**
 * @brief Thrown by the GraphNode default-execute chain when none of
 *        ``execute`` / ``execute_async`` / ``execute_full`` /
 *        ``execute_full_async`` is overridden in a subclass.
 *
 * Inherits from ``std::runtime_error`` for back-compat — existing
 * test code catching ``runtime_error`` continues to work. The
 * dedicated type lets the engine (and the streaming default below)
 * distinguish "user forgot to override" from "user override threw
 * for legitimate reasons", so we can fall through to
 * ``execute_stream`` for streaming-only nodes without silently
 * swallowing real errors. (TODO_v0.3.md item #10 / v0.3.2.)
 */
class NEOGRAPH_API GraphNodeMissingOverride : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

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
class NEOGRAPH_API GraphNode {
public:
    virtual ~GraphNode() = default;

    /**
     * @brief v0.4 unified dispatch entry — replaces the 8-virtual cross
     *        product over (sync/async) × (writes/full) × (stream/non-stream).
     *
     * **New nodes override THIS method.** Read ``in.state`` for channel
     * inputs, read ``in.ctx`` for per-run metadata (cancel token,
     * deadline, current step, …), check ``in.stream_cb`` for an
     * optional ``LLM_TOKEN`` sink, and return a ``NodeOutput`` (alias
     * for ``NodeResult``) populated with channel writes plus optional
     * ``Command`` / ``Send`` directives.
     *
     * **Legacy nodes that override one of the 8 virtuals** keep
     * working unchanged: the default body of ``run()`` below forwards
     * to ``execute_full_async`` / ``execute_full_stream_async``, which
     * preserves the existing default-fallback chain (and its
     * ``ExecuteDefaultGuard`` recursion guard).
     *
     * The engine (``NodeExecutor::execute_node_with_retry_async``)
     * dispatches via this method as of PR 2. Sync-vs-async is no
     * longer a user concern — return whatever your body needs to
     * ``co_return`` (sync work: ``co_return execute(...)``; async work:
     * ``co_return co_await provider->complete_async(...)``).
     *
     * @code
     * class MyNode : public GraphNode {
     *     asio::awaitable<NodeOutput> run(const NodeInput& in) override {
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
     * @see ROADMAP_v1.md "Execution plan" → PR 2; ``NodeInput`` and
     * ``NodeOutput`` above.
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
    virtual asio::awaitable<NodeOutput> run(NodeInput in);

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
    NEOGRAPH_DEPRECATED_VIRTUAL
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
    NEOGRAPH_DEPRECATED_VIRTUAL
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
    NEOGRAPH_DEPRECATED_VIRTUAL
    virtual std::vector<ChannelWrite> execute_stream(
        const GraphState& state, const GraphStreamCallback& cb);

    /**
     * @brief Async peer of execute_stream — Sem 3.4b.
     *
     * Default body co_returns execute_stream(state, cb). Same crossover
     * shape as execute / execute_async. Override at least one of the
     * sync/async pair when adding a streaming-aware async node.
     */
    NEOGRAPH_DEPRECATED_VIRTUAL
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
    NEOGRAPH_DEPRECATED_VIRTUAL
    virtual NodeResult execute_full(const GraphState& state);

    /**
     * @brief Async peer of execute_full — Sem 3.4b / Stage 4.
     *
     * Default routes through `execute_full(state)` directly so that:
     *   - Sync overrides of `execute_full` that emit Command/Send
     *     work correctly on the async path (no silent dropping).
     *   - Async-native overrides of `execute_async` flow through
     *     `execute_full → execute → run_sync(execute_async)` for a
     *     single sync→async hop.
     *
     * **Recommended override pattern**:
     *   - Async-native Send/Command emitters: override THIS method
     *     directly, build the NodeResult inside the awaitable.
     *   - Sync-only emitters: don't need to override this — the
     *     default already calls your sync `execute_full`.
     *
     * The pre-6bd9632 contract required a one-line bridge override
     * here for sync Send/Command emitters; that contract is no longer
     * needed but is harmless if you have it.
     */
    NEOGRAPH_DEPRECATED_VIRTUAL
    virtual asio::awaitable<NodeResult> execute_full_async(
        const GraphState& state);

    /**
     * @brief Extended streaming execution returning a full NodeResult.
     * @param state The current graph state.
     * @param cb Callback for emitting streaming events.
     * @return NodeResult with writes, optional Command, and optional Sends.
     */
    NEOGRAPH_DEPRECATED_VIRTUAL
    virtual NodeResult execute_full_stream(
        const GraphState& state, const GraphStreamCallback& cb);

    /**
     * @brief Async peer of execute_full_stream — Sem 3.4b.
     */
    NEOGRAPH_DEPRECATED_VIRTUAL
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
class NEOGRAPH_API LLMCallNode : public GraphNode {
public:
    /**
     * @brief Construct an LLM call node.
     * @param name Unique node name within the graph.
     * @param ctx Node context providing the LLM provider, tools, model, and instructions.
     */
    LLMCallNode(const std::string& name, const NodeContext& ctx);

    /// v0.4 PR 9a: unified ``run`` override. Builds completion params,
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
 * Deliberately does NOT override `execute_stream`: tool execution is
 * synchronous work against the user's `Tool` implementations and
 * produces no LLM token stream to emit. The default `execute_stream`
 * inherited from GraphNode falls through to `execute()`, which is the
 * intended behaviour. Emit your own progress events from inside your
 * Tool if you need fine-grained observability.
 */
class NEOGRAPH_API ToolDispatchNode : public GraphNode {
public:
    /**
     * @brief Construct a tool dispatch node.
     * @param name Unique node name within the graph.
     * @param ctx Node context providing available tools.
     */
    ToolDispatchNode(const std::string& name, const NodeContext& ctx);

    /// v0.4 PR 9a: unified ``run`` — finds the latest assistant
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
