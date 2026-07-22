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
#include <neograph/graph/admin.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/channel_key.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/compiler.h>
#include <neograph/graph/coordinator.h>
#include <neograph/graph/executor.h>
#include <neograph/graph/node.h>
#include <neograph/graph/node_cache.h>
#include <neograph/graph/registry.h>
#include <neograph/graph/run_context.h>
#include <neograph/graph/scheduler.h>
#include <neograph/graph/state.h>
#include <neograph/graph/store.h>
#include <neograph/graph/types.h>
#include <neograph/tool_dispatch.h>   // ToolGate (issue #89)
#include <neograph/tool_set.h>

#include <asio/awaitable.hpp>
#include <asio/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neograph::graph {

class ValidatedTopology;

/**
 * @brief Construction-time configuration for a GraphEngine.
 *
 * This is the canonical path for new code: collect runtime dependencies and
 * policies first, then hand the complete configuration to GraphEngine::build().
 * The resulting engine is ready to run without a sequence of post-compile
 * setter calls.
 *
 * Existing GraphEngine::compile() and configuration setters remain supported.
 * compile() is a compatibility facade over build(), and the setters update the
 * same internal state represented here.
 */
struct EngineConfig {
    /// Provider, tools, model, and instructions used while factories create nodes.
    NodeContext node_context;

    /// Optional durable execution-state backend.
    std::shared_ptr<CheckpointStore> checkpoint_store;

    /// Optional cross-thread long-term memory exposed as RunContext::store.
    std::shared_ptr<Store> store;

    /// Optional override for the topology's default retry policy.
    std::optional<RetryPolicy> retry_policy;

    /// Per-node retry overrides applied before the first run.
    std::map<std::string, RetryPolicy> node_retry_policies;

    /// Engine-wide tool policy, preserved across run() and resume().
    ToolGate tool_gate;

    /// Fan-out worker count. One preserves the historical no-pool fast path.
    std::size_t worker_count = 1;

    /// Pure nodes whose result cache should be enabled at construction time.
    std::set<std::string> cached_nodes;
};

/**
 * @brief Owned construction resources layered beside EngineConfig.
 *
 * Kept as a sibling type so the already-public EngineConfig and NodeContext
 * layouts remain unchanged. Empty resources preserve the legacy raw-tool and
 * process-global registry behavior. Move this value into build() or link();
 * ToolSet ownership transfers to the resulting engine.
 */
struct EngineResources {
    ToolSet                              tools;
    std::shared_ptr<const GraphRegistry> registry;
};

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
     * ``CancelledException``. Each engine entry point forks an operation
     * child and binds only that child's ``cancellation_slot`` to its internal
     * work, so an in-flight LLM HTTP request gets aborted at the socket layer
     * without sharing one slot across concurrent runs.
     *
     * Default ``nullptr`` → no cancellation; existing behaviour
     * unchanged for callers that haven't opted in.
     */
    std::shared_ptr<CancelToken> cancel_token;

    /// Token accounting for this run (issue #88). Leave null and the engine
    /// makes one; pass your own to accumulate across several runs — e.g. a
    /// per-tenant budget that spans a whole conversation rather than one turn.
    /// Whatever ends up here is what ``RunResult::usage`` reports.
    std::shared_ptr<UsageAccumulator> usage;

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

/// @brief Normal, paused, or step-limited outcome of a successful run call.
enum class RunStatus {
    Completed,
    Interrupted,
    StepLimit,
};

/**
 * @brief Result of a graph execution run.
 *
 * ## ``output`` shape (issue #25)
 *
 * The canonical shape of ``output`` is **channels-wrapped**: each
 * channel value lives at ``output["channels"][<name>]["value"]``,
 * with sibling metadata such as ``["version"]`` recording how many
 * times the channel was written. ``output["global_version"]`` carries
 * the super-step counter.
 *
 * Some compiled flows project additional **flat top-level keys** for
 * ergonomic access — e.g. ``react_graph`` exposes a synthetic
 * ``output["final_response"]`` so the README quickstart can read the
 * model's last reply with one bracket. These projections coexist with
 * the channels wrapper; the wrapper is always present and is the
 * source of truth.
 *
 * Use the ``channel<T>(name)`` accessor below to read a channel value
 * without committing to either shape — it checks the channels wrapper
 * first, then falls back to a flat top-level key with the same name.
 * That way the same call site works against ``GraphEngine::build``-
 * built graphs (channels-wrapped only) and ``react_graph``-built
 * graphs (channels-wrapped + flat-key projections) alike.
 */
struct RunResult {
    json        output;                             ///< Final serialized graph state. See struct docstring for shape details.
    bool        interrupted       = false;          ///< True if execution was interrupted (HITL).
    std::string interrupt_node;                     ///< Name of the node that triggered the interrupt.
    json        interrupt_value;                    ///< Value associated with the interrupt.
    std::string checkpoint_id;                      ///< ID of the last checkpoint saved.
    std::vector<std::string> execution_trace;       ///< Ordered list of executed node names.

    /// Token usage for the whole run, subgraphs included (issue #88). Zero for
    /// a graph that made no LLM calls — not an error, just nothing to count.
    ///
    /// **This is what THIS call to run() spent, not the whole bill.** A previous
    /// attempt that threw never produced a RunResult, so its tokens are not
    /// here — even though they were really spent. Spend 15 tokens, crash, retry,
    /// spend 15 more, and this field says 15 against a bill of 30.
    ///
    /// For cost accounting that survives retries, hand the engine your own
    /// accumulator via ``RunConfig::usage``: it outlives the failed attempt
    /// because it is yours and the engine only borrows it.
    ///
    /// @see UsageAccounting.ACrashedAttemptIsInvisibleToRunResult
    /// @see UsageAccounting.ACallerSuppliedAccumulatorSeesTheCrashedAttempt
    ChatCompletion::Usage usage;

    /// True only when execution stopped at RunConfig::max_steps while work
    /// remained ready. The status is stored in output rather than as a data
    /// member so adding this query does not change RunResult's binary layout.
    inline bool max_steps_exhausted() const noexcept {
        if (!output.is_object()) return false;
        auto* metadata = yyjson_mut_obj_get(output.raw_val(), "_neograph");
        if (!yyjson_mut_is_obj(metadata)) return false;
        auto* marker = yyjson_mut_obj_get(metadata, "max_steps_exhausted");
        return yyjson_mut_is_bool(marker) && yyjson_mut_get_bool(marker);
    }

    /// @brief Return a typed outcome without changing the legacy result layout.
    RunStatus status() const noexcept {
        if (interrupted) return RunStatus::Interrupted;
        if (max_steps_exhausted()) return RunStatus::StepLimit;
        return RunStatus::Completed;
    }

    /// @brief Read a channel value as type ``T`` (issue #25).
    ///
    /// Looks up the value in ``output``. Tries the canonical
    /// channels-wrapped path first
    /// (``output["channels"][name]["value"]``); falls back to a flat
    /// top-level key (``output[name]``) for graphs that project flat
    /// keys such as ``react_graph``'s ``final_response``.
    ///
    /// @throws json::out_of_range if no channel with that name exists
    /// in either shape.
    /// @throws json::type_error if the channel exists but its value
    /// cannot be converted to ``T`` (delegated to ``json::get<T>``).
    ///
    /// @tparam T One of the types ``json::get<T>`` is specialised for
    ///         — ``int``, ``long``, ``double``, ``std::string``,
    ///         ``bool``, ``json``, etc. See ``json.h``.
    template <typename T>
    T channel(const std::string& name) const {
        return channel_raw(name).get<T>();
    }

    /// @brief Read a channel using a reusable typed key.
    template <typename T>
    T channel(const ChannelKey<T>& key) const {
        return channel<T>(key.name());
    }

    /// @brief Read a typed channel when present, preserving conversion errors.
    template <typename T>
    std::optional<T> try_channel(const ChannelKey<T>& key) const {
        if (!has_channel(key.name())) return std::nullopt;
        return channel(key);
    }

    /// @brief Read a channel value as a raw ``json`` node (issue #25).
    ///
    /// Same lookup rules as ``channel<T>`` — channels-wrapped first,
    /// flat top-level key as fallback. Returns the underlying json so
    /// the caller can walk arrays / objects without committing to a
    /// scalar conversion.
    ///
    /// @throws json::out_of_range if no channel with that name exists
    /// in either shape.
    json channel_raw(const std::string& name) const {
        if (output.contains("channels") && output["channels"].contains(name)) {
            const auto wrapped = output["channels"][name];
            if (wrapped.contains("value")) return wrapped["value"];
        }
        if (output.contains(name)) return output[name];
        throw json::out_of_range(
            "RunResult::channel: no such channel '" + name + "' in output "
            "(checked output[\"channels\"][\"" + name + "\"][\"value\"] and output[\"" + name + "\"])");
    }

    /// @brief Test whether a channel value exists in either shape.
    ///
    /// True if either the channels-wrapped path or a flat top-level
    /// key with the same name resolves. Useful as a guard before
    /// ``channel<T>`` when the graph may or may not have written the
    /// channel (e.g. early HITL interrupt before a node ran).
    bool has_channel(const std::string& name) const noexcept {
        if (output.contains("channels") && output["channels"].contains(name)
            && output["channels"][name].contains("value")) {
            return true;
        }
        return output.contains(name);
    }
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
 * After `build()` or `link()` returns, the graph definition (nodes, edges, channels)
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
 *   crash but produce unspecified checkpoint interleaving (last-
 *   writer-wins on `save_checkpoint`, last-saver visible to subsequent
 *   `load_latest`); serialize per-thread access yourself if you need
 *   deterministic semantics. Same caveat applies across **multiple
 *   engines** sharing one `CheckpointStore` for the same `thread_id`
 *   — e.g. evolving-agent patterns that tear an engine down and
 *   recompile while a straggler `run_async` is still in flight: the
 *   store guarantees each individual checkpoint op is atomic, not that
 *   they sequence in any particular order. Likewise, `update_state` /
 *   `get_state` / `fork` overlapping a live `run_async` on the same
 *   `thread_id` race against the engine's own writes; if you need
 *   "no straggler may overwrite my admin op", cancel the straggler
 *   via `RunConfig::cancel_token` first and `co_await` its completion
 *   before issuing the admin call.
 * - Custom `GraphNode` subclasses must be stateless or self-synchronized.
 *   Node instances are owned by the engine and shared across all runs.
 * - User-provided `CheckpointStore` / `Store` / `Provider` / `Tool`
 *   implementations must be thread-safe.
 *
 * @code
 * EngineConfig engine_config;
 * engine_config.node_context = context;
 * engine_config.checkpoint_store = checkpoint_store;
 * auto engine = GraphEngine::build(graph_json, std::move(engine_config));
 * RunConfig config;
 * config.thread_id = "session-1";
 * config.input = {{"messages", json::array({{{"role","user"},{"content","Hello"}}})}};
 * auto result = engine->run(config);
 * @endcode
 *
 * @see RunConfig, RunResult, GraphNode
 *
 * @note New code can obtain state-administration operations through admin().
 * The corresponding methods remain on GraphEngine for compatibility. Pass all
 * construction policy through EngineConfig and EngineResources; setters remain
 * supported but are configuration, not runtime control.
 */
class NEOGRAPH_API GraphEngine {
public:
    /**
     * @brief Link an already compiled graph into a configured runtime.
     *
     * This is the canonical runtime boundary for callers that explicitly run
     * GraphCompiler and inspect or transform the resulting CompiledGraph. It
     * performs semantic validation, consumes the graph by move, and applies
     * every EngineConfig option without parsing the source JSON again.
     *
     * Translation round-trip verification requires the original JSON and is
     * therefore performed by build() before it delegates here. Callers using
     * link() directly are responsible for calling GraphCompiler::verify_roundtrip
     * when they need that source-to-IR guarantee.
     *
     * EngineConfig::node_context is ignored because CompiledGraph already owns
     * its instantiated nodes. The remaining runtime configuration is applied.
     *
     * @param graph Compiled graph to consume.
     * @param config Runtime configuration applied before the engine is returned.
     * @return A configured GraphEngine ready for execution.
     * @throws std::runtime_error If semantic validation fails in strict mode.
     */
    static std::unique_ptr<GraphEngine> link(CompiledGraph graph, EngineConfig config = {});

    /**
     * @brief Link with owned tools and a per-engine registry overlay.
     *
     * For directly compiled graphs, the caller must have used tools.view() in
     * the NodeContext and the same registry in GraphCompiler::compile(). The
     * canonical build() overload performs both bindings automatically.
     */
    static std::unique_ptr<GraphEngine> link(CompiledGraph   graph,
                                             EngineConfig    config,
                                             EngineResources resources);

    /**
     * @brief Instantiate and link a topology already proven semantically valid.
     */
    static std::unique_ptr<GraphEngine> link(ValidatedTopology topology,
                                             EngineConfig config = {});

    /// @brief Validated topology link with owned resources and local registry.
    static std::unique_ptr<GraphEngine> link(ValidatedTopology topology,
                                             EngineConfig config,
                                             EngineResources resources);

    /**
     * @brief Build a ready-to-run engine from one construction-time config.
     *
     * New code should prefer this entry point when it needs runtime Store,
     * retry, worker, cache, or tool-gate configuration. It applies every option
     * before returning the engine, avoiding a partially configured interval.
     *
     * @param definition JSON graph definition.
     * @param config Node construction context and engine runtime configuration.
     * @return A configured GraphEngine ready for execution.
     * @throws std::runtime_error If the graph definition is invalid.
     */
    static std::unique_ptr<GraphEngine> build(const json& definition, EngineConfig config);

    /**
     * @brief Build with exact tool ownership and a local-first registry.
     *
     * resources.tools replaces NodeContext::tools with pointers to the owned
     * collection. Supplying both forms is rejected as ambiguous.
     */
    static std::unique_ptr<GraphEngine> build(const json&     definition,
                                               EngineConfig    config,
                                               EngineResources resources);

    /**
     * @brief Build using strict topology validation when schema_version is
     *        missing or zero.
     *
     * The caller's JSON is not mutated. Existing malformed or unsupported
     * schema_version values are preserved so the compiler reports them rather
     * than silently replacing them.
     */
    static std::unique_ptr<GraphEngine> build_strict(const json& definition,
                                                      EngineConfig config);

    /// @brief Strict build overload with owned tools and a local registry.
    static std::unique_ptr<GraphEngine> build_strict(const json&     definition,
                                                      EngineConfig    config,
                                                      EngineResources resources);

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
     *
     * @note Compatibility facade. This overload retains its original signature
     *       and behavior and delegates to build().
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

    /**
     * @brief Resume while preserving caller cancellation and deadline settings.
     *
     * `config.thread_id` identifies the interrupted session. Input is ignored;
     * the latest checkpoint remains the source of resumed graph state.
     */
    RunResult resume(const RunConfig&           config,
                     const json&                resume_value = json(),
                     const GraphStreamCallback& cb           = nullptr);

    /// Async peer of resume — non-blocking coroutine surface.
    asio::awaitable<RunResult> resume_async(
        const std::string& thread_id,
        const json& resume_value = json(),
        const GraphStreamCallback& cb = nullptr);

    /// Async peer of the RunConfig-preserving resume overload.
    asio::awaitable<RunResult> resume_async(RunConfig           config,
                                            json                resume_value = json(),
                                            GraphStreamCallback cb           = nullptr);

    /// @brief Borrow the state-administration facade for this engine.
    GraphAdmin admin() noexcept;

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
     * @brief Intercept every tool call before it runs (issue #89).
     *
     * A gate returns Allow (optionally rewriting the arguments), Deny (the
     * model is told why) or Interrupt (pause and ask a human). It is consulted
     * for every call in a batch **before any tool runs**, so an Interrupt has
     * no side effects to undo and the resumed node cannot double-apply a
     * sibling's effects. Reaches both dispatch paths — graph nodes and
     * llm::Agent — because they share one dispatcher (issue #87).
     *
     * **It lives on the engine, not on RunConfig, and that is deliberate.**
     * `resume()` builds its own RunConfig internally, so a per-run gate would
     * vanish the moment a human answered the very prompt it raised — the
     * dangerous tool would then run unchecked, with the caller believing they
     * had a permission system. A policy that disappears exactly when it is
     * being exercised is worse than no policy. Set it once, here, and it holds
     * for every run and every resume on this engine.
     *
     * @code
     * engine->set_tool_gate([](ToolCall call, ToolGateContext gctx)
     *         -> asio::awaitable<ToolDecision> {
     *     if (call.name != "shell") co_return ToolDecision::allow();
     *     if (!gctx.resume_value)
     *         co_return ToolDecision::interrupt("shell needs approval",
     *                                           json::parse(call.arguments));
     *     if (gctx.resume_value->value("approved", false))
     *         co_return ToolDecision::allow();
     *     co_return ToolDecision::deny("the human said no");
     * });
     * @endcode
     */
    void set_tool_gate(ToolGate gate) { tool_gate_ = std::move(gate); }

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
     * @brief Resize (or install) the engine-owned worker pool for
     *        parallel fan-out.
     *
     * **`compile()` default is `set_worker_count(1)`** — no
     * engine-owned thread pool, fan-out branches dispatch inline on
     * the coroutine's own executor. That keeps sequential and
     * single-`Send` workloads off the ~6-7 µs cross-thread submit
     * path, and avoids silently exposing nodes that hold
     * non-thread-safe state to concurrent execution. See
     * `docs/migration-v0.4-to-v1.0.md` (Migration 3) for the full
     * rationale.
     *
     * Call this with `n >= 2` to opt into a real engine-owned
     * `asio::thread_pool` for multi-`Send` fan-out or multi-outgoing
     * edges (e.g. `set_worker_count(4)` for a 4-way `Send`). For
     * `hardware_concurrency()` sizing, see `set_worker_count_auto()`.
     *
     * Must be called before any concurrent `run()`; resizing rebuilds
     * both the pool and the internal executor and is a hard error
     * against in-flight runs. Values < 1 are clamped to 1.
     *
     * @param n Number of worker threads in the fan-out pool.
     * @see set_worker_count_auto()
     */
    void set_worker_count(std::size_t n);

    /**
     * @brief Opt into a `hardware_concurrency()`-sized worker pool.
     *
     * Equivalent to
     * `set_worker_count(std::thread::hardware_concurrency())`, with a
     * fallback of 4 if the runtime cannot detect. **`compile()` does
     * NOT call this — its default is `set_worker_count(1)`.** Use
     * this once after `compile()` (and before any `run()`) to enable
     * real parallel fan-out for multi-`Send` / multi-outgoing-edge
     * workloads.
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

    static std::unique_ptr<GraphEngine> link_impl(CompiledGraph graph,
                                                   EngineConfig config,
                                                   EngineResources resources,
                                                   bool validate);

    /// #89 — set_tool_gate(). Lives on the engine rather than RunConfig so it
    /// survives resume(), which builds its own RunConfig internally.
    ToolGate tool_gate_;

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
        const json* resume_value = nullptr);

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
