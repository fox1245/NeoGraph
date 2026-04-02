#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>

#include <taskflow/taskflow.hpp>
#include <stdexcept>
#include <algorithm>

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

    // --- Parse interrupt points (Phase 2) ---
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
// Helpers
// =========================================================================

void GraphEngine::own_tools(std::vector<std::unique_ptr<Tool>> tools) {
    owned_tools_ = std::move(tools);
}

void GraphEngine::set_checkpoint_store(std::shared_ptr<CheckpointStore> store) {
    checkpoint_store_ = std::move(store);
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
        // Skip channels not defined in this graph (e.g., parent-only channels
        // passed through by SubgraphNode's default input mapping)
        if (std::find(known.begin(), known.end(), key) != known.end()) {
            state.write(key, value);
        }
    }
}

// =========================================================================
// run / run_stream
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
    if (!checkpoint_store_) {
        throw std::runtime_error("Cannot resume: no checkpoint store configured");
    }

    auto cp_opt = checkpoint_store_->load_latest(thread_id);
    if (!cp_opt) {
        throw std::runtime_error("No checkpoint found for thread: " + thread_id);
    }
    auto& cp = *cp_opt;

    // Build a RunConfig that restores from checkpoint
    RunConfig config;
    config.thread_id = thread_id;
    config.max_steps = 50;

    return execute_graph(config, cb, cp.next_node, resume_value);
}

// =========================================================================
// resolve_next_nodes(): determine successor(s) from a node
//   - Conditional edge: single successor (condition evaluation)
//   - Multiple regular edges from same source: fan-out (all successors)
//   - Single regular edge: single successor
// =========================================================================

std::vector<std::string> GraphEngine::resolve_next_nodes(
    const std::string& current, const GraphState& state) const {

    // 1. Conditional edge takes priority
    for (const auto& ce : conditional_edges_) {
        if (ce.from == current) {
            auto cond_fn = ConditionRegistry::instance().get(ce.condition);
            auto result  = cond_fn(state);
            auto it = ce.routes.find(result);
            if (it != ce.routes.end()) return {it->second};
            return {ce.routes.rbegin()->second};
        }
    }

    // 2. Regular edges: collect ALL successors (fan-out if multiple)
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
    if (it == predecessors_.end()) return true; // no predecessors = ready

    for (const auto& pred : it->second) {
        if (completed.find(pred) == completed.end()) return false;
    }
    return true;
}

// =========================================================================
// Global Taskflow executor (for parallel fan-out)
// =========================================================================
static tf::Executor& global_executor() {
    static tf::Executor exec;
    return exec;
}

// =========================================================================
// execute_graph(): super-step loop with fan-out/fan-in + checkpoint + HITL
//   - Single node ready: sequential execution
//   - Multiple nodes ready: parallel execution via Taskflow
// =========================================================================

RunResult GraphEngine::execute_graph(const RunConfig& config,
                                      const GraphStreamCallback& cb,
                                      const std::string& resume_from,
                                      const json& resume_value) {
    // 1. Initialize state
    GraphState state;
    init_state(state);

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

    // Lambda: execute a single node and return its writes
    auto exec_node = [&](const std::string& node_name) -> std::vector<ChannelWrite> {
        auto node_it = nodes_.find(node_name);
        if (node_it == nodes_.end()) {
            throw std::runtime_error("Node not found: " + node_name);
        }

        if (cb) cb(GraphEvent{GraphEvent::Type::NODE_START, node_name, json()});

        auto writes = cb
            ? node_it->second->execute_stream(state, cb)
            : node_it->second->execute(state);

        if (cb) {
            for (const auto& w : writes) {
                cb(GraphEvent{GraphEvent::Type::CHANNEL_WRITE, node_name,
                              json{{"channel", w.channel}}});
            }
            cb(GraphEvent{GraphEvent::Type::NODE_END, node_name, json()});
        }

        return writes;
    };

    for (int step = start_step; step < config.max_steps + start_step; ++step) {
        if (ready.empty() || hit_end) break;

        // --- interrupt_before check ---
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

                    if (cb) cb(GraphEvent{GraphEvent::Type::INTERRUPT, node_name,
                                json{{"phase", "before"}, {"checkpoint_id", cp.id}}});
                    return result;
                }
            }
        }

        // --- Execute ready nodes ---
        if (ready.size() == 1) {
            // Sequential: direct call
            auto writes = exec_node(ready[0]);
            state.apply_writes(writes);
            trace.push_back(ready[0]);
            completed.insert(ready[0]);
        } else {
            // Parallel: Taskflow fan-out
            std::map<std::string, std::vector<ChannelWrite>> all_writes;
            std::mutex writes_mutex;

            tf::Taskflow taskflow("fan-out");
            for (const auto& node_name : ready) {
                taskflow.emplace([&, node_name]() {
                    auto writes = exec_node(node_name);
                    std::lock_guard lock(writes_mutex);
                    all_writes[node_name] = std::move(writes);
                }).name(node_name);
            }
            global_executor().run(taskflow).wait();

            // Apply writes in deterministic order
            for (const auto& node_name : ready) {
                auto it = all_writes.find(node_name);
                if (it != all_writes.end()) {
                    state.apply_writes(it->second);
                }
                trace.push_back(node_name);
                completed.insert(node_name);
            }
        }

        // --- interrupt_after check ---
        for (const auto& node_name : ready) {
            if (interrupt_after_.count(node_name) &&
                checkpoint_store_ && !config.thread_id.empty()) {

                // Determine next for this node (for checkpoint)
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

                if (cb) cb(GraphEvent{GraphEvent::Type::INTERRUPT, node_name,
                            json{{"phase", "after"}, {"checkpoint_id", cp.id}}});
                return result;
            }
        }

        // --- Resolve next ready set ---
        // Note: do NOT filter by `completed` — cycles require re-entry.
        // max_steps prevents infinite loops.
        std::set<std::string> candidates;
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

        // Filter: only nodes with ALL predecessors completed (fan-in)
        ready.clear();
        for (const auto& candidate : candidates) {
            if (all_predecessors_done(candidate, completed)) {
                ready.push_back(candidate);
            }
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
