#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>

#include <taskflow/taskflow.hpp>
#include <stdexcept>
#include <algorithm>
#include <thread>

namespace neograph::graph {

// =========================================================================
// compile(): JSON definition -> GraphEngine
// =========================================================================
std::unique_ptr<GraphEngine> GraphEngine::compile(
    const json& definition,
    const NodeContext& default_context,
    std::shared_ptr<CheckpointStore> store) {

    auto engine = std::unique_ptr<GraphEngine>(new GraphEngine());
    engine->name_ = definition.value("name", "unnamed_graph");

    // --- Parse channels ---
    if (definition.contains("channels")) {
        for (const auto& [name, ch_def] : definition["channels"].items()) {
            ChannelDef cd;
            cd.name          = name;
            cd.reducer_name  = ch_def.value("reducer", "overwrite");
            cd.initial_value = ch_def.contains("initial") ? ch_def["initial"] : json();

            if (cd.reducer_name == "append")
                cd.type = ReducerType::APPEND;
            else if (cd.reducer_name == "overwrite")
                cd.type = ReducerType::OVERWRITE;
            else
                cd.type = ReducerType::CUSTOM;

            engine->channel_defs_.push_back(std::move(cd));
        }
    }

    // --- Parse nodes ---
    if (definition.contains("nodes")) {
        for (const auto& [name, node_def] : definition["nodes"].items()) {
            auto type = node_def.value("type", "");
            auto node = NodeFactory::instance().create(type, name, node_def, default_context);
            engine->nodes_[name] = std::move(node);
        }
    }

    // --- Parse edges ---
    if (definition.contains("edges")) {
        for (const auto& edge_def : definition["edges"]) {
            bool is_conditional = edge_def.contains("condition")
                               || edge_def.value("type", "") == "conditional";

            if (is_conditional) {
                ConditionalEdge ce;
                ce.from      = edge_def.at("from").get<std::string>();
                ce.condition = edge_def.at("condition").get<std::string>();
                if (edge_def.contains("routes")) {
                    for (const auto& [key, target] : edge_def["routes"].items()) {
                        ce.routes[key] = target.get<std::string>();
                    }
                }
                engine->conditional_edges_.push_back(std::move(ce));
            } else {
                Edge e;
                e.from = edge_def.at("from").get<std::string>();
                e.to   = edge_def.at("to").get<std::string>();
                engine->edges_.push_back(std::move(e));
            }
        }
    }

    // --- Parse interrupt points ---
    if (definition.contains("interrupt_before")) {
        for (const auto& n : definition["interrupt_before"]) {
            engine->interrupt_before_.insert(n.get<std::string>());
        }
    }
    if (definition.contains("interrupt_after")) {
        for (const auto& n : definition["interrupt_after"]) {
            engine->interrupt_after_.insert(n.get<std::string>());
        }
    }

    // --- Parse retry policy ---
    if (definition.contains("retry_policy")) {
        auto& rp = definition["retry_policy"];
        engine->default_retry_policy_.max_retries = rp.value("max_retries", 0);
        engine->default_retry_policy_.initial_delay_ms = rp.value("initial_delay_ms", 100);
        engine->default_retry_policy_.backoff_multiplier = rp.value("backoff_multiplier", 2.0f);
        engine->default_retry_policy_.max_delay_ms = rp.value("max_delay_ms", 5000);
    }

    // --- Build predecessor map (for fan-in detection) ---
    for (const auto& e : engine->edges_) {
        if (e.from != std::string(START_NODE) && e.to != std::string(END_NODE)) {
            engine->predecessors_[e.to].insert(e.from);
        }
    }
    for (const auto& ce : engine->conditional_edges_) {
        for (const auto& [key, target] : ce.routes) {
            if (target != std::string(END_NODE)) {
                engine->predecessors_[target].insert(ce.from);
            }
        }
    }

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

Checkpoint GraphEngine::save_checkpoint(
    const GraphState& state,
    const std::string& thread_id,
    const std::string& current_node,
    const std::string& next_node,
    const std::string& phase,
    int step,
    const std::string& parent_id) const {

    Checkpoint cp;
    cp.id              = Checkpoint::generate_id();
    cp.thread_id       = thread_id;
    cp.channel_values  = state.serialize();
    cp.parent_id       = parent_id;
    cp.current_node    = current_node;
    cp.next_node       = next_node;
    cp.interrupt_phase = phase;
    cp.step            = step;
    cp.timestamp       = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    checkpoint_store_->save(cp);
    return cp;
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
    new_cp.next_node       = cp.next_node;
    new_cp.interrupt_phase = "updated";
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
    forked.next_node       = cp_opt->next_node;
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

    if (cp_opt->next_node == std::string(END_NODE)) {
        RunResult result;
        result.output = cp_opt->channel_values;
        result.checkpoint_id = cp_opt->id;
        return result;
    }

    RunConfig config;
    config.thread_id = thread_id;
    config.max_steps = 50;

    return execute_graph(config, cb, cp_opt->next_node, resume_value);
}

// =========================================================================
// resolve_next_nodes
// =========================================================================

std::vector<std::string> GraphEngine::resolve_next_nodes(
    const std::string& current, const GraphState& state) const {

    for (const auto& ce : conditional_edges_) {
        if (ce.from == current) {
            auto cond_fn = ConditionRegistry::instance().get(ce.condition);
            auto result  = cond_fn(state);
            auto it = ce.routes.find(result);
            if (it != ce.routes.end()) return {it->second};
            return {ce.routes.rbegin()->second};
        }
    }

    std::vector<std::string> successors;
    for (const auto& e : edges_) {
        if (e.from == current) {
            successors.push_back(e.to);
        }
    }

    if (successors.empty()) return {END_NODE};
    return successors;
}

bool GraphEngine::all_predecessors_done(
    const std::string& node,
    const std::set<std::string>& completed) const {

    auto it = predecessors_.find(node);
    if (it == predecessors_.end()) return true;

    for (const auto& pred : it->second) {
        if (completed.find(pred) == completed.end()) return false;
    }
    return true;
}

// =========================================================================
// Global Taskflow executor
// =========================================================================
static tf::Executor& global_executor() {
    static tf::Executor exec;
    return exec;
}

// =========================================================================
// execute_node_with_retry: run a node with retry policy + NodeInterrupt
// =========================================================================

NodeResult GraphEngine::execute_node_with_retry(
    const std::string& node_name,
    GraphState& state,
    const GraphStreamCallback& cb,
    StreamMode stream_mode) {

    auto node_it = nodes_.find(node_name);
    if (node_it == nodes_.end()) {
        throw std::runtime_error("Node not found: " + node_name);
    }

    auto policy = get_retry_policy(node_name);
    int delay_ms = policy.initial_delay_ms;

    for (int attempt = 0; attempt <= policy.max_retries; ++attempt) {
        try {
            if (cb && has_mode(stream_mode, StreamMode::EVENTS)) {
                json data;
                if (attempt > 0) data["retry_attempt"] = attempt;
                cb(GraphEvent{GraphEvent::Type::NODE_START, node_name, data});
            }

            // Use execute_full to get NodeResult (Command/Send support)
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
// execute_graph(): super-step loop
//   Supports: fan-out/fan-in, checkpoint, HITL, NodeInterrupt,
//             Command, Send, retry, stream modes
// =========================================================================

RunResult GraphEngine::execute_graph(const RunConfig& config,
                                      const GraphStreamCallback& cb,
                                      const std::string& resume_from,
                                      const json& resume_value) {
    // 1. Initialize state
    GraphState state;
    init_state(state);

    StreamMode stream_mode = config.stream_mode;
    std::string last_checkpoint_id;
    int start_step = 0;

    if (!resume_from.empty() && checkpoint_store_ && !config.thread_id.empty()) {
        auto cp_opt = checkpoint_store_->load_latest(config.thread_id);
        if (cp_opt) {
            state.restore(cp_opt->channel_values);
            last_checkpoint_id = cp_opt->id;
            start_step = static_cast<int>(cp_opt->step);

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
    std::vector<std::string> ready;
    if (!resume_from.empty()) {
        ready = {resume_from};
    } else {
        for (const auto& e : edges_) {
            if (e.from == std::string(START_NODE) && e.to != std::string(END_NODE)) {
                ready.push_back(e.to);
            }
        }
    }

    // 3. Super-step loop
    std::vector<std::string> trace;
    std::set<std::string> completed;
    bool hit_end = false;

    // Pending Send requests (dynamic fan-out)
    std::vector<Send> pending_sends;

    // Command override for routing
    std::optional<std::string> command_goto;

    for (int step = start_step; step < config.max_steps + start_step; ++step) {
        if (ready.empty() || hit_end) break;

        // --- interrupt_before check (compile-time) ---
        bool is_resume_entry = (!resume_from.empty() && step == start_step);
        if (!is_resume_entry) {
            for (const auto& node_name : ready) {
                if (interrupt_before_.count(node_name) &&
                    checkpoint_store_ && !config.thread_id.empty()) {

                    auto cp = save_checkpoint(state, config.thread_id,
                        node_name, node_name, "before", step, last_checkpoint_id);

                    RunResult result;
                    result.output          = state.serialize();
                    result.interrupted     = true;
                    result.interrupt_node  = node_name;
                    result.interrupt_value = json{{"message", "Interrupt before node: " + node_name}};
                    result.checkpoint_id   = cp.id;
                    result.execution_trace = std::move(trace);

                    if (cb && has_mode(stream_mode, StreamMode::EVENTS))
                        cb(GraphEvent{GraphEvent::Type::INTERRUPT, node_name,
                            json{{"phase", "before"}, {"checkpoint_id", cp.id}}});
                    return result;
                }
            }
        }

        // --- Execute ready nodes ---
        command_goto.reset();
        pending_sends.clear();

        // Collect NodeResults from all executed nodes
        std::vector<NodeResult> step_results;

        auto execute_single = [&](const std::string& node_name) -> NodeResult {
            try {
                auto nr = execute_node_with_retry(node_name, state, cb, stream_mode);

                // Apply writes from this node
                state.apply_writes(nr.writes);

                // Apply Command updates if present
                if (nr.command) {
                    state.apply_writes(nr.command->updates);
                }

                trace.push_back(node_name);
                completed.insert(node_name);
                return nr;

            } catch (const NodeInterrupt& ni) {
                if (checkpoint_store_ && !config.thread_id.empty()) {
                    save_checkpoint(state, config.thread_id,
                        node_name, node_name, "node_interrupt", step, last_checkpoint_id);
                }
                throw;
            }
        };

        try {
            if (ready.size() == 1) {
                step_results.push_back(execute_single(ready[0]));
            } else {
                // Parallel: Taskflow fan-out
                std::map<std::string, NodeResult> all_results;
                std::mutex results_mutex;
                std::exception_ptr first_exception;

                tf::Taskflow taskflow("fan-out");
                for (const auto& node_name : ready) {
                    taskflow.emplace([&, node_name]() {
                        try {
                            auto nr = execute_node_with_retry(
                                node_name, state, cb, stream_mode);
                            std::lock_guard lock(results_mutex);
                            all_results[node_name] = std::move(nr);
                        } catch (...) {
                            std::lock_guard lock(results_mutex);
                            if (!first_exception) first_exception = std::current_exception();
                        }
                    }).name(node_name);
                }
                global_executor().run(taskflow).wait();

                if (first_exception) std::rethrow_exception(first_exception);

                for (const auto& node_name : ready) {
                    auto it = all_results.find(node_name);
                    if (it != all_results.end()) {
                        state.apply_writes(it->second.writes);
                        if (it->second.command)
                            state.apply_writes(it->second.command->updates);
                        step_results.push_back(std::move(it->second));
                    }
                    trace.push_back(node_name);
                    completed.insert(node_name);
                }
            }
        } catch (const NodeInterrupt& ni) {
            RunResult result;
            result.output          = state.serialize();
            result.interrupted     = true;
            result.interrupt_node  = ni.reason();
            result.interrupt_value = json{{"reason", ni.reason()}, {"type", "NodeInterrupt"}};
            result.execution_trace = std::move(trace);
            if (checkpoint_store_ && !config.thread_id.empty()) {
                auto cp_opt = checkpoint_store_->load_latest(config.thread_id);
                if (cp_opt) result.checkpoint_id = cp_opt->id;
            }
            return result;
        }

        // --- Collect Command goto and Send requests from step results ---
        for (auto& nr : step_results) {
            if (nr.command && !nr.command->goto_node.empty()) {
                command_goto = nr.command->goto_node;
            }
            for (auto& s : nr.sends) {
                pending_sends.push_back(std::move(s));
            }
        }

        // --- Stream VALUES mode: emit full state after each step ---
        if (cb && has_mode(stream_mode, StreamMode::VALUES)) {
            cb(GraphEvent{GraphEvent::Type::CHANNEL_WRITE, "__state__",
                          state.serialize()});
        }

        // --- interrupt_after check ---
        for (const auto& node_name : ready) {
            if (interrupt_after_.count(node_name) &&
                checkpoint_store_ && !config.thread_id.empty()) {

                auto nexts = resolve_next_nodes(node_name, state);
                std::string next_str = nexts.empty() ? std::string(END_NODE) : nexts[0];

                auto cp = save_checkpoint(state, config.thread_id,
                    node_name, next_str, "after", step, last_checkpoint_id);

                RunResult result;
                result.output          = state.serialize();
                result.interrupted     = true;
                result.interrupt_node  = node_name;
                result.interrupt_value = json{{"message", "Interrupt after node: " + node_name}};
                result.checkpoint_id   = cp.id;
                result.execution_trace = std::move(trace);

                if (cb && has_mode(stream_mode, StreamMode::EVENTS))
                    cb(GraphEvent{GraphEvent::Type::INTERRUPT, node_name,
                        json{{"phase", "after"}, {"checkpoint_id", cp.id}}});
                return result;
            }
        }

        // --- Execute pending Sends (dynamic fan-out) ---
        if (!pending_sends.empty()) {
            if (cb && has_mode(stream_mode, StreamMode::DEBUG)) {
                json send_info = json::array();
                for (const auto& s : pending_sends)
                    send_info.push_back({{"target", s.target_node}, {"input", s.input}});
                cb(GraphEvent{GraphEvent::Type::NODE_START, "__send__",
                              json{{"sends", send_info}}});
            }

            if (pending_sends.size() == 1) {
                // Single send: sequential
                auto& s = pending_sends[0];
                auto node_it = nodes_.find(s.target_node);
                if (node_it != nodes_.end()) {
                    // Apply send input to a temporary state overlay
                    apply_input(state, s.input);
                    auto nr = execute_node_with_retry(s.target_node, state, cb, stream_mode);
                    state.apply_writes(nr.writes);
                    trace.push_back(s.target_node + "[send]");
                }
            } else {
                // Multiple sends: parallel via Taskflow
                std::vector<NodeResult> send_results(pending_sends.size());
                std::mutex send_mutex;
                std::exception_ptr send_exception;

                tf::Taskflow taskflow("send-fan-out");
                for (size_t si = 0; si < pending_sends.size(); ++si) {
                    taskflow.emplace([&, si]() {
                        try {
                            auto& s = pending_sends[si];
                            auto node_it = nodes_.find(s.target_node);
                            if (node_it == nodes_.end()) return;

                            // Each send gets its own state copy with send input applied
                            GraphState send_state;
                            init_state(send_state);
                            send_state.restore(state.serialize());
                            apply_input(send_state, s.input);

                            auto nr = node_it->second->execute_full(send_state);
                            std::lock_guard lock(send_mutex);
                            send_results[si] = std::move(nr);
                        } catch (...) {
                            std::lock_guard lock(send_mutex);
                            if (!send_exception) send_exception = std::current_exception();
                        }
                    }).name(pending_sends[si].target_node);
                }
                global_executor().run(taskflow).wait();

                if (send_exception) std::rethrow_exception(send_exception);

                // Apply all send results to main state
                for (size_t si = 0; si < pending_sends.size(); ++si) {
                    state.apply_writes(send_results[si].writes);
                    trace.push_back(pending_sends[si].target_node + "[send]");
                }
            }
        }

        // --- Resolve next ready set ---
        std::set<std::string> candidates;

        if (command_goto) {
            // Command override: go directly to the specified node
            if (*command_goto == std::string(END_NODE)) {
                hit_end = true;
            } else {
                candidates.insert(*command_goto);
            }

            if (cb && has_mode(stream_mode, StreamMode::DEBUG)) {
                cb(GraphEvent{GraphEvent::Type::NODE_START, "__routing__",
                              json{{"command_goto", *command_goto}}});
            }
        } else {
            // Standard edge routing
            for (const auto& node_name : ready) {
                auto nexts = resolve_next_nodes(node_name, state);
                for (const auto& next : nexts) {
                    if (next == std::string(END_NODE)) {
                        hit_end = true;
                    } else {
                        candidates.insert(next);
                    }
                }
            }
        }

        ready.clear();
        for (const auto& candidate : candidates) {
            if (command_goto || all_predecessors_done(candidate, completed)) {
                ready.push_back(candidate);
            }
        }

        // --- Debug: emit routing decision ---
        if (cb && has_mode(stream_mode, StreamMode::DEBUG) && !ready.empty()) {
            json next_nodes = json::array();
            for (const auto& n : ready) next_nodes.push_back(n);
            cb(GraphEvent{GraphEvent::Type::NODE_START, "__routing__",
                          json{{"next_nodes", next_nodes}, {"step", step}}});
        }

        // --- Checkpoint after each super-step ---
        if (checkpoint_store_ && !config.thread_id.empty()) {
            std::string next_str = ready.empty() ? std::string(END_NODE) : ready[0];
            auto cp = save_checkpoint(state, config.thread_id,
                trace.back(), next_str, "completed", step, last_checkpoint_id);
            last_checkpoint_id = cp.id;
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
