#include <neograph/graph/executor.h>
#include <neograph/graph/state.h>
#include <neograph/graph/loader.h>

#include <taskflow/taskflow.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
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
        trace.push_back(s.target_node + "[send]");
        return;
    }

    // Multi-send: Taskflow fan-out with isolated state per target.
    // Multi-sends intentionally skip retry (execute_full, not retry loop)
    // to preserve pre-extraction semantics — revisit if users need retry
    // across large Send fan-outs.
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

                auto nr = node_it->second->execute_full(send_state);

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
    for (size_t si = 0; si < sends.size(); ++si) {
        state.apply_writes(send_results[si].writes);
        trace.push_back(sends[si].target_node + "[send]");
    }
}

} // namespace neograph::graph
