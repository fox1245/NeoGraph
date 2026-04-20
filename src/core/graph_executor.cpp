#include <neograph/graph/executor.h>
#include <neograph/graph/state.h>
#include <neograph/graph/loader.h>

#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <taskflow/taskflow.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

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
// run_one and run_parallel; both must agree on the scheme so resume
// replay matches across restarts.
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

// Process-wide Taskflow executor for parallel fan-out / multi-send.
// Kept as a static local so all NodeExecutor instances share one pool.
tf::Executor& global_executor() {
    static tf::Executor exec;
    return exec;
}

} // namespace

// =========================================================================
// NodeExecutor construction + helpers
// =========================================================================

NodeExecutor::NodeExecutor(
    const std::map<std::string, std::unique_ptr<GraphNode>>& nodes,
    const std::vector<ChannelDef>& channel_defs,
    RetryPolicyLookup retry_policy_for)
    : nodes_(nodes),
      channel_defs_(channel_defs),
      retry_policy_for_(std::move(retry_policy_for)) {}

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
// execute_node_with_retry: inner retry loop
// =========================================================================

NodeResult NodeExecutor::execute_node_with_retry(
    const std::string& node_name,
    GraphState& state,
    const GraphStreamCallback& cb,
    StreamMode stream_mode) {

    auto node_it = nodes_.find(node_name);
    if (node_it == nodes_.end()) {
        throw std::runtime_error("Node not found: " + node_name);
    }

    auto policy  = retry_policy_for_(node_name);
    int delay_ms = policy.initial_delay_ms;

    for (int attempt = 0; attempt <= policy.max_retries; ++attempt) {
        try {
            if (cb && has_mode(stream_mode, StreamMode::EVENTS)) {
                json data;
                if (attempt > 0) data["retry_attempt"] = attempt;
                cb(GraphEvent{GraphEvent::Type::NODE_START, node_name, data});
            }

            auto node_result = cb
                ? node_it->second->execute_full_stream(state, cb)
                : node_it->second->execute_full(state);

            if (cb) {
                if (has_mode(stream_mode, StreamMode::UPDATES)) {
                    for (const auto& w : node_result.writes) {
                        cb(GraphEvent{GraphEvent::Type::CHANNEL_WRITE, node_name,
                                      json{{"channel", w.channel}, {"value", w.value}}});
                    }
                }
                if (has_mode(stream_mode, StreamMode::EVENTS)) {
                    json end_data;
                    if (node_result.command)
                        end_data["command_goto"] = node_result.command->goto_node;
                    if (!node_result.sends.empty())
                        end_data["sends"] = (int)node_result.sends.size();
                    cb(GraphEvent{GraphEvent::Type::NODE_END, node_name, end_data});
                }
            }

            return node_result;

        } catch (const NodeInterrupt&) {
            // NodeInterrupt is a control-flow signal, not a retryable
            // error. Short-circuit the retry loop and let the caller
            // (run_one / run_parallel) decide whether to save a
            // NodeInterrupt checkpoint.
            throw;

        } catch (const std::exception& e) {
            if (attempt >= policy.max_retries) {
                if (cb && has_mode(stream_mode, StreamMode::EVENTS)) {
                    cb(GraphEvent{GraphEvent::Type::ERROR, node_name,
                                  json{{"error", e.what()}, {"attempts", attempt + 1}}});
                }
                throw;
            }

            if (cb && has_mode(stream_mode, StreamMode::DEBUG)) {
                cb(GraphEvent{GraphEvent::Type::ERROR, node_name,
                              json{{"retry", attempt + 1},
                                   {"max_retries", policy.max_retries},
                                   {"delay_ms", delay_ms},
                                   {"error", e.what()}}});
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(
                static_cast<int>(delay_ms * policy.backoff_multiplier),
                policy.max_delay_ms);
        }
    }

    throw std::runtime_error("Unreachable: retry loop exited without return or throw");
}

// =========================================================================
// execute_node_with_retry_async: coroutine peer (Sem 3.6 incremental)
// =========================================================================
//
// Mirrors the sync retry loop above but:
//   * dispatches to node->execute_full_(stream_)async via co_await,
//     so a node whose execute_async issues real non-blocking I/O lets
//     the io_context serve other coroutines while this one is waiting.
//   * replaces std::this_thread::sleep_for with an asio::steady_timer
//     async_wait — backoff no longer freezes the worker.
//   * NodeInterrupt and exception semantics are preserved bit-for-bit
//     against the sync path; the GCC-13-safe shape (no co_await inside
//     a catch block) requires capturing the result/error into
//     std::optional inside try, then deciding what to do outside.

asio::awaitable<NodeResult> NodeExecutor::execute_node_with_retry_async(
    const std::string& node_name,
    GraphState& state,
    const GraphStreamCallback& cb,
    StreamMode stream_mode) {

    auto node_it = nodes_.find(node_name);
    if (node_it == nodes_.end()) {
        throw std::runtime_error("Node not found: " + node_name);
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
            if (cb) {
                ok_result.emplace(co_await node_it->second
                    ->execute_full_stream_async(state, cb));
            } else {
                ok_result.emplace(co_await node_it->second
                    ->execute_full_async(state));
            }
        } catch (const NodeInterrupt&) {
            // NodeInterrupt is a control-flow signal, not a retryable
            // error. Bubble out of the coroutine so the caller can save
            // a NodeInterrupt checkpoint, matching the sync path.
            throw;
        } catch (...) {
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

        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds(delay_ms));
        co_await timer.async_wait(asio::use_awaitable);

        delay_ms = std::min(
            static_cast<int>(delay_ms * policy.backoff_multiplier),
            policy.max_delay_ms);
    }

    throw std::runtime_error(
        "Unreachable: async retry loop exited without return or throw");
}

// =========================================================================
// run_one: single-node path
// =========================================================================

NodeResult NodeExecutor::run_one(
    const std::string& node_name,
    int step,
    GraphState& state,
    const std::unordered_map<std::string, NodeResult>& replay,
    CheckpointCoordinator& coord,
    const std::string& parent_cp_id,
    const BarrierState& barrier_state,
    std::vector<std::string>& trace,
    const GraphStreamCallback& cb,
    StreamMode stream_mode) {

    try {
        const std::string task_id = make_static_task_id(step, node_name);
        NodeResult nr;

        auto replay_it = replay.find(task_id);
        if (replay_it != replay.end()) {
            // Replayed from a previous partial run — do NOT re-execute,
            // do NOT re-record. Just apply the recorded writes.
            nr = replay_it->second;
        } else {
            nr = execute_node_with_retry(node_name, state, cb, stream_mode);

            // Record BEFORE apply_writes so a crash between the two
            // still leaves a durable log for resume to replay.
            coord.record_pending_write(parent_cp_id,
                task_id, task_id, node_name, nr, step);
        }

        state.apply_writes(nr.writes);
        if (nr.command) {
            state.apply_writes(nr.command->updates);
        }

        trace.push_back(node_name);
        return nr;

    } catch (const NodeInterrupt& ni) {
        // NodeInterrupt pauses this specific node — resume must
        // re-enter exactly here. Save a phase=NodeInterrupt cp with
        // next_nodes={node_name} so load_for_resume restarts the super
        // step at this step with only this node ready.
        coord.save_super_step(state,
            node_name, std::vector<std::string>{node_name},
            CheckpointPhase::NodeInterrupt, step, parent_cp_id,
            barrier_state);
        throw;
    }
}

// =========================================================================
// run_one_async: coroutine peer (Sem 3.6 incremental Step 2)
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
    StreamMode stream_mode) {

    const std::string task_id = make_static_task_id(step, node_name);

    // GCC-13-safe outcome capture: collect result or NodeInterrupt
    // marker inside try, decide outside. Other exceptions propagate
    // out of the coroutine the same way the sync run_one lets them.
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
                node_name, state, cb, stream_mode));

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
// run_parallel: Taskflow fan-out
// =========================================================================

std::vector<NodeResult> NodeExecutor::run_parallel(
    const std::vector<std::string>& ready,
    int step,
    GraphState& state,
    const std::unordered_map<std::string, NodeResult>& replay,
    CheckpointCoordinator& coord,
    const std::string& parent_cp_id,
    const BarrierState& barrier_state,
    std::vector<std::string>& trace,
    const GraphStreamCallback& cb,
    StreamMode stream_mode) {

    std::map<std::string, NodeResult> all_results;
    std::mutex results_mutex;
    std::exception_ptr first_exception;
    // Captured alongside first_exception so a NodeInterrupt from any
    // parallel worker can still produce the same narrow resume
    // (phase=NodeInterrupt, next_nodes={this_one}) that run_one does.
    std::string first_exception_node;

    tf::Taskflow taskflow("fan-out");
    for (const auto& node_name : ready) {
        taskflow.emplace([&, node_name]() {
            try {
                const std::string task_id = make_static_task_id(step, node_name);
                NodeResult nr;

                auto replay_it = replay.find(task_id);
                if (replay_it != replay.end()) {
                    nr = replay_it->second;
                } else {
                    nr = execute_node_with_retry(node_name, state, cb, stream_mode);
                    coord.record_pending_write(parent_cp_id,
                        task_id, task_id, node_name, nr, step);
                }

                std::lock_guard lock(results_mutex);
                all_results[node_name] = std::move(nr);
            } catch (...) {
                std::lock_guard lock(results_mutex);
                if (!first_exception) {
                    first_exception = std::current_exception();
                    first_exception_node = node_name;
                }
            }
        }).name(node_name);
    }
    global_executor().run(taskflow).wait();

    if (first_exception) {
        // If this is a NodeInterrupt, match run_one's contract: save a
        // phase=NodeInterrupt cp scoped to just the interrupting node
        // BEFORE rethrowing. Successful siblings have already recorded
        // pending writes against parent_cp_id, so resume's replay map
        // will skip them and only the throwing node will re-execute.
        try {
            std::rethrow_exception(first_exception);
        } catch (const NodeInterrupt&) {
            coord.save_super_step(state,
                first_exception_node,
                std::vector<std::string>{first_exception_node},
                CheckpointPhase::NodeInterrupt, step, parent_cp_id,
                barrier_state);
            throw;
        }
        // Non-NodeInterrupt: propagate as-is, no cp save (matches
        // non-retry failure of run_one, which also doesn't save).
    }

    // Apply results in ready order so caller can pair them 1:1 with
    // Scheduler's routing input (ready[i] ↔ step_results[i]).
    std::vector<NodeResult> step_results;
    step_results.reserve(ready.size());
    for (const auto& node_name : ready) {
        auto it = all_results.find(node_name);
        if (it != all_results.end()) {
            state.apply_writes(it->second.writes);
            if (it->second.command)
                state.apply_writes(it->second.command->updates);
            step_results.push_back(std::move(it->second));
        }
        trace.push_back(node_name);
    }
    return step_results;
}

// =========================================================================
// run_parallel_async: coroutine peer (Sem 3.7)
// =========================================================================
//
// Uses asio::experimental::make_parallel_group on a vector of
// co_spawn-deferred workers. wait_for_all gives back a
// (completion_order, exception_ptrs, results) triple — we ignore
// order (apply writes in ready-order, matching sync run_parallel)
// and fold exceptions down to first_exception, mirroring the sync
// taskflow contract.
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
    StreamMode stream_mode) {

    auto ex = co_await asio::this_coro::executor;

    // Per-branch worker. Captures by ref — all captured refs outlive
    // co_await below (stack scope), and state reads are
    // shared_mutex-guarded inside GraphState. No worker writes to
    // shared state; the NodeResults are aggregated out-of-band by the
    // parallel_group and applied in-order after wait_for_all returns.
    auto worker = [&, this](std::string node_name) -> asio::awaitable<NodeResult> {
        const std::string task_id = make_static_task_id(step, node_name);
        auto replay_it = replay.find(task_id);
        if (replay_it != replay.end()) {
            co_return replay_it->second;
        }
        auto nr = co_await execute_node_with_retry_async(
            node_name, state, cb, stream_mode);
        co_await coord.record_pending_write_async(
            parent_cp_id, task_id, task_id, node_name, nr, step);
        co_return nr;
    };

    // Build deferred ops, one per ready node. co_spawn-with-deferred
    // returns an op that, when awaited, runs the worker coroutine and
    // completes with (exception_ptr, NodeResult).
    using DeferredOp = decltype(asio::co_spawn(
        ex, worker(std::declval<std::string>()), asio::deferred));
    std::vector<DeferredOp> ops;
    ops.reserve(ready.size());
    for (const auto& node_name : ready) {
        ops.push_back(asio::co_spawn(ex, worker(node_name), asio::deferred));
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
    std::exception_ptr first_exception;
    std::string        first_exception_node;
    bool               is_node_interrupt = false;
    for (std::size_t i = 0; i < ready.size(); ++i) {
        if (excs[i] && !first_exception) {
            first_exception      = excs[i];
            first_exception_node = ready[i];
            try {
                std::rethrow_exception(first_exception);
            } catch (const NodeInterrupt&) {
                is_node_interrupt = true;
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
// run_sends: single-send (retry on shared state) + multi-send (isolated)
// =========================================================================

void NodeExecutor::run_sends(
    const std::vector<Send>& sends,
    int step,
    GraphState& state,
    const std::unordered_map<std::string, NodeResult>& replay,
    CheckpointCoordinator& coord,
    const std::string& parent_cp_id,
    std::vector<std::string>& trace,
    const GraphStreamCallback& cb,
    StreamMode stream_mode) {

    if (sends.empty()) return;

    if (cb && has_mode(stream_mode, StreamMode::DEBUG)) {
        json send_info = json::array();
        for (const auto& s : sends)
            send_info.push_back({{"target", s.target_node}, {"input", s.input}});
        cb(GraphEvent{GraphEvent::Type::NODE_START, "__send__",
                      json{{"sends", send_info}}});
    }

    if (sends.size() == 1) {
        // Single send: sequential on shared state, with retry.
        const auto& s = sends[0];
        auto node_it = nodes_.find(s.target_node);
        if (node_it == nodes_.end()) return;

        const std::string task_id = make_send_task_id(step, 0, s.target_node, s.input);
        NodeResult nr;

        auto replay_it = replay.find(task_id);
        if (replay_it != replay.end()) {
            nr = replay_it->second;
        } else {
            apply_input(state, s.input);
            nr = execute_node_with_retry(s.target_node, state, cb, stream_mode);
            coord.record_pending_write(parent_cp_id,
                task_id, task_id, s.target_node, nr, step);
        }
        state.apply_writes(nr.writes);
        // Send targets can return Command{goto, updates}; goto is meaningless
        // for a fan-out one-shot but `updates` are plain channel writes the
        // node chose to emit via the Command channel rather than `.writes`,
        // and must be merged — matches run_one's behaviour.
        if (nr.command) state.apply_writes(nr.command->updates);
        trace.push_back(s.target_node + "[send]");
        return;
    }

    // Multi-send: Taskflow fan-out with isolated state per target. Each
    // worker routes through execute_node_with_retry, matching the parallel
    // ready-set fan-out path — so NODE_START/END events fire and the
    // retry policy is honoured, just as for single-Send and
    // non-Send parallel execution. Callback thread-safety is the user's
    // responsibility, same contract as the existing parallel ready-set
    // path; nodes still execute on isolated per-worker GraphState copies.
    std::vector<NodeResult> send_results(sends.size());
    std::mutex send_mutex;
    std::exception_ptr send_exception;

    tf::Taskflow taskflow("send-fan-out");
    for (size_t si = 0; si < sends.size(); ++si) {
        taskflow.emplace([&, si]() {
            try {
                const auto& s = sends[si];
                auto node_it = nodes_.find(s.target_node);
                if (node_it == nodes_.end()) return;

                const std::string task_id = make_send_task_id(
                    step, si, s.target_node, s.input);

                auto replay_it = replay.find(task_id);
                if (replay_it != replay.end()) {
                    std::lock_guard lock(send_mutex);
                    send_results[si] = replay_it->second;
                    return;
                }

                // Isolated state: fresh init + restore from shared state
                // + apply this Send's input. Ensures concurrent Sends
                // don't interfere with each other's channel writes.
                GraphState send_state;
                init_state(send_state);
                send_state.restore(state.serialize());
                apply_input(send_state, s.input);

                auto nr = execute_node_with_retry(
                    s.target_node, send_state, cb, stream_mode);

                coord.record_pending_write(parent_cp_id,
                    task_id, task_id, s.target_node, nr, step);

                std::lock_guard lock(send_mutex);
                send_results[si] = std::move(nr);
            } catch (...) {
                std::lock_guard lock(send_mutex);
                if (!send_exception) send_exception = std::current_exception();
            }
        }).name(sends[si].target_node);
    }
    global_executor().run(taskflow).wait();

    if (send_exception) std::rethrow_exception(send_exception);

    // Fan writes from each isolated state back into the shared state.
    // Command.updates are applied on par with .writes (same rationale as
    // the single-Send branch above — goto is meaningless for a fan-out
    // target, but updates are channel writes that must merge).
    for (size_t si = 0; si < sends.size(); ++si) {
        state.apply_writes(send_results[si].writes);
        if (send_results[si].command)
            state.apply_writes(send_results[si].command->updates);
        trace.push_back(sends[si].target_node + "[send]");
    }
}

} // namespace neograph::graph
