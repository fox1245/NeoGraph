/**
 * @file graph/engine.h
 * @brief Main graph execution engine with super-step loop and HITL support.
 *
 * GraphEngine is the central orchestrator that compiles JSON graph definitions,
 * executes them using the Pregel BSP (Bulk Synchronous Parallel) model,
 * and provides checkpointing, state management, and Human-in-the-Loop APIs.
 */
#pragma once

#include <neograph/graph/checkpoint.h>
#include <neograph/graph/compiler.h>
#include <neograph/graph/coordinator.h>
#include <neograph/graph/executor.h>
#include <neograph/graph/node.h>
#include <neograph/graph/scheduler.h>
#include <neograph/graph/state.h>
#include <neograph/graph/store.h>
#include <neograph/graph/types.h>

#include <asio/awaitable.hpp>

#include <memory>
#include <set>

namespace neograph::graph {

/**
 * @brief Configuration for a graph execution run.
 */
struct RunConfig {
    std::string thread_id;                          ///< Thread ID for checkpoint association.
    json        input;                              ///< Initial channel writes (e.g., {"messages": [...]}).
    int         max_steps  = 50;                    ///< Safety limit for maximum super-steps per run.
    StreamMode  stream_mode = StreamMode::ALL;      ///< Which event types to emit during streaming.
};

/**
 * @brief Result of a graph execution run.
 */
struct RunResult {
    json        output;                             ///< Final serialized graph state.
    bool        interrupted       = false;          ///< True if execution was interrupted (HITL).
    std::string interrupt_node;                     ///< Name of the node that triggered the interrupt.
    json        interrupt_value;                    ///< Value associated with the interrupt.
    std::string checkpoint_id;                      ///< ID of the last checkpoint saved.
    std::vector<std::string> execution_trace;       ///< Ordered list of executed node names.
};

/**
 * @brief Super-step loop execution engine for graph-based agent workflows.
 *
 * GraphEngine compiles a JSON graph definition into an executable workflow,
 * then runs it using the Pregel BSP model. Key capabilities:
 *
 * - **Parallel execution**: Multiple independent nodes run concurrently via Taskflow.
 * - **Checkpointing**: Full state snapshots at every super-step for time-travel.
 * - **HITL**: interrupt_before/after + resume() for human-in-the-loop workflows.
 * - **Send/Command**: Dynamic fan-out and routing overrides from nodes.
 * - **Retry policies**: Per-node exponential backoff on failure.
 *
 * ## Thread safety
 *
 * After `compile()` returns, the graph definition (nodes, edges, channels)
 * is treated as immutable. A single GraphEngine instance is therefore safe
 * to share across user threads that invoke `run()` / `run_stream()` /
 * `resume()` concurrently with **distinct `thread_id`s** — each call
 * constructs its own GraphState and the bundled InMemoryCheckpointStore
 * is mutex-guarded. This lets you host multi-tenant agent workloads on a
 * shared engine without an external async runtime; just dispatch onto
 * `std::async`, a thread pool, or your existing event loop's worker.
 *
 * Caveats:
 * - Mutator APIs (`set_retry_policy`, `set_node_retry_policy`,
 *   `set_checkpoint_store`, `set_store`, `own_tools`) must be called
 *   before any concurrent `run()` — they are configuration, not runtime.
 * - Concurrent `run()` calls sharing the **same** `thread_id` do not
 *   crash but produce unspecified checkpoint interleaving; serialize
 *   per-thread access yourself if you need deterministic semantics.
 * - Custom `GraphNode` subclasses must be stateless or self-synchronized.
 *   Node instances are owned by the engine and shared across all runs.
 * - User-provided `CheckpointStore` / `Store` / `Provider` / `Tool`
 *   implementations must be thread-safe.
 *
 * @code
 * auto engine = GraphEngine::compile(graph_json, context, checkpoint_store);
 * RunConfig config;
 * config.thread_id = "session-1";
 * config.input = {{"messages", json::array({{{"role","user"},{"content","Hello"}}})}};
 * auto result = engine->run(config);
 * @endcode
 *
 * @see RunConfig, RunResult, GraphNode
 */
class GraphEngine {
public:
    /**
     * @brief Compile a graph from a JSON definition.
     *
     * Parses the JSON graph definition, creates nodes via NodeFactory,
     * resolves edges and conditions, and returns a ready-to-run engine.
     *
     * @param definition JSON graph definition (nodes, edges, channels, etc.).
     * @param default_context Default NodeContext providing provider, tools, model.
     * @param store Optional checkpoint store for persistence (nullptr = no checkpointing).
     * @return A compiled GraphEngine ready for execution.
     * @throws std::runtime_error If the graph definition is invalid.
     */
    static std::unique_ptr<GraphEngine> compile(
        const json& definition,
        const NodeContext& default_context,
        std::shared_ptr<CheckpointStore> store = nullptr);

    /**
     * @brief Execute the graph synchronously (blocking).
     *
     * Thread-safe across distinct `thread_id`s on a shared engine instance:
     * dispatch via `std::async`, a thread pool, or any executor. See the
     * class-level "Thread safety" notes for caveats.
     *
     * @param config Run configuration with thread ID, input, and limits.
     * @return Execution result with final state and metadata.
     */
    RunResult run(const RunConfig& config);

    /**
     * @brief Async peer of run() — Stage 3 / Semester 3.6 (API surface).
     *
     * Returns an `asio::awaitable<RunResult>` so callers driving an
     * io_context can `co_await engine->run_async(cfg)` alongside other
     * coroutines (typically multiple concurrent agents).
     *
     * **Current implementation is a thin wrapper that co_returns
     * run(cfg).** This means the call still blocks the resumed
     * thread for the entire run duration — including all I/O —
     * because the engine's super-step loop, node dispatch, and
     * checkpoint writes are not yet coroutine-native. The non-
     * blocking benefit only materializes once the engine internals
     * are coroutinized (planned follow-up; node-level async already
     * exists via execute_async, so the building blocks are in place).
     *
     * Adding the wrapper now lets external callers migrate to the
     * async surface ahead of the internal refactor — when that lands,
     * no API breaks.
     *
     * @param config Run configuration.
     * @return Awaitable yielding the execution result.
     */
    asio::awaitable<RunResult> run_async(const RunConfig& config);

    /**
     * @brief Execute the graph with streaming event callbacks.
     * @param config Run configuration.
     * @param cb Callback invoked for each graph event (filtered by config.stream_mode).
     * @return Execution result.
     */
    RunResult run_stream(const RunConfig& config,
                         const GraphStreamCallback& cb);

    /// Async peer of run_stream — same caveat as run_async: thin
    /// wrapper today, real coroutine internals later.
    asio::awaitable<RunResult> run_stream_async(
        const RunConfig& config, const GraphStreamCallback& cb);

    /**
     * @brief Resume execution from a HITL interrupt.
     *
     * Loads the last checkpoint for the given thread, applies the resume
     * value, and continues execution from the interrupted node.
     *
     * @param thread_id Thread ID of the interrupted session.
     * @param resume_value Optional value to inject before resuming (e.g., human input).
     * @param cb Optional streaming callback.
     * @return Execution result after resumption.
     */
    RunResult resume(const std::string& thread_id,
                     const json& resume_value = json(),
                     const GraphStreamCallback& cb = nullptr);

    /// Async peer of resume — same caveat as run_async.
    asio::awaitable<RunResult> resume_async(
        const std::string& thread_id,
        const json& resume_value = json(),
        const GraphStreamCallback& cb = nullptr);

    // ── State inspection & manipulation (LangGraph Checkpointer API) ──

    /**
     * @brief Get the current state for a thread.
     * @param thread_id Thread ID to look up.
     * @return Serialized state JSON, or std::nullopt if no checkpoint exists.
     */
    std::optional<json> get_state(const std::string& thread_id) const;

    /**
     * @brief Get the checkpoint history for a thread (time-travel).
     * @param thread_id Thread ID to look up.
     * @param limit Maximum number of checkpoints to return (default: 100).
     * @return Vector of Checkpoint objects, newest first.
     */
    std::vector<Checkpoint> get_state_history(const std::string& thread_id,
                                              int limit = 100) const;

    /**
     * @brief Update the state for a thread by applying channel writes.
     *
     * Loads the latest checkpoint, applies the writes, and saves a new checkpoint.
     * Useful for injecting state externally (e.g., from a UI).
     *
     * @param thread_id Thread ID to update.
     * @param channel_writes JSON object of channel_name -> value pairs to write.
     * @param as_node Optional node name to attribute the update to (for tracing).
     */
    void update_state(const std::string& thread_id,
                      const json& channel_writes,
                      const std::string& as_node = "");

    /**
     * @brief Fork a thread, creating a new thread from an existing checkpoint.
     *
     * Copies the specified checkpoint (or the latest) to a new thread ID,
     * enabling branching execution paths.
     *
     * @param source_thread_id Thread to fork from.
     * @param new_thread_id Thread ID for the new fork.
     * @param checkpoint_id Specific checkpoint to fork from (empty = latest).
     * @return The checkpoint ID of the forked state.
     */
    std::string fork(const std::string& source_thread_id,
                     const std::string& new_thread_id,
                     const std::string& checkpoint_id = "");

    // ── Configuration ──

    /**
     * @brief Transfer tool ownership to the engine.
     *
     * The engine takes ownership of the tools and keeps them alive for
     * the duration of the engine's lifetime.
     *
     * @param tools Vector of tool unique_ptrs to transfer.
     */
    void own_tools(std::vector<std::unique_ptr<Tool>> tools);

    /**
     * @brief Set the checkpoint persistence store.
     * @param store Checkpoint store implementation.
     */
    void set_checkpoint_store(std::shared_ptr<CheckpointStore> store);

    /**
     * @brief Set the cross-thread shared memory store.
     * @param store Store implementation for cross-thread data sharing.
     * @see Store, InMemoryStore
     */
    void set_store(std::shared_ptr<Store> store);

    /**
     * @brief Get the cross-thread shared memory store.
     * @return Shared pointer to the Store, or nullptr if not set.
     */
    std::shared_ptr<Store> get_store() const { return store_; }

    /**
     * @brief Set the default retry policy for all nodes.
     * @param policy Retry policy with backoff configuration.
     */
    void set_retry_policy(const RetryPolicy& policy);

    /**
     * @brief Set a retry policy for a specific node (overrides default).
     * @param node_name Name of the node.
     * @param policy Retry policy for this specific node.
     */
    void set_node_retry_policy(const std::string& node_name, const RetryPolicy& policy);

    /**
     * @brief Get the graph name (from the JSON definition).
     * @return Graph name string.
     */
    const std::string& get_graph_name() const { return name_; }

private:
    GraphEngine() = default;

    void init_state(GraphState& state) const;
    void apply_input(GraphState& state, const json& input) const;

    RunResult execute_graph(const RunConfig& config,
                            const GraphStreamCallback& cb,
                            const std::vector<std::string>& resume_from = {},
                            const json& resume_value = json());

    RetryPolicy get_retry_policy(const std::string& node_name) const;

    // --- Graph definition ---
    std::string name_;

    /// Populated by GraphCompiler during compile(); consumed at runtime
    /// by init_state() to construct GraphState channels.
    std::vector<ChannelDef> channel_defs_;

    std::map<std::string, std::unique_ptr<GraphNode>> nodes_;
    std::vector<Edge>            edges_;
    std::vector<ConditionalEdge> conditional_edges_;

    /// Owns routing decisions. Constructed after edges_ /
    /// conditional_edges_ are populated in compile(); holds references
    /// to both, so it must be destroyed before them (trivially true:
    /// member order guarantees destruction is reverse of declaration).
    std::unique_ptr<Scheduler> scheduler_;

    /// Owns per-super-step node invocation (retry, replay, pending
    /// writes, Taskflow fan-out, Send dispatch). Holds references into
    /// nodes_ / channel_defs_ above, so must be declared after them —
    /// reverse destruction order keeps the references valid.
    std::unique_ptr<NodeExecutor> executor_;

    std::set<std::string> interrupt_before_;
    std::set<std::string> interrupt_after_;

    std::shared_ptr<CheckpointStore> checkpoint_store_;
    std::shared_ptr<Store>           store_;
    std::vector<std::unique_ptr<Tool>> owned_tools_;

    // Retry policies
    RetryPolicy default_retry_policy_;
    std::map<std::string, RetryPolicy> node_retry_policies_;
};

} // namespace neograph::graph
