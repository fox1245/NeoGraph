#include <neograph/graph/executor.h>
#include <neograph/graph/engine.h>   // RunContext (forward-declared in executor.h)
#include <neograph/graph/state.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/cancel.h>

#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <random>
#include <chrono>
#include <cstdio>
#include <optional>
#include <stdexcept>

namespace neograph::graph {

namespace {

// ── FNV-1a 64-bit for Send task_id hashing ─────────────────────────────
// Deterministic, 0 deps, sufficient for resume's replay map key.
// Moved here from graph_engine.cpp along with the Send execution path.
inline uint64_t fnv1a_64(const std::string& s) noexcept {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}
inline std::string fnv1a_hex(const std::string& s) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(fnv1a_64(s)));
    return std::string(buf, 16);
}

// Stable task_id for a static node in a given super-step. Used by
// run_one_async and run_parallel_async; both must agree on the
// scheme so resume replay matches across restarts.
inline std::string make_static_task_id(int step, const std::string& node_name) {
    return "s" + std::to_string(step) + ":" + node_name;
}

// Stable task_id for a Send: includes index + target + deterministic
// hash of the input payload so the same logical Send rehydrates under
// the same key on resume.
inline std::string make_send_task_id(int step, size_t idx,
                                      const std::string& target,
                                      const json& input) {
    return "s" + std::to_string(step) + ":send[" + std::to_string(idx)
           + "]:" + target + ":" + fnv1a_hex(input.dump());
}

} // namespace

// =========================================================================
// NodeExecutor construction + helpers
// =========================================================================

NodeExecutor::NodeExecutor(
    const std::map<std::string, std::unique_ptr<GraphNode>>& nodes,
    const std::vector<ChannelDef>& channel_defs,
    RetryPolicyLookup retry_policy_for,
    asio::thread_pool* fan_out_pool,
    NodeCache* node_cache)
    : nodes_(nodes),
      channel_defs_(channel_defs),
      retry_policy_for_(std::move(retry_policy_for)),
      fan_out_pool_(fan_out_pool),
      node_cache_(node_cache) {}

void NodeExecutor::init_state(GraphState& state) const {
    for (const auto& cd : channel_defs_) {
        auto reducer = ReducerRegistry::instance().get(cd.reducer_name);
        json initial = cd.initial_value;
        if (cd.type == ReducerType::APPEND && initial.is_null()) {
            initial = json::array();
        }
        state.init_channel(cd.name, cd.type, reducer, initial);
    }
}

void NodeExecutor::apply_input(GraphState& state, const json& input) const {
    if (!input.is_object()) return;
    auto known = state.channel_names();
    for (const auto& [key, value] : input.items()) {
        if (std::find(known.begin(), known.end(), key) != known.end()) {
            state.write(key, value);
        }
    }
}

// =========================================================================
// execute_node_with_retry_async: inner retry loop
// =========================================================================
//
// Drives node->execute_full_(stream_)async via co_await so a node
// whose execute_async issues real non-blocking I/O lets the executor
// serve other coroutines while this one is waiting. Backoff uses an
// asio::steady_timer.async_wait so retry waits do not freeze workers.
// NodeInterrupt and exception semantics match the pre-3.0 sync
// retry loop bit-for-bit; the GCC-13-safe shape (no co_await inside a
// catch block) requires capturing the result/error into std::optional
// inside try, then deciding what to do outside.

asio::awaitable<NodeResult> NodeExecutor::execute_node_with_retry_async(
    const std::string& node_name,
    GraphState& state,
    const GraphStreamCallback& cb,
    StreamMode stream_mode,
    const RunContext& ctx) {

    // PR 2 (v0.4.0): dispatch via the unified ``run(NodeInput)`` virtual.
    // ``ctx`` reaches the user-overridable surface for the first time
    // — new nodes read it directly, legacy nodes that don't override
    // ``run()`` get the default body forwarding to the existing 8-
    // virtual chain (priority order preserved). PR 1 plumbed ``ctx``
    // through every dispatch hop; this is where it lands at the
    // boundary the user actually sees.

    auto node_it = nodes_.find(node_name);
    if (node_it == nodes_.end()) {
        throw std::runtime_error("Node not found: " + node_name);
    }

    // Node-level cache: opt-in per node, only consulted when caller is
    // not streaming (cached hits cannot replay LLM_TOKEN events). Hash
    // is computed once per call so retries reuse the same lookup key.
    const bool cache_eligible =
        node_cache_ && !cb && node_cache_->is_enabled(node_name);
    std::string cache_state_hash;
    if (cache_eligible) {
        cache_state_hash = hash_state_for_cache(state.serialize());
        if (auto cached = node_cache_->lookup(node_name, cache_state_hash);
            cached) {
            co_return std::move(*cached);
        }
    }

    auto policy  = retry_policy_for_(node_name);
    int delay_ms = policy.initial_delay_ms;

    auto ex = co_await asio::this_coro::executor;

    for (int attempt = 0; attempt <= policy.max_retries; ++attempt) {
        if (cb && has_mode(stream_mode, StreamMode::EVENTS)) {
            json data;
            if (attempt > 0) data["retry_attempt"] = attempt;
            cb(GraphEvent{GraphEvent::Type::NODE_START, node_name, data});
        }

        // Capture the outcome of one attempt without co_await inside a
        // catch block (GCC 13 ICEs on that shape). NodeInterrupt is
        // re-thrown immediately so it short-circuits the loop just
        // like the sync path.
        std::optional<NodeResult> ok_result;
        std::exception_ptr  retryable_err;

        try {
            // PR 2: single dispatch point. NodeInput bundles the legacy
            // (state[, cb]) tuple plus the per-run RunContext. The
            // ``stream_cb`` field is null on the non-stream path so the
            // default ``run()`` routes through ``execute_full_async``;
            // non-null on the stream path so it routes through
            // ``execute_full_stream_async``. Both branches preserve
            // the legacy default-fallback chain bit-for-bit.
            NodeInput in{state, ctx, cb ? &cb : nullptr};
            ok_result.emplace(co_await node_it->second->run(in));
        } catch (const NodeInterrupt&) {
            // NodeInterrupt is a control-flow signal, not a retryable
            // error. Bubble out of the coroutine so the caller can save
            // a NodeInterrupt checkpoint, matching the sync path.
            throw;
        } catch (const CancelledException&) {
            // v0.3.2: cancel is also a control-flow signal — never
            // retry. Without this short-circuit a fan-out worker
            // whose Provider::complete throws CancelledException on
            // socket abort would re-enter the retry loop, fresh
            // run_sync would race emit-vs-bind on the already-set
            // cancel flag, the second HTTP call would slip through,
            // and the cost leak would persist for max_retries × ~3 s.
            throw;
        } catch (const std::bad_alloc&) {
            // OOM is not retryable. Sleeping then re-running won't
            // free memory; it just delays the inevitable failure
            // and risks compounding allocator pressure.
            throw;
        } catch (const std::logic_error&) {
            // logic_error and its subclasses (invalid_argument,
            // out_of_range, length_error, domain_error, future_error)
            // signal programming bugs — assertions, contract
            // violations. Retrying produces the same bug. Surface
            // immediately so the bug shows up loud rather than as
            // a slow retry-exhaustion timeout.
            throw;
        } catch (...) {
            // runtime_error, network/timeout errors, third-party
            // exceptions: assume transient and let the retry loop
            // decide based on attempt count + policy.
            retryable_err = std::current_exception();
        }

        if (ok_result) {
            auto& nr = *ok_result;
            if (cb) {
                if (has_mode(stream_mode, StreamMode::UPDATES)) {
                    for (const auto& w : nr.writes) {
                        cb(GraphEvent{GraphEvent::Type::CHANNEL_WRITE, node_name,
                                      json{{"channel", w.channel}, {"value", w.value}}});
                    }
                }
                if (has_mode(stream_mode, StreamMode::EVENTS)) {
                    json end_data;
                    if (nr.command)
                        end_data["command_goto"] = nr.command->goto_node;
                    if (!nr.sends.empty())
                        end_data["sends"] = (int)nr.sends.size();
                    cb(GraphEvent{GraphEvent::Type::NODE_END, node_name, end_data});
                }
            }
            if (cache_eligible) {
                node_cache_->store(node_name, cache_state_hash, nr);
            }
            co_return std::move(nr);
        }

        // Retry path. retryable_err must be populated since neither the
        // result nor a NodeInterrupt path was taken.
        if (attempt >= policy.max_retries) {
            if (cb && has_mode(stream_mode, StreamMode::EVENTS)) {
                std::string what;
                try { std::rethrow_exception(retryable_err); }
                catch (const std::exception& e) { what = e.what(); }
                catch (...) { what = "unknown"; }
                cb(GraphEvent{GraphEvent::Type::ERROR, node_name,
                              json{{"error", what}, {"attempts", attempt + 1}}});
            }
            std::rethrow_exception(retryable_err);
        }

        if (cb && has_mode(stream_mode, StreamMode::DEBUG)) {
            std::string what;
            try { std::rethrow_exception(retryable_err); }
            catch (const std::exception& e) { what = e.what(); }
            catch (...) { what = "unknown"; }
            cb(GraphEvent{GraphEvent::Type::ERROR, node_name,
                          json{{"retry", attempt + 1},
                               {"max_retries", policy.max_retries},
                               {"delay_ms", delay_ms},
                               {"error", what}}});
        }

        // Apply jitter to break thundering-herd lockstep when N
        // parallel branches retry against the same upstream. The
        // policy default is jitter_pct=0 (deterministic, back-compat);
        // callers configuring fan-out workloads should set ~0.2.
        int actual_delay = delay_ms;
        if (policy.jitter_pct > 0.0f && delay_ms > 0) {
            // Per-thread RNG so concurrent branches don't share state.
            thread_local std::mt19937 rng{std::random_device{}()};
            float pct = std::min(1.0f, policy.jitter_pct);
            std::uniform_real_distribution<float> dist(-pct, pct);
            float scaled = delay_ms * (1.0f + dist(rng));
            if (scaled < 0.0f) scaled = 0.0f;
            actual_delay = static_cast<int>(scaled);
        }

        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds(actual_delay));
        co_await timer.async_wait(asio::use_awaitable);

        delay_ms = std::min(
            static_cast<int>(delay_ms * policy.backoff_multiplier),
            policy.max_delay_ms);
    }

    throw std::runtime_error(
        "Unreachable: async retry loop exited without return or throw");
}

// =========================================================================
// run_one_async: single-node path
// =========================================================================

asio::awaitable<NodeResult> NodeExecutor::run_one_async(
    const std::string& node_name,
    int step,
    GraphState& state,
    const std::unordered_map<std::string, NodeResult>& replay,
    CheckpointCoordinator& coord,
    const std::string& parent_cp_id,
    const BarrierState& barrier_state,
    std::vector<std::string>& trace,
    const GraphStreamCallback& cb,
    StreamMode stream_mode,
    const RunContext& ctx) {

    const std::string task_id = make_static_task_id(step, node_name);

    // GCC-13-safe outcome capture: collect result or NodeInterrupt
    // marker inside try, decide outside. Other exceptions propagate
    // out of the coroutine untouched.
    std::optional<NodeResult> ok_result;
    bool interrupted = false;

    try {
        auto replay_it = replay.find(task_id);
        if (replay_it != replay.end()) {
            // Replayed from a previous partial run — do NOT re-execute,
            // do NOT re-record. Just apply the recorded writes.
            ok_result.emplace(replay_it->second);
        } else {
            ok_result.emplace(co_await execute_node_with_retry_async(
                node_name, state, cb, stream_mode, ctx));

            // Record BEFORE apply_writes so a crash between the two
            // still leaves a durable log for resume to replay.
            co_await coord.record_pending_write_async(parent_cp_id,
                task_id, task_id, node_name, *ok_result, step);
        }
    } catch (const NodeInterrupt&) {
        interrupted = true;
    }

    if (interrupted) {
        // NodeInterrupt pauses this specific node — resume must
        // re-enter exactly here. Save a phase=NodeInterrupt cp with
        // next_nodes={node_name} so load_for_resume restarts the super
        // step at this step with only this node ready.
        // GCC-13 ICE workaround: build the next_nodes vector outside
        // the co_await arg list (nested brace-init in a coroutine
        // body trips build_special_member_call).
        std::vector<std::string> next_nodes;
        next_nodes.push_back(node_name);
        co_await coord.save_super_step_async(state,
            node_name, next_nodes,
            CheckpointPhase::NodeInterrupt, step, parent_cp_id,
            barrier_state);
        throw NodeInterrupt(node_name);
    }

    auto& nr = *ok_result;
    state.apply_writes(nr.writes);
    if (nr.command) {
        state.apply_writes(nr.command->updates);
    }

    trace.push_back(node_name);
    co_return std::move(nr);
}

// =========================================================================
// run_parallel_async: fan-out via make_parallel_group
// =========================================================================
//
// Uses asio::experimental::make_parallel_group on a vector of
// co_spawn-deferred workers. wait_for_all gives back a
// (completion_order, exception_ptrs, results) triple — we ignore
// order (apply writes in ready-order so scheduler pairs ready[i] ↔
// results[i]) and fold exceptions down to first_exception.
//
// GCC-13-safe: no co_await inside a catch; no nested brace-init in
// the coroutine body. The first-exception classifier uses a separate
// try/catch to tag NodeInterrupt; the cp save happens outside.

asio::awaitable<std::vector<NodeResult>>
NodeExecutor::run_parallel_async(
    const std::vector<std::string>& ready,
    int step,
    GraphState& state,
    const std::unordered_map<std::string, NodeResult>& replay,
    CheckpointCoordinator& coord,
    const std::string& parent_cp_id,
    const BarrierState& barrier_state,
    std::vector<std::string>& trace,
    const GraphStreamCallback& cb,
    StreamMode stream_mode,
    const RunContext& ctx) {

    // Single-ready bypass: skip the parallel_group machinery entirely.
    // When the super-step has only one ready node, building deferred
    // ops, allocating excs/values/order vectors, and wrapping in
    // wait_for_all is pure overhead. Run the worker inline on the
    // outer executor instead. Hit by every linear-chain step (seq
    // bench's 3-step path, par bench's summarizer fan-in step) and
    // by any super-step where conditional routing collapses to one
    // target.
    if (ready.size() == 1) {
        const auto& node_name = ready[0];
        const std::string task_id = make_static_task_id(step, node_name);
        auto replay_it = replay.find(task_id);
        NodeResult nr;
        std::exception_ptr interrupt_exc;
        if (replay_it != replay.end()) {
            nr = replay_it->second;
        } else {
            // GCC-13: cannot co_await inside a catch handler. Classify
            // here, save + rethrow outside the try/catch.
            try {
                nr = co_await execute_node_with_retry_async(
                    node_name, state, cb, stream_mode, ctx);
            } catch (const NodeInterrupt&) {
                interrupt_exc = std::current_exception();
            }
            if (interrupt_exc) {
                std::vector<std::string> next_nodes;
                next_nodes.push_back(node_name);
                co_await coord.save_super_step_async(state,
                    node_name, next_nodes,
                    CheckpointPhase::NodeInterrupt, step, parent_cp_id,
                    barrier_state);
                std::rethrow_exception(interrupt_exc);
            }
            co_await coord.record_pending_write_async(
                parent_cp_id, task_id, task_id, node_name, nr, step);
        }
        state.apply_writes(nr.writes);
        if (nr.command) state.apply_writes(nr.command->updates);
        trace.push_back(node_name);
        std::vector<NodeResult> result;
        result.push_back(std::move(nr));
        co_return result;
    }

    auto outer_ex = co_await asio::this_coro::executor;
    // Branches dispatch to the engine's owned pool when available so
    // CPU-bound fan-out actually parallelizes even if the outer
    // coroutine is driven on a single-threaded executor (e.g. sync
    // run() routing through run_sync). With no pool, branches share
    // the outer executor — matches the single-threaded contract the
    // async peer tests assert.
    asio::any_io_executor branch_ex = fan_out_pool_
        ? asio::any_io_executor(fan_out_pool_->get_executor())
        : asio::any_io_executor(outer_ex);

    // Per-branch worker. Captures by ref — all captured refs outlive
    // co_await below (stack scope), and state reads are
    // shared_mutex-guarded inside GraphState. No worker writes to
    // shared state; the NodeResults are aggregated out-of-band by the
    // parallel_group and applied in-order after wait_for_all returns.
    // INVARIANT (capture-by-reference safety): the outer awaitable
    // suspends at make_parallel_group::async_wait below and stays
    // suspended until every worker finishes. The captured locals
    // (state, replay, coord, parent_cp_id, trace, cb, stream_mode)
    // therefore live for the full lifetime of every worker frame.
    // No worker mutates the captured state — NodeResult outputs are
    // aggregated out-of-band and applied in-order after wait_for_all
    // returns. If a future change introduces opportunistic
    // cancellation that abandons the parallel_group early, switch
    // these captures to by-value copies first.
    auto worker = [&, this](std::string node_name) -> asio::awaitable<NodeResult> {
        const std::string task_id = make_static_task_id(step, node_name);
        auto replay_it = replay.find(task_id);
        if (replay_it != replay.end()) {
            co_return replay_it->second;
        }
        auto nr = co_await execute_node_with_retry_async(
            node_name, state, cb, stream_mode, ctx);
        co_await coord.record_pending_write_async(
            parent_cp_id, task_id, task_id, node_name, nr, step);
        co_return nr;
    };

    // Build deferred ops, one per ready node. co_spawn-with-deferred
    // returns an op that, when awaited, runs the worker coroutine and
    // completes with (exception_ptr, NodeResult).
    using DeferredOp = decltype(asio::co_spawn(
        branch_ex, worker(std::declval<std::string>()), asio::deferred));
    std::vector<DeferredOp> ops;
    ops.reserve(ready.size());
    for (const auto& node_name : ready) {
        ops.push_back(asio::co_spawn(branch_ex, worker(node_name), asio::deferred));
    }

    // wait_for_all returns:
    //   completion_order : std::vector<std::size_t>
    //   excs             : std::vector<std::exception_ptr>  (per-branch)
    //   values           : std::vector<NodeResult>          (per-branch)
    auto [order, excs, values] = co_await asio::experimental::make_parallel_group(
        std::move(ops))
        .async_wait(asio::experimental::wait_for_all(),
                    asio::use_awaitable);
    (void)order;  // we apply in ready-order regardless of completion-order

    // Find first exception + classify. NodeInterrupt gets a dedicated
    // cp-save with next_nodes={offender} so resume re-enters on just
    // that node (siblings' pending writes are already recorded).
    //
    // TSan-safe NodeInterrupt path: extract the reason as a plain
    // ``std::string`` while we still hold the exception_ptr, then
    // throw a *fresh* NodeInterrupt on the main thread. The original
    // worker-allocated eptr (and its libstdc++-internal atomic
    // refcount) never crosses the ``co_await save_super_step_async``
    // suspend boundary — that's where TSan was flagging a benign
    // libstdc++ ``__exception_ptr::_M_release`` race against the
    // worker thread's eptr destructor (see
    // ``feedback_parallel_group_eptr_race.md``). Non-interrupt
    // exceptions still rethrow the original eptr on the same thread
    // they were classified, no co_await between, so no race window.
    std::exception_ptr first_exception;
    std::string        first_exception_node;
    bool               is_node_interrupt = false;
    std::string        ni_reason;
    for (std::size_t i = 0; i < ready.size(); ++i) {
        if (excs[i] && !first_exception) {
            first_exception      = excs[i];
            first_exception_node = ready[i];
            try {
                std::rethrow_exception(first_exception);
            } catch (const NodeInterrupt& ni) {
                is_node_interrupt = true;
                ni_reason = ni.reason();
            } catch (...) {
                // non-interrupt; leave is_node_interrupt = false
            }
        }
    }

    if (first_exception) {
        if (is_node_interrupt) {
            // GCC-13 workaround: build the next_nodes vector outside
            // the co_await arg list (nested brace-init trips the ICE).
            std::vector<std::string> next_nodes;
            next_nodes.push_back(first_exception_node);
            co_await coord.save_super_step_async(state,
                first_exception_node, next_nodes,
                CheckpointPhase::NodeInterrupt, step, parent_cp_id,
                barrier_state);
            // Drop the worker eptr and throw a fresh NodeInterrupt —
            // see comment above the loop. The fresh exception is
            // wholly owned by the current (main) thread.
            first_exception = nullptr;
            throw NodeInterrupt(ni_reason);
        }
        std::rethrow_exception(first_exception);
    }

    // Apply results in ready-order (scheduler pairs ready[i] ↔ results[i]).
    std::vector<NodeResult> step_results;
    step_results.reserve(ready.size());
    for (std::size_t i = 0; i < ready.size(); ++i) {
        state.apply_writes(values[i].writes);
        if (values[i].command) {
            state.apply_writes(values[i].command->updates);
        }
        step_results.push_back(std::move(values[i]));
        trace.push_back(ready[i]);
    }
    co_return step_results;
}

// =========================================================================
// run_sends_async: single-send + multi-send dynamic fan-out
// =========================================================================
//
// Two-branch shape:
//   * single Send — sequential on the shared state, with retry.
//   * multi Send — make_parallel_group on deferred workers, each
//     with its own isolated GraphState copy. Results fan back to
//     the shared state after wait_for_all.
//
// GCC-13-safe: no co_await inside a catch. The multi-send worker
// captures exceptions into the parallel_group's excs array; the
// first exception (if any) is rethrown after the wait resolves.

asio::awaitable<std::vector<StepRouting>> NodeExecutor::run_sends_async(
    const std::vector<Send>& sends,
    int step,
    GraphState& state,
    const std::unordered_map<std::string, NodeResult>& replay,
    CheckpointCoordinator& coord,
    const std::string& parent_cp_id,
    std::vector<std::string>& trace,
    const GraphStreamCallback& cb,
    StreamMode stream_mode,
    const RunContext& ctx) {

    std::vector<StepRouting> routings;
    if (sends.empty()) co_return routings;

    if (cb && has_mode(stream_mode, StreamMode::DEBUG)) {
        json send_info = json::array();
        for (const auto& s : sends) {
            json item;
            item["target"] = s.target_node;
            item["input"]  = s.input;
            send_info.push_back(item);
        }
        json data;
        data["sends"] = send_info;
        cb(GraphEvent{GraphEvent::Type::NODE_START, "__send__", data});
    }

    // --- Single Send: sequential on shared state, with retry. ---
    if (sends.size() == 1) {
        const auto& s = sends[0];
        auto node_it = nodes_.find(s.target_node);
        if (node_it == nodes_.end()) co_return routings;

        const std::string task_id = make_send_task_id(
            step, 0, s.target_node, s.input);
        NodeResult nr;

        auto replay_it = replay.find(task_id);
        if (replay_it != replay.end()) {
            nr = replay_it->second;
        } else {
            apply_input(state, s.input);
            nr = co_await execute_node_with_retry_async(
                s.target_node, state, cb, stream_mode, ctx);
            co_await coord.record_pending_write_async(parent_cp_id,
                task_id, task_id, s.target_node, nr, step);
        }
        state.apply_writes(nr.writes);
        if (nr.command) state.apply_writes(nr.command->updates);
        trace.push_back(s.target_node + "[send]");

        StepRouting r;
        r.node_name = s.target_node;
        if (nr.command && !nr.command->goto_node.empty()) {
            r.command_goto = nr.command->goto_node;
        }
        routings.push_back(std::move(r));
        co_return routings;
    }

    // --- Multi Send: isolated per-target state + parallel_group. ---
    //
    // Each worker coroutine builds its own GraphState from a
    // serialize/restore round-trip, applies the Send's input, then
    // drives execute_node_with_retry_async. Isolated state can't be
    // shared across concurrent Sends — each gets its own copy.
    //
    // PERF: serialize the source state once and have every worker
    // restore from the same snapshot. Pre-fix every worker called
    // state.serialize() inside its lambda — N workers paid N full
    // serializations of the entire channel set, which dominated the
    // par micro-bench (callgrind: yyjson_mut_doc_new + val_pool_grow
    // + mut_val_mut_copy = ~9% of total instructions). The state is
    // logically immutable across the worker batch (state mutations
    // happen post-join, line ~568 below) so a single snapshot is
    // safe to share.
    auto state_snapshot = state.serialize();
    auto outer_ex = co_await asio::this_coro::executor;
    asio::any_io_executor ex = fan_out_pool_
        ? asio::any_io_executor(fan_out_pool_->get_executor())
        : asio::any_io_executor(outer_ex);

    auto worker = [&, this](std::size_t si) -> asio::awaitable<NodeResult> {
        const auto& s = sends[si];
        auto node_it = nodes_.find(s.target_node);
        if (node_it == nodes_.end()) {
            co_return NodeResult{};  // silently skip missing target
        }

        const std::string task_id = make_send_task_id(
            step, si, s.target_node, s.input);

        auto replay_it = replay.find(task_id);
        if (replay_it != replay.end()) {
            co_return replay_it->second;
        }

        GraphState send_state;
        init_state(send_state);
        send_state.restore(state_snapshot);
        apply_input(send_state, s.input);
        // v0.3.1: serialize/restore drops the per-run cancel token (it
        // lives outside the channel set), so forward the parent's token
        // explicitly. Without this, a Python node inside a multi-Send
        // worker would see a null `state.run_cancel_token()` and the
        // node-installed CurrentCancelTokenScope would propagate
        // nothing — a cancelled run would still leak LLM cost on the
        // dynamic fan-out branches. Single-Send / static-parallel
        // paths read the parent state directly, so they were already
        // covered.
        send_state.set_run_cancel_token(state.run_cancel_token_shared());

        auto nr = co_await execute_node_with_retry_async(
            s.target_node, send_state, cb, stream_mode, ctx);
        co_await coord.record_pending_write_async(parent_cp_id,
            task_id, task_id, s.target_node, nr, step);
        co_return nr;
    };

    using DeferredOp = decltype(asio::co_spawn(
        ex, worker(std::size_t{0}), asio::deferred));
    std::vector<DeferredOp> ops;
    ops.reserve(sends.size());
    for (std::size_t si = 0; si < sends.size(); ++si) {
        ops.push_back(asio::co_spawn(ex, worker(si), asio::deferred));
    }

    auto [order, excs, values] = co_await asio::experimental::make_parallel_group(
        std::move(ops))
        .async_wait(asio::experimental::wait_for_all(),
                    asio::use_awaitable);
    (void)order;

    // First-exception pass — same shape as run_parallel_async, but
    // a Send's NodeInterrupt does not emit a dedicated cp: Send
    // targets are fan-out one-shots without an obvious resume
    // scope, so interrupts propagate for the caller to surface.
    for (std::size_t i = 0; i < sends.size(); ++i) {
        if (excs[i]) {
            std::rethrow_exception(excs[i]);
        }
    }

    // Fan back in send order. Each Send-spawned task contributes one
    // StepRouting so its `Command.goto` and / or default outgoing edges
    // flow into the next super-step's routing decision.
    routings.reserve(sends.size());
    for (std::size_t si = 0; si < sends.size(); ++si) {
        state.apply_writes(values[si].writes);
        if (values[si].command) state.apply_writes(values[si].command->updates);
        trace.push_back(sends[si].target_node + "[send]");

        StepRouting r;
        r.node_name = sends[si].target_node;
        if (values[si].command && !values[si].command->goto_node.empty()) {
            r.command_goto = values[si].command->goto_node;
        }
        routings.push_back(std::move(r));
    }
    co_return routings;
}

} // namespace neograph::graph
