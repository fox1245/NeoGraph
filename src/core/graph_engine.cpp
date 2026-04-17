#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/coordinator.h>

#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <cstdint>

namespace neograph::graph {

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
    GraphEngine* raw_engine = engine.get();
    engine->executor_ = std::make_unique<NodeExecutor>(
        engine->nodes_,
        engine->channel_defs_,
        [raw_engine](const std::string& node_name) {
            return raw_engine->get_retry_policy(node_name);
        });

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
    return execute_graph(config, nullptr);
}

RunResult GraphEngine::run_stream(const RunConfig& config,
                                   const GraphStreamCallback& cb) {
    return execute_graph(config, cb);
}

RunResult GraphEngine::resume(const std::string& thread_id,
                               const json& resume_value,
                               const GraphStreamCallback& cb) {
    if (!checkpoint_store_)
        throw std::runtime_error("Cannot resume: no checkpoint store configured");

    auto cp_opt = checkpoint_store_->load_latest(thread_id);
    if (!cp_opt)
        throw std::runtime_error("No checkpoint found for thread: " + thread_id);

    // "Completed to END" means the original run finished cleanly — nothing
    // left to resume. Under multi-node checkpoints this is encoded as
    // next_nodes == {END} (a single-element vector), so check for exactly
    // that shape.
    if (cp_opt->next_nodes.size() == 1 &&
        cp_opt->next_nodes[0] == std::string(END_NODE)) {
        RunResult result;
        result.output = cp_opt->channel_values;
        result.checkpoint_id = cp_opt->id;
        return result;
    }

    RunConfig config;
    config.thread_id = thread_id;
    config.max_steps = 50;

    return execute_graph(config, cb, cp_opt->next_nodes, resume_value);
}

// =========================================================================
// execute_graph(): super-step loop
//   Owns: state init, interrupt_before/after gates, resume load,
//         super-step commit, routing via Scheduler.
//   Delegates: node invocation (single/parallel/Send) → NodeExecutor,
//              checkpoint lifecycle → CheckpointCoordinator,
//              routing decisions → Scheduler.
// =========================================================================

RunResult GraphEngine::execute_graph(const RunConfig& config,
                                      const GraphStreamCallback& cb,
                                      const std::vector<std::string>& resume_from,
                                      const json& resume_value) {
    const bool is_resume = !resume_from.empty();
    // 1. Initialize state
    GraphState state;
    init_state(state);

    StreamMode stream_mode = config.stream_mode;
    CheckpointCoordinator coord(checkpoint_store_, config.thread_id);

    std::string last_checkpoint_id;
    int start_step = 0;

    // Replay map for crash / partial-failure recovery: task_id -> NodeResult
    // that was already recorded as a pending write under last_checkpoint_id.
    // Tasks whose id hits this map are skipped during execution and their
    // results are applied exactly as originally recorded.
    std::unordered_map<std::string, NodeResult> replay_results;

    // Per-run barrier accumulation. Scheduler mutates this across
    // super-steps so partial upstream-signal sets add up over time.
    // Since schema v2 the map is persisted into every checkpoint and
    // restored below on resume, so an interrupt mid-accumulation no
    // longer loses upstream signals.
    BarrierState barrier_state;

    if (is_resume) {
        auto ctx = coord.load_for_resume();
        if (ctx.have_cp) {
            state.restore(ctx.channel_values);
            last_checkpoint_id = ctx.checkpoint_id;
            start_step         = ctx.start_step;
            replay_results     = std::move(ctx.replay_results);
            barrier_state      = std::move(ctx.barrier_state);

            if (!resume_value.is_null()) {
                json resume_msg = {
                    {"role", "user"},
                    {"content", resume_value.is_string()
                        ? resume_value.get<std::string>()
                        : resume_value.dump()}
                };
                state.write("messages", json::array({resume_msg}));
            }
        }
    } else {
        apply_input(state, config.input);
    }

    // 2. Determine initial ready set
    std::vector<std::string> ready =
        is_resume ? resume_from : scheduler_->plan_start_step();

    // 3. Super-step loop
    std::vector<std::string> trace;
    bool hit_end = false;

    // Pending Send requests (dynamic fan-out)
    std::vector<Send> pending_sends;

    for (int step = start_step; step < config.max_steps + start_step; ++step) {
        if (ready.empty() || hit_end) break;

        // --- interrupt_before check (compile-time) ---
        bool is_resume_entry = (is_resume && step == start_step);
        if (!is_resume_entry) {
            for (const auto& node_name : ready) {
                if (interrupt_before_.count(node_name) && coord.enabled()) {

                    // The interrupt happens before this node runs. Resume
                    // must re-enter the WHOLE super-step (this node PLUS
                    // every sibling that was also ready) — saving just
                    // this one would silently drop the siblings.
                    auto cp_id = coord.save_super_step(state,
                        node_name, ready,
                        CheckpointPhase::Before, step, last_checkpoint_id,
                        barrier_state);

                    RunResult result;
                    result.output          = state.serialize();
                    result.interrupted     = true;
                    result.interrupt_node  = node_name;
                    result.interrupt_value = json{{"message", "Interrupt before node: " + node_name}};
                    result.checkpoint_id   = cp_id;
                    result.execution_trace = std::move(trace);

                    if (cb && has_mode(stream_mode, StreamMode::EVENTS))
                        cb(GraphEvent{GraphEvent::Type::INTERRUPT, node_name,
                            json{{"phase", "before"}, {"checkpoint_id", cp_id}}});
                    return result;
                }
            }
        }

        // --- Execute ready nodes ---
        pending_sends.clear();
        std::vector<NodeResult> step_results;

        try {
            if (ready.size() == 1) {
                step_results.push_back(executor_->run_one(
                    ready[0], step, state, replay_results,
                    coord, last_checkpoint_id, barrier_state,
                    trace, cb, stream_mode));
            } else {
                step_results = executor_->run_parallel(
                    ready, step, state, replay_results,
                    coord, last_checkpoint_id, barrier_state,
                    trace, cb, stream_mode);
            }
        } catch (const NodeInterrupt& ni) {
            RunResult result;
            result.output          = state.serialize();
            result.interrupted     = true;
            result.interrupt_node  = ni.reason();
            result.interrupt_value = json{{"reason", ni.reason()}, {"type", "NodeInterrupt"}};
            result.execution_trace = std::move(trace);
            // The NodeInterrupt cp was already written by execute_single
            // above; find it by loading the most recent entry for this
            // thread so the RunResult surfaces its id to the caller.
            if (coord.enabled()) {
                auto cp_opt = coord.store()->load_latest(coord.thread_id());
                if (cp_opt) result.checkpoint_id = cp_opt->id;
            }
            return result;
        }

        // --- Collect Send requests (pending_sends are drained below).
        // Command.goto_node is consumed later by the Scheduler; we
        // only need to surface Sends here. ---
        for (auto& nr : step_results) {
            for (auto& s : nr.sends) {
                pending_sends.push_back(std::move(s));
            }
        }

        // --- Execute pending Sends BEFORE interrupt_after ---
        // Rationale: interrupt_after semantically means "pause after
        // this node's super-step has fully completed", which includes
        // any dynamic fan-out it emitted. If we gate before Sends and
        // a node with interrupt_after emits them, the interrupt cp
        // captures pre-Sends state, resume's start_step advances past
        // this super-step, and the Sends vanish permanently.
        executor_->run_sends(pending_sends, step, state, replay_results,
                             coord, last_checkpoint_id,
                             trace, cb, stream_mode);

        // --- Stream VALUES mode: emit full state after each step
        // (post-Sends so the emitted snapshot matches what will land
        // in the super-step's committed checkpoint). ---
        if (cb && has_mode(stream_mode, StreamMode::VALUES)) {
            cb(GraphEvent{GraphEvent::Type::CHANNEL_WRITE, "__state__",
                          state.serialize()});
        }

        // --- interrupt_after check ---
        for (const auto& node_name : ready) {
            if (interrupt_after_.count(node_name) && coord.enabled()) {

                // Every node in `ready` has already executed by this
                // point, so resume must re-enter with the union of all
                // their successors — not just the interrupted node's.
                std::set<std::string> union_next;
                for (const auto& rn : ready) {
                    for (const auto& nx : scheduler_->resolve_next_nodes(rn, state)) {
                        union_next.insert(nx);
                    }
                }
                std::vector<std::string> nexts(union_next.begin(), union_next.end());
                if (nexts.empty()) nexts.push_back(std::string(END_NODE));

                auto cp_id = coord.save_super_step(state,
                    node_name, nexts, CheckpointPhase::After, step, last_checkpoint_id,
                    barrier_state);

                RunResult result;
                result.output          = state.serialize();
                result.interrupted     = true;
                result.interrupt_node  = node_name;
                result.interrupt_value = json{{"message", "Interrupt after node: " + node_name}};
                result.checkpoint_id   = cp_id;
                result.execution_trace = std::move(trace);

                if (cb && has_mode(stream_mode, StreamMode::EVENTS))
                    cb(GraphEvent{GraphEvent::Type::INTERRUPT, node_name,
                        json{{"phase", "after"}, {"checkpoint_id", cp_id}}});
                return result;
            }
        }

        // --- Plan next super-step via Scheduler ---
        // Scheduler internally pairs ready[i] ↔ step_results[i],
        // extracts Command.goto_node, and applies barrier gating
        // against `barrier_state` (shared across super-steps).
        auto plan = scheduler_->plan_next_step(
            ready, step_results, state, barrier_state);
        hit_end = hit_end || plan.hit_end;
        ready  = std::move(plan.ready);

        // --- Debug: emit routing decision ---
        if (cb && has_mode(stream_mode, StreamMode::DEBUG)) {
            if (plan.winning_command_goto) {
                cb(GraphEvent{GraphEvent::Type::NODE_START, "__routing__",
                              json{{"command_goto", *plan.winning_command_goto}}});
            }
            if (!ready.empty()) {
                json next_nodes = json::array();
                for (const auto& n : ready) next_nodes.push_back(n);
                cb(GraphEvent{GraphEvent::Type::NODE_START, "__routing__",
                              json{{"next_nodes", next_nodes}, {"step", step}}});
            }
        }

        // --- Checkpoint after each super-step ---
        if (coord.enabled()) {
            // Persist the ENTIRE ready set — under signal dispatch multiple
            // nodes can be simultaneously scheduled, and resume must pick
            // up all of them (not just the first) or sibling branches are
            // silently dropped across a crash.
            std::vector<std::string> next_nodes =
                ready.empty() ? std::vector<std::string>{std::string(END_NODE)}
                              : ready;
            const std::string parent_cp_id = last_checkpoint_id;
            auto cp_id = coord.save_super_step(state,
                trace.back(), next_nodes, CheckpointPhase::Completed, step, parent_cp_id,
                barrier_state);
            last_checkpoint_id = cp_id;

            // Pending writes for the just-committed super-step are now
            // superseded by the fresh checkpoint — safe to discard.
            // Ordering matters: clear ONLY after save_super_step returned,
            // so a crash between save and clear is harmless (stale writes
            // will simply be ignored once a newer cp exists).
            coord.clear_pending_writes(parent_cp_id);

            // Replay map is scoped to the parent cp we just superseded;
            // subsequent steps start with a clean slate.
            replay_results.clear();
        }
    }

    // 4. Build result
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

    return result;
}

} // namespace neograph::graph
