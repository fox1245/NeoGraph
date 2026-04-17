#include <neograph/graph/coordinator.h>
#include <neograph/graph/state.h>
#include <chrono>

namespace neograph::graph {

namespace {

// ── PendingWrite <-> NodeResult serialization ─────────────────────────
// These helpers previously lived as file-local inlines inside
// graph_engine.cpp. They are the wire format between an in-memory
// NodeResult and the on-store PendingWrite record; moved here because
// only the coordinator drives both directions now.

inline json serialize_writes(const std::vector<ChannelWrite>& writes) {
    json arr = json::array();
    for (const auto& w : writes) {
        arr.push_back({{"channel", w.channel}, {"value", w.value}});
    }
    return arr;
}
inline std::vector<ChannelWrite> deserialize_writes(const json& arr) {
    std::vector<ChannelWrite> out;
    if (!arr.is_array()) return out;
    out.reserve(arr.size());
    for (const auto& item : arr) {
        out.push_back(ChannelWrite{
            item.value("channel", std::string{}),
            item.contains("value") ? item["value"] : json()
        });
    }
    return out;
}

inline json serialize_command(const std::optional<Command>& cmd) {
    if (!cmd) return json();
    return {
        {"goto_node", cmd->goto_node},
        {"updates",   serialize_writes(cmd->updates)}
    };
}
inline std::optional<Command> deserialize_command(const json& j) {
    if (j.is_null() || !j.is_object()) return std::nullopt;
    Command c;
    c.goto_node = j.value("goto_node", std::string{});
    c.updates   = deserialize_writes(j.value("updates", json::array()));
    return c;
}

inline json serialize_sends(const std::vector<Send>& sends) {
    json arr = json::array();
    for (const auto& s : sends) {
        arr.push_back({{"target_node", s.target_node}, {"input", s.input}});
    }
    return arr;
}
inline std::vector<Send> deserialize_sends(const json& arr) {
    std::vector<Send> out;
    if (!arr.is_array()) return out;
    out.reserve(arr.size());
    for (const auto& item : arr) {
        Send s;
        s.target_node = item.value("target_node", std::string{});
        s.input       = item.contains("input") ? item["input"] : json();
        out.push_back(std::move(s));
    }
    return out;
}

inline PendingWrite make_pending_write(const std::string& task_id,
                                       const std::string& task_path,
                                       const std::string& node_name,
                                       const NodeResult& nr,
                                       int step) {
    PendingWrite pw;
    pw.task_id   = task_id;
    pw.task_path = task_path;
    pw.node_name = node_name;
    pw.writes    = serialize_writes(nr.writes);
    pw.command   = serialize_command(nr.command);
    pw.sends     = serialize_sends(nr.sends);
    pw.step      = step;
    pw.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return pw;
}

inline NodeResult pending_to_node_result(const PendingWrite& pw) {
    NodeResult nr;
    nr.writes  = deserialize_writes(pw.writes);
    nr.command = deserialize_command(pw.command);
    nr.sends   = deserialize_sends(pw.sends);
    return nr;
}

inline int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

} // namespace

// =========================================================================
// CheckpointCoordinator
// =========================================================================

CheckpointCoordinator::CheckpointCoordinator(
    std::shared_ptr<CheckpointStore> store,
    std::string thread_id)
    : store_(std::move(store)), thread_id_(std::move(thread_id)) {}

std::string CheckpointCoordinator::save_super_step(
    const GraphState& state,
    const std::string& current_node,
    const std::vector<std::string>& next_nodes,
    CheckpointPhase phase,
    int step,
    const std::string& parent_id,
    const BarrierState& barrier_state) const {

    if (!enabled()) return {};

    Checkpoint cp;
    cp.id              = Checkpoint::generate_id();
    cp.thread_id       = thread_id_;
    cp.channel_values  = state.serialize();
    cp.parent_id       = parent_id;
    cp.current_node    = current_node;
    cp.next_nodes      = next_nodes;
    cp.interrupt_phase = phase;
    cp.barrier_state   = barrier_state;
    cp.step            = step;
    cp.timestamp       = now_ms();

    store_->save(cp);
    return cp.id;
}

ResumeContext CheckpointCoordinator::load_for_resume() const {
    ResumeContext ctx;
    if (!enabled()) return ctx;

    auto cp_opt = store_->load_latest(thread_id_);
    if (!cp_opt) return ctx;

    ctx.have_cp        = true;
    ctx.checkpoint_id  = cp_opt->id;
    ctx.channel_values = cp_opt->channel_values;
    ctx.phase          = cp_opt->interrupt_phase;
    ctx.next_nodes     = cp_opt->next_nodes;
    ctx.barrier_state  = cp_opt->barrier_state;

    // Phase-aware step offset:
    //   Before / NodeInterrupt → cp was saved *before* the node in this
    //     step ran, so resume re-enters AT cp.step.
    //   After / Completed      → cp was saved *after* the step's work
    //     finished, so resume starts at the NEXT step.
    //   Updated                → treated like Completed for step
    //     advancement (update_state substitutes for a committed step).
    ctx.start_step = static_cast<int>(cp_opt->step);
    if (cp_opt->interrupt_phase == CheckpointPhase::After ||
        cp_opt->interrupt_phase == CheckpointPhase::Completed ||
        cp_opt->interrupt_phase == CheckpointPhase::Updated) {
        ctx.start_step += 1;
    }

    // Rehydrate in-flight super-step writes so the engine can replay
    // completed tasks instead of re-executing them.
    auto pending = store_->get_writes(thread_id_, ctx.checkpoint_id);
    for (const auto& pw : pending) {
        ctx.replay_results.emplace(pw.task_id, pending_to_node_result(pw));
    }

    return ctx;
}

void CheckpointCoordinator::record_pending_write(
    const std::string& parent_cp_id,
    const std::string& task_id,
    const std::string& task_path,
    const std::string& node_name,
    const NodeResult& nr,
    int step) const {

    if (!enabled()) return;
    store_->put_writes(thread_id_, parent_cp_id,
                       make_pending_write(task_id, task_path, node_name, nr, step));
}

void CheckpointCoordinator::clear_pending_writes(
    const std::string& parent_cp_id) const {

    if (!enabled()) return;
    if (parent_cp_id.empty()) return;
    store_->clear_writes(thread_id_, parent_cp_id);
}

} // namespace neograph::graph
