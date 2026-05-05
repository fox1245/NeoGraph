/**
 * @file graph/engine.h
 * @brief Main graph execution engine with super-step loop and HITL support.
 *
 * GraphEngine is the central orchestrator that compiles JSON graph definitions,
 * executes them using the Pregel BSP (Bulk Synchronous Parallel) model,
 * and provides checkpointing, state management, and Human-in-the-Loop APIs.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/compiler.h>
#include <neograph/graph/coordinator.h>
#include <neograph/graph/executor.h>
#include <neograph/graph/node.h>
#include <neograph/graph/node_cache.h>
#include <neograph/graph/scheduler.h>
#include <neograph/graph/state.h>
#include <neograph/graph/store.h>
#include <neograph/graph/types.h>

#include <asio/awaitable.hpp>
#include <asio/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace neograph::graph {

/**
 * @brief Configuration for a graph execution run.
 */
struct RunConfig {
    std::string thread_id;                          ///< Thread ID for checkpoint association.
    json        input;                              ///< Initial channel writes (e.g., {"messages": [...]}).
    int         max_steps  = 50;                    ///< Safety limit for maximum super-steps per run.
    StreamMode  stream_mode = StreamMode::ALL;      ///< Which event types to emit during streaming.

    /**
     * @brief Optional cooperative cancel handle (v0.3+).
     *
     * When set, the engine super-step loop polls
     * ``cancel_token->is_cancelled()`` between steps and bails with
     * ``CancelledException``. The pybind binding additionally binds
     * the token's ``cancellation_slot`` to the run's ``co_spawn`` so
     * an in-flight LLM HTTP request gets aborted at the socket layer.
     *
     * Default ``nullptr`` → no cancellation; existing behaviour
     * unchanged for callers that haven't opted in.
     */
    std::shared_ptr<CancelToken> cancel_token;

    /**
     * @brief Auto-resume from latest checkpoint for ``thread_id`` (v0.3.1+).
     *
     * Default ``false`` keeps the historical behaviour: every run
     * starts from a fresh ``GraphState`` initialized by reducers and
     * overwritten by ``input``. Multi-turn callers carrying prior
     * conversation state through the input dict themselves see no
     * change.
     *
     * When ``true``:
     *
     * 1. The engine loads the latest checkpoint for ``thread_id`` (if
     *    one exists). The checkpoint's channel values seed
     *    ``GraphState`` instead of the per-channel initial values.
     * 2. ``input`` is then applied on top via the same reducer pipeline
     *    as a fresh run — so e.g. an APPEND ``messages`` channel grows
     *    by the new turn instead of being clobbered.
     * 3. The super-step loop starts at the entry node (``plan_start_step``)
     *    — this flag is for the multi-turn-chat use case where the
     *    previous run completed at ``__end__`` and the caller wants to
     *    add a new user message and re-run. For HITL resume from an
     *    interrupted run, use ``resume()`` / ``resume_async()`` instead.
     * 4. If no checkpoint exists for ``thread_id``, behaves as if the
     *    flag were unset (fresh run from ``input``). No error.
     *
     * Requires a configured ``CheckpointStore`` — without one the flag
     * is a no-op.
     */
    bool resume_if_exists = false;
};

/**
 * @brief Per-run dispatch metadata threaded through the engine and executor.
 *
 * Built from ``RunConfig`` at the top of ``execute_graph_async`` and
 * carried by reference through every internal dispatch hop
 * (``NodeExecutor::run_one_async`` / ``run_parallel_async`` /
 * ``run_sends_async`` / ``execute_node_with_retry_async``). Replaces the
 * v0.3.x-era smuggling channels — ``GraphState::run_cancel_token_`` and
 * the ``current_cancel_token()`` thread-local — with one explicit
 * argument that survives every Send-fan-out / serialize-restore /
 * thread-hop.
 *
 * **PR 1 (v0.4.0) is plumbing-only**: the struct exists, the engine
 * builds it, and ``NodeExecutor`` carries it through, but nothing yet
 * consumes it. ``GraphNode::execute_full_async`` still receives only
 * ``state``. The smuggling channels remain authoritative until
 * subsequent PRs flip the consumers (PR 2: new ``run(NodeInput)``
 * virtual; PR 3: hierarchical CancelToken; PR 4: deprecate the
 * smuggling).
 *
 * Cheap to copy (one shared_ptr + a couple of strings); workers that
 * need an isolated copy take it by value, the common path takes it by
 * const reference.
 *
 * @see RunConfig (the caller-supplied source) and ROADMAP_v1.md
 * (Candidate 2: "Explicit RunContext for per-run metadata").
 */
struct RunContext {
    /// Cooperative cancel handle. Mirrors ``RunConfig::cancel_token``.
    /// Null when the caller did not opt in.
    std::shared_ptr<CancelToken> cancel_token;

    /// Optional absolute wall-clock deadline. Reserved for a future PR
    /// (RunConfig has no deadline field today); left ``std::nullopt``
    /// by the engine for now so existing behaviour is unchanged.
    std::optional<std::chrono::steady_clock::time_point> deadline;

    /// Per-run trace correlator. Reserved for OTel integration; the
    /// engine does not populate this in PR 1 (callers can fill it via
    /// a future ``RunConfig::trace_id`` field).
    std::string trace_id;

    /// Mirrors ``RunConfig::thread_id`` so executor-side logic (e.g.
    /// future per-thread metric tags) does not have to hold a separate
    /// ``RunConfig`` reference.
    std::string thread_id;

    /// Current super-step index. Updated by the engine at the top of
    /// each super-step iteration so per-step consumers see a consistent
    /// value without the engine threading ``int step`` separately.
    int step = 0;

    /// Mirrors ``RunConfig::stream_mode``.
    StreamMode stream_mode = StreamMode::ALL;
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
 * - **Parallel execution**: Multiple independent nodes run concurrently on an
 *   engine-owned `asio::thread_pool`; sync and async entry points share it.
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
 *
 * @note Public surface size — this class exposes ~23 public methods
 * spanning three concerns: graph execution (run/run_async/run_stream/
 * resume), state administration (get_state/update_state/fork), and
 * runtime configuration (set_retry_policy/set_worker_count/
 * set_node_cache_enabled/set_checkpoint_store/own_tools). A future
 * major version (v1.0) is expected to split into `GraphEngine` (run
 * + resume only), `GraphAdmin` (state inspection/update/fork), and a
 * `GraphConfigBuilder` consumed at compile time. The current shape
 * is kept so existing examples and downstream consumers don't break;
 * the class-level docs above flag mutator setters as "configuration,
 * not runtime" already.
 */
class NEOGRAPH_API GraphEngine {
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
     * @brief Async peer of run() — returns an awaitable yielding the result.
     *
     * Callers driving an io_context can `co_await engine->run_async(cfg)`
     * alongside other coroutines (typically multiple concurrent agents).
     * The super-step loop, node dispatch, checkpoint I/O, parallel
     * fan-out, and retry backoff are all coroutine-native (3.0) — the
     * caller's executor is never blocked by engine work.
     *
     * The config is taken **by value** so the awaitable owns its own
     * copy in the coroutine frame. This makes the common
     * `asio::co_spawn(io.get_executor(), engine->run_async(stack_cfg),
     * use_future)` shape safe — `stack_cfg` may go out of scope before
     * the awaitable resolves without dangling-referencing the config.
     *
     * @param config Run configuration (moved into the coroutine frame).
     * @return Awaitable yielding the execution result.
     */
    asio::awaitable<RunResult> run_async(RunConfig config);

    /**
     * @brief Execute the graph with streaming event callbacks.
     * @param config Run configuration.
     * @param cb Callback invoked for each graph event (filtered by config.stream_mode).
     * @return Execution result.
     */
    RunResult run_stream(const RunConfig& config,
                         const GraphStreamCallback& cb);

    /// Async peer of run_stream — non-blocking coroutine surface.
    /// `config` and `cb` are taken by value for the same reason as
    /// run_async() — see that overload's docstring.
    asio::awaitable<RunResult> run_stream_async(
        RunConfig config, GraphStreamCallback cb);

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

    /// Async peer of resume — non-blocking coroutine surface.
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
     * @brief Resize the dedicated worker pool for parallel fan-out.
     *
     * `compile()` already wires up a pool sized to
     * `std::thread::hardware_concurrency()` (with a fallback of 4 if
     * the platform fails to detect), so multi-Send fan-out
     * parallelizes by default. Call this only to override that
     * default — for example, `set_worker_count(1)` for nodes that
     * hold non-thread-safe state, or a larger value if the workload's
     * fan-out width exceeds the core count.
     *
     * Must be called before any concurrent `run()`; resizing rebuilds
     * both the pool and the internal executor and is not safe against
     * in-flight runs. Values < 1 are clamped to 1.
     *
     * @param n Number of worker threads in the fan-out pool.
     * @see set_worker_count_auto()
     */
    void set_worker_count(std::size_t n);

    /**
     * @brief Resize the worker pool to `hardware_concurrency()`.
     *
     * Equivalent to
     * `set_worker_count(std::thread::hardware_concurrency())`, with
     * the same fallback (4) if the runtime cannot detect. `compile()`
     * already calls this; use it only to revert after an explicit
     * `set_worker_count(N)` overrode the default.
     */
    void set_worker_count_auto();

    /**
     * @brief Enable or disable per-node result caching.
     *
     * When enabled for a node, the executor hashes the input state and
     * looks up `(node_name, hash)` in the engine's NodeCache. On hit,
     * the cached NodeResult is replayed without invoking the node — no
     * LLM call, no tool execution. On miss, the node runs and the
     * result is stored.
     *
     * Cache is OFF by default. Only opt in for nodes that are pure
     * (deterministic, no external side effects, no time dependence).
     * Streaming runs (`run_stream`) bypass the cache for the affected
     * nodes because cached hits cannot replay LLM_TOKEN events.
     *
     * @param node_name Name of the node to enable / disable.
     * @param enabled   True to enable caching; false to disable.
     */
    void set_node_cache_enabled(const std::string& node_name, bool enabled);

    /**
     * @brief Drop all cached entries (per-node enable state preserved).
     */
    void clear_node_cache();

    /// @brief Borrow the engine's NodeCache for stats inspection.
    const NodeCache& node_cache() const { return node_cache_; }

    /**
     * @brief Get the graph name (from the JSON definition).
     * @return Graph name string.
     */
    const std::string& get_graph_name() const { return name_; }

private:
    GraphEngine() = default;

    void init_state(GraphState& state) const;
    void apply_input(GraphState& state, const json& input) const;

    /// Super-step loop (coroutine). Owns: state init, interrupt
    /// gates, resume load, super-step commit, routing via Scheduler.
    /// Delegates: node invocation to NodeExecutor, checkpoint
    /// lifecycle to CheckpointCoordinator, routing decisions to
    /// Scheduler. All internal I/O is non-blocking via co_await.
    asio::awaitable<RunResult> execute_graph_async(
        const RunConfig& config,
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
    /// writes, parallel fan-out, Send dispatch). Holds references
    /// into nodes_ / channel_defs_ above, so must be declared after
    /// them — reverse destruction order keeps the references valid.
    std::unique_ptr<NodeExecutor> executor_;

    std::set<std::string> interrupt_before_;
    std::set<std::string> interrupt_after_;

    std::shared_ptr<CheckpointStore> checkpoint_store_;
    std::shared_ptr<Store>           store_;
    std::vector<std::unique_ptr<Tool>> owned_tools_;

    // Retry policies
    RetryPolicy default_retry_policy_;
    std::map<std::string, RetryPolicy> node_retry_policies_;

    /// Optional fan-out worker pool. Null by default; populated only
    /// when set_worker_count() is called. When present,
    /// NodeExecutor dispatches parallel-branch co_spawns onto this
    /// pool's executor so CPU-bound fan-out parallelizes across
    /// cores. Declared after executor_ so reverse-order destruction
    /// joins the pool workers *before* executor_ and its node refs
    /// are freed.
    std::unique_ptr<asio::thread_pool> pool_;

    /// Per-node result cache (opt-in via set_node_cache_enabled).
    /// Stored by value so the engine owns it; NodeExecutor holds a
    /// non-owning pointer threaded through set_worker_count() rebuilds.
    NodeCache node_cache_;

    /// Inflight-run counter. Incremented at the top of
    /// execute_graph_async and decremented at coroutine completion
    /// via an RAII guard. set_worker_count() asserts this is zero
    /// before swapping the executor — resizing the pool while a run
    /// is mid-flight would drop tasks deferred onto the old pool.
    std::atomic<int> active_runs_{0};
};

} // namespace neograph::graph
