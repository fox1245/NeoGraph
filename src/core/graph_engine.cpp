#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/coordinator.h>

#include <neograph/async/run_sync.h>

#include <asio/thread_pool.hpp>

#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

namespace neograph::graph {

namespace {
// Default fan-out pool size. hardware_concurrency() can return 0 on
// platforms that fail to detect; fall back to 4 so we always have
// real parallelism instead of the old single-thread default that
// pinned multi-Send fan-out behind one worker.
std::size_t default_worker_count() {
    auto n = std::thread::hardware_concurrency();
    return n > 0 ? static_cast<std::size_t>(n) : 4u;
}
} // namespace

// =========================================================================
// compile(): JSON definition -> GraphEngine
//
// Parsing is delegated to GraphCompiler (pure JSON → CompiledGraph). This
// function is now just a thin adapter: move every parsed field into the
// engine's runtime home, then construct the Scheduler from the resulting
// edge topology + barrier specs.
// =========================================================================
std::unique_ptr<GraphEngine> GraphEngine::compile(
    const json& definition,
    const NodeContext& default_context,
    std::shared_ptr<CheckpointStore> store) {

    auto cg = GraphCompiler::compile(definition, default_context);

    auto engine = std::unique_ptr<GraphEngine>(new GraphEngine());
    engine->name_              = std::move(cg.name);
    engine->channel_defs_      = std::move(cg.channel_defs);
    engine->nodes_             = std::move(cg.nodes);
    engine->edges_             = std::move(cg.edges);
    engine->conditional_edges_ = std::move(cg.conditional_edges);
    engine->interrupt_before_  = std::move(cg.interrupt_before);
    engine->interrupt_after_   = std::move(cg.interrupt_after);
    if (cg.retry_policy) {
        engine->default_retry_policy_ = *cg.retry_policy;
    }

    // Signal-based dispatch — see Scheduler. A node becomes ready in
    // super-step S+1 iff some node in step S routed to it (regular edge,
    // conditional-edge branch, Command goto, or Send). No static
    // predecessor map: that would conflate XOR routing with AND fan-in
    // and deadlock conditional self-loops. Explicitly-declared barriers
    // (via the node's "barrier": {"wait_for": [...]} field) opt back
    // into AND-join semantics for those specific nodes.
    engine->scheduler_ = std::make_unique<Scheduler>(
        engine->edges_, engine->conditional_edges_, std::move(cg.barrier_specs));

    // NodeExecutor owns retry + fan-out + Send invocation. Bind the
    // retry-policy lookup to this engine's per-node override map so
    // set_node_retry_policy continues to work after compile() returns.
    // The default pool is sized to hardware_concurrency() so a
    // FANOUT > 1 workload parallelizes out of the box; users who need
    // serial semantics (e.g. nodes with non-thread-safe state) can
    // call set_worker_count(1).
    engine->set_worker_count(default_worker_count());

    engine->checkpoint_store_ = std::move(store);
    return engine;
}

// =========================================================================
// Configuration helpers
// =========================================================================

void GraphEngine::own_tools(std::vector<std::unique_ptr<Tool>> tools) {
    owned_tools_ = std::move(tools);
}

void GraphEngine::set_checkpoint_store(std::shared_ptr<CheckpointStore> store) {
    checkpoint_store_ = std::move(store);
}

void GraphEngine::set_store(std::shared_ptr<Store> store) {
    store_ = std::move(store);
}

void GraphEngine::set_retry_policy(const RetryPolicy& policy) {
    default_retry_policy_ = policy;
}

void GraphEngine::set_node_retry_policy(const std::string& node_name, const RetryPolicy& policy) {
    node_retry_policies_[node_name] = policy;
}

void GraphEngine::set_worker_count(std::size_t n) {
    if (n < 1) n = 1;
    // n == 1 means "no engine-owned thread pool" — fan-out branches
    // dispatch on whichever executor drives the coroutine (single-
    // thread io_context for run_sync, the caller's pool for
    // run_async). This matches the pre-b59444f fast path and keeps
    // worker=1 cheap for CPU-tiny / non-thread-safe workloads
    // (par micro-bench: ~12µs vs the cross-thread-submit path's
    // ~94µs at the same worker count). For n >= 2 we wire a real
    // thread_pool — rebuild it because asio::thread_pool isn't
    // resizable. Dtor joins the old pool's workers, so callers must
    // not resize across an in-flight run() (documented on declaration).
    GraphEngine* self = this;
    auto retry_lookup = [self](const std::string& node_name) {
        return self->get_retry_policy(node_name);
    };
    if (n == 1) {
        pool_.reset();
        executor_ = std::make_unique<NodeExecutor>(
            nodes_, channel_defs_, std::move(retry_lookup),
            nullptr, &node_cache_);
        return;
    }
    pool_ = std::make_unique<asio::thread_pool>(n);
    executor_ = std::make_unique<NodeExecutor>(
        nodes_, channel_defs_, std::move(retry_lookup),
        pool_.get(), &node_cache_);
}

void GraphEngine::set_worker_count_auto() {
    set_worker_count(default_worker_count());
}

void GraphEngine::set_node_cache_enabled(const std::string& node_name,
                                          bool enabled) {
    node_cache_.set_enabled(node_name, enabled);
}

void GraphEngine::clear_node_cache() {
    node_cache_.clear();
}

RetryPolicy GraphEngine::get_retry_policy(const std::string& node_name) const {
    auto it = node_retry_policies_.find(node_name);
    if (it != node_retry_policies_.end()) return it->second;
    return default_retry_policy_;
}

// =========================================================================
// get_state / get_state_history / update_state / fork
// =========================================================================

std::optional<json> GraphEngine::get_state(const std::string& thread_id) const {
    if (!checkpoint_store_) return std::nullopt;
    auto cp_opt = checkpoint_store_->load_latest(thread_id);
    if (!cp_opt) return std::nullopt;
    return cp_opt->channel_values;
}

std::vector<Checkpoint> GraphEngine::get_state_history(
    const std::string& thread_id, int limit) const {
    if (!checkpoint_store_) return {};
    return checkpoint_store_->list(thread_id, limit);
}

void GraphEngine::update_state(const std::string& thread_id,
                                const json& channel_writes,
                                const std::string& as_node) {
    if (!checkpoint_store_)
        throw std::runtime_error("Cannot update_state: no checkpoint store configured");

    auto cp_opt = checkpoint_store_->load_latest(thread_id);
    if (!cp_opt)
        throw std::runtime_error("No checkpoint found for thread: " + thread_id);
    auto& cp = *cp_opt;

    GraphState state;
    init_state(state);
    state.restore(cp.channel_values);

    if (channel_writes.is_object()) {
        auto known = state.channel_names();
        for (const auto& [key, value] : channel_writes.items()) {
            if (std::find(known.begin(), known.end(), key) != known.end()) {
                state.write(key, value);
            }
        }
    }

    Checkpoint new_cp;
    new_cp.id              = Checkpoint::generate_id();
    new_cp.thread_id       = thread_id;
    new_cp.channel_values  = state.serialize();
    new_cp.parent_id       = cp.id;
    new_cp.current_node    = as_node.empty() ? cp.current_node : as_node;
    new_cp.next_nodes      = cp.next_nodes;
    new_cp.interrupt_phase = CheckpointPhase::Updated;
    // Barrier accumulators must survive an admin update: if a user
    // update_states during an in-flight AND-join, dropping barrier_state
    // would silently discard partial arrivals. Coordinator-driven
    // super-step saves propagate this for the same reason.
    new_cp.barrier_state   = cp.barrier_state;
    new_cp.step            = cp.step;
    new_cp.timestamp       = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    checkpoint_store_->save(new_cp);
}

std::string GraphEngine::fork(const std::string& source_thread_id,
                               const std::string& new_thread_id,
                               const std::string& checkpoint_id) {
    if (!checkpoint_store_)
        throw std::runtime_error("Cannot fork: no checkpoint store configured");

    std::optional<Checkpoint> cp_opt;
    if (checkpoint_id.empty()) {
        cp_opt = checkpoint_store_->load_latest(source_thread_id);
    } else {
        cp_opt = checkpoint_store_->load_by_id(checkpoint_id);
    }
    if (!cp_opt)
        throw std::runtime_error("No checkpoint found for fork source");

    Checkpoint forked;
    forked.id              = Checkpoint::generate_id();
    forked.thread_id       = new_thread_id;
    forked.channel_values  = cp_opt->channel_values;
    forked.channel_versions = cp_opt->channel_versions;
    forked.parent_id       = cp_opt->id;
    forked.current_node    = cp_opt->current_node;
    forked.next_nodes      = cp_opt->next_nodes;
    forked.interrupt_phase = cp_opt->interrupt_phase;
    // Copy barrier_state so a fork taken mid-AND-join resumes with the
    // same partial-arrival accumulator as its source.
    forked.barrier_state   = cp_opt->barrier_state;
    forked.metadata        = {{"forked_from", {
        {"thread_id", source_thread_id},
        {"checkpoint_id", cp_opt->id}
    }}};
    forked.step            = cp_opt->step;
    forked.timestamp       = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    checkpoint_store_->save(forked);
    return forked.id;
}

// =========================================================================
// State helpers
// =========================================================================

void GraphEngine::init_state(GraphState& state) const {
    for (const auto& cd : channel_defs_) {
        auto reducer = ReducerRegistry::instance().get(cd.reducer_name);
        json initial = cd.initial_value;
        if (cd.type == ReducerType::APPEND && initial.is_null()) {
            initial = json::array();
        }
        state.init_channel(cd.name, cd.type, reducer, initial);
    }
}

void GraphEngine::apply_input(GraphState& state, const json& input) const {
    if (!input.is_object()) return;
    auto known = state.channel_names();
    for (const auto& [key, value] : input.items()) {
        if (std::find(known.begin(), known.end(), key) != known.end()) {
            state.write(key, value);
        }
    }
}

// =========================================================================
// run / run_stream / resume
// =========================================================================

RunResult GraphEngine::run(const RunConfig& config) {
    // Drive the async super-step loop on a single-threaded io_context
    // owned by the caller's stack — the coroutine machinery adds
    // roughly a promise/future per call, so we skip the extra
    // thread-pool hop. Parallel fan-out inside run_parallel_async /
    // run_sends_async still uses pool_ explicitly for CPU
    // parallelism (see NodeExecutor::fan_out_pool_).
    return neograph::async::run_sync(execute_graph_async(config, nullptr));
}

asio::awaitable<RunResult>
GraphEngine::run_async(const RunConfig& config) {
    co_return co_await execute_graph_async(config, nullptr);
}

RunResult GraphEngine::run_stream(const RunConfig& config,
                                   const GraphStreamCallback& cb) {
    return neograph::async::run_sync(execute_graph_async(config, cb));
}

asio::awaitable<RunResult>
GraphEngine::run_stream_async(const RunConfig& config,
                              const GraphStreamCallback& cb) {
    co_return co_await execute_graph_async(config, cb);
}

RunResult GraphEngine::resume(const std::string& thread_id,
                               const json& resume_value,
                               const GraphStreamCallback& cb) {
    return neograph::async::run_sync(resume_async(thread_id, resume_value, cb));
}

asio::awaitable<RunResult>
GraphEngine::resume_async(const std::string& thread_id,
                          const json& resume_value,
                          const GraphStreamCallback& cb) {
    // Sem 3.7.5: real async resume. Mirrors sync resume() but the
    // load_latest and the downstream super-step loop go through
    // their *_async peers, so the whole resume path is non-blocking.
    if (!checkpoint_store_)
        throw std::runtime_error("Cannot resume: no checkpoint store configured");

    auto cp_opt = co_await checkpoint_store_->load_latest_async(thread_id);
    if (!cp_opt)
        throw std::runtime_error("No checkpoint found for thread: " + thread_id);

    if (cp_opt->next_nodes.size() == 1 &&
        cp_opt->next_nodes[0] == std::string(END_NODE)) {
        RunResult result;
        result.output = cp_opt->channel_values;
        result.checkpoint_id = cp_opt->id;
        co_return result;
    }

    RunConfig config;
    config.thread_id = thread_id;
    config.max_steps = 50;

    co_return co_await execute_graph_async(
        config, cb, cp_opt->next_nodes, resume_value);
}

// =========================================================================
// execute_graph_async — super-step loop (coroutine)
// =========================================================================
//   Owns: state init, interrupt_before/after gates, resume load,
//         super-step commit, routing via Scheduler.
//   Delegates: node invocation (single/parallel/Send) → NodeExecutor,
//              checkpoint lifecycle → CheckpointCoordinator,
//              routing decisions → Scheduler.
//
// Sync entry points (run / run_stream / resume) drive this through
// `block_on_pool`, which co_spawns onto the engine's thread_pool
// and blocks via std::future. Async callers on their own executor
// co_await it directly.

asio::awaitable<RunResult>
GraphEngine::execute_graph_async(const RunConfig& config,
                                 const GraphStreamCallback& cb,
                                 const std::vector<std::string>& resume_from,
                                 const json& resume_value) {
    const bool is_resume = !resume_from.empty();

    GraphState state;
    init_state(state);

    StreamMode stream_mode = config.stream_mode;
    CheckpointCoordinator coord(checkpoint_store_, config.thread_id);

    std::string last_checkpoint_id;
    int start_step = 0;

    std::unordered_map<std::string, NodeResult> replay_results;
    BarrierState barrier_state;

    if (is_resume) {
        auto ctx = co_await coord.load_for_resume_async();
        if (ctx.have_cp) {
            state.restore(ctx.channel_values);
            last_checkpoint_id = ctx.checkpoint_id;
            start_step         = ctx.start_step;
            replay_results     = std::move(ctx.replay_results);
            barrier_state      = std::move(ctx.barrier_state);

            if (!resume_value.is_null()) {
                // Build the resume message outside the brace-init that
                // would otherwise nest inside the coroutine body. Same
                // GCC 13 ICE shape; same workaround.
                std::string content = resume_value.is_string()
                    ? resume_value.get<std::string>()
                    : resume_value.dump();
                json resume_msg;
                resume_msg["role"]    = "user";
                resume_msg["content"] = content;
                state.write("messages", json::array({resume_msg}));
            }
        }
    } else {
        apply_input(state, config.input);
    }

    std::vector<std::string> ready =
        is_resume ? resume_from : scheduler_->plan_start_step();

    std::vector<std::string> trace;
    bool hit_end = false;

    std::vector<Send> pending_sends;

    for (int step = start_step; step < config.max_steps + start_step; ++step) {
        if (ready.empty() || hit_end) break;

        // --- interrupt_before check ---
        bool is_resume_entry = (is_resume && step == start_step);
        if (!is_resume_entry) {
            for (const auto& node_name : ready) {
                if (interrupt_before_.count(node_name) && coord.enabled()) {
                    auto cp_id = co_await coord.save_super_step_async(state,
                        node_name, ready,
                        CheckpointPhase::Before, step, last_checkpoint_id,
                        barrier_state);

                    RunResult result;
                    result.output          = state.serialize();
                    result.interrupted     = true;
                    result.interrupt_node  = node_name;
                    json iv;
                    iv["message"] = "Interrupt before node: " + node_name;
                    result.interrupt_value = iv;
                    result.checkpoint_id   = cp_id;
                    result.execution_trace = std::move(trace);

                    if (cb && has_mode(stream_mode, StreamMode::EVENTS)) {
                        json data;
                        data["phase"]         = "before";
                        data["checkpoint_id"] = cp_id;
                        cb(GraphEvent{GraphEvent::Type::INTERRUPT, node_name, data});
                    }
                    co_return result;
                }
            }
        }

        // --- Execute ready nodes ---
        pending_sends.clear();
        std::vector<NodeResult> step_results;

        // Capture NodeInterrupt outside the catch (GCC-13-safe) so the
        // checkpoint lookup that follows can do its own work.
        bool interrupted = false;
        std::string interrupt_reason;

        try {
            if (ready.size() == 1) {
                step_results.push_back(co_await executor_->run_one_async(
                    ready[0], step, state, replay_results,
                    coord, last_checkpoint_id, barrier_state,
                    trace, cb, stream_mode));
            } else {
                // Sem 3.7: full async fan-out via
                // asio::experimental::make_parallel_group. The
                // io_context's worker thread now stays free for other
                // coroutines while the parallel branches run.
                step_results = co_await executor_->run_parallel_async(
                    ready, step, state, replay_results,
                    coord, last_checkpoint_id, barrier_state,
                    trace, cb, stream_mode);
            }
        } catch (const NodeInterrupt& ni) {
            interrupted = true;
            interrupt_reason = ni.reason();
        }

        if (interrupted) {
            RunResult result;
            result.output          = state.serialize();
            result.interrupted     = true;
            result.interrupt_node  = interrupt_reason;
            json iv;
            iv["reason"] = interrupt_reason;
            iv["type"]   = "NodeInterrupt";
            result.interrupt_value = iv;
            result.execution_trace = std::move(trace);

            if (coord.enabled()) {
                auto cp_opt = coord.store()->load_latest(coord.thread_id());
                if (cp_opt) result.checkpoint_id = cp_opt->id;
            }
            co_return result;
        }

        // --- Collect Send requests ---
        for (auto& nr : step_results) {
            for (auto& s : nr.sends) {
                pending_sends.push_back(std::move(s));
            }
        }

        // --- Execute pending Sends BEFORE interrupt_after ---
        // Sem 3.7.6: async fan-out, parallel_group-backed.
        // 4.x: per-task StepRoutings flow back so each spawned task's
        //      Command.goto / default outgoing edge contribute to the
        //      next super-step routing decision (LangGraph parity).
        auto send_routings = co_await executor_->run_sends_async(
            pending_sends, step, state, replay_results,
            coord, last_checkpoint_id, trace, cb, stream_mode);

        if (cb && has_mode(stream_mode, StreamMode::VALUES)) {
            cb(GraphEvent{GraphEvent::Type::CHANNEL_WRITE, "__state__",
                          state.serialize()});
        }

        // --- interrupt_after check ---
        for (const auto& node_name : ready) {
            if (interrupt_after_.count(node_name) && coord.enabled()) {
                std::set<std::string> union_next;
                for (const auto& rn : ready) {
                    for (const auto& nx : scheduler_->resolve_next_nodes(rn, state)) {
                        union_next.insert(nx);
                    }
                }
                // Send-spawned tasks also influence the next super-step;
                // include their goto / default edges in the snapshot so
                // a checkpoint resumed after the interrupt knows the
                // full successor set.
                for (const auto& sr : send_routings) {
                    if (sr.command_goto) {
                        union_next.insert(*sr.command_goto);
                    } else {
                        for (const auto& nx :
                                scheduler_->resolve_next_nodes(sr.node_name, state)) {
                            union_next.insert(nx);
                        }
                    }
                }
                std::vector<std::string> nexts(union_next.begin(), union_next.end());
                if (nexts.empty()) nexts.push_back(std::string(END_NODE));

                auto cp_id = co_await coord.save_super_step_async(state,
                    node_name, nexts, CheckpointPhase::After, step,
                    last_checkpoint_id, barrier_state);

                RunResult result;
                result.output          = state.serialize();
                result.interrupted     = true;
                result.interrupt_node  = node_name;
                json iv;
                iv["message"] = "Interrupt after node: " + node_name;
                result.interrupt_value = iv;
                result.checkpoint_id   = cp_id;
                result.execution_trace = std::move(trace);

                if (cb && has_mode(stream_mode, StreamMode::EVENTS)) {
                    json data;
                    data["phase"]         = "after";
                    data["checkpoint_id"] = cp_id;
                    cb(GraphEvent{GraphEvent::Type::INTERRUPT, node_name, data});
                }
                co_return result;
            }
        }

        // Build the unified routing list: original ready-set first,
        // then Send-spawned tasks in fan-in order. plan_impl unions
        // everyone's edges and applies barrier gating. Per-task
        // command_goto preempts via plan_impl's Pass 1, matching the
        // documented "any command_goto wins" semantic.
        std::vector<StepRouting> unified_routings;
        unified_routings.reserve(step_results.size() + send_routings.size());
        for (size_t i = 0; i < step_results.size(); ++i) {
            StepRouting r;
            r.node_name = ready[i];
            if (step_results[i].command
                && !step_results[i].command->goto_node.empty()) {
                r.command_goto = step_results[i].command->goto_node;
            }
            unified_routings.push_back(std::move(r));
        }
        for (auto& sr : send_routings) {
            unified_routings.push_back(std::move(sr));
        }

        auto plan = scheduler_->plan_next_step(
            unified_routings, state, barrier_state);
        hit_end = hit_end || plan.hit_end;
        ready  = std::move(plan.ready);

        if (cb && has_mode(stream_mode, StreamMode::DEBUG)) {
            if (plan.winning_command_goto) {
                json data;
                data["command_goto"] = *plan.winning_command_goto;
                cb(GraphEvent{GraphEvent::Type::NODE_START, "__routing__", data});
            }
            if (!ready.empty()) {
                json next_nodes_arr = json::array();
                for (const auto& n : ready) next_nodes_arr.push_back(n);
                json data;
                data["next_nodes"] = next_nodes_arr;
                data["step"]       = step;
                cb(GraphEvent{GraphEvent::Type::NODE_START, "__routing__", data});
            }
        }

        if (coord.enabled()) {
            std::vector<std::string> next_nodes_for_cp =
                ready.empty() ? std::vector<std::string>{std::string(END_NODE)}
                              : ready;
            const std::string parent_cp_id = last_checkpoint_id;
            auto cp_id = co_await coord.save_super_step_async(state,
                trace.back(), next_nodes_for_cp, CheckpointPhase::Completed,
                step, parent_cp_id, barrier_state);
            last_checkpoint_id = cp_id;

            co_await coord.clear_pending_writes_async(parent_cp_id);
            replay_results.clear();
        }
    }

    RunResult result;
    result.output          = state.serialize();
    result.execution_trace = std::move(trace);
    if (!last_checkpoint_id.empty()) {
        result.checkpoint_id = last_checkpoint_id;
    }

    auto messages = state.get_messages();
    if (!messages.empty() && messages.back().role == "assistant") {
        result.output["final_response"] = messages.back().content;
    }

    co_return result;
}

} // namespace neograph::graph
