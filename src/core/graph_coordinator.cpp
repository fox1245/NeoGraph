#include <neograph/graph/coordinator.h>
#include <neograph/graph/state.h>
#include <neograph/hook_runtime.h>

#include "channel_write_codec.h"

#include <chrono>
#include <limits>
#include <stdexcept>

namespace neograph::graph {

namespace {

// ── PendingWrite <-> NodeResult serialization ─────────────────────────
// These helpers previously lived as file-local inlines inside
// graph_engine.cpp. They are the wire format between an in-memory
// NodeResult and the on-store PendingWrite record; moved here because
// only the coordinator drives both directions now.

inline json serialize_command(const std::optional<Command>& cmd) {
    if (!cmd) return json();
    return {{"goto_node", cmd->goto_node},
            {"updates", detail::serialize_channel_writes(cmd->updates)}};
}
inline std::optional<Command> deserialize_command(const json& j) {
    if (j.is_null() || !j.is_object()) return std::nullopt;
    Command c;
    c.goto_node = j.value("goto_node", std::string{});
    c.updates   = detail::deserialize_channel_writes(j.value("updates", json::array()));
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
                                       const NodeResult&  nr,
                                       int                step) {
    PendingWrite pw;
    pw.task_id   = task_id;
    pw.task_path = task_path;
    pw.node_name = node_name;
    pw.writes    = detail::serialize_channel_writes(nr.writes);
    pw.command   = serialize_command(nr.command);
    pw.sends     = serialize_sends(nr.sends);
    pw.step      = step;
    pw.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
    return pw;
}

inline NodeResult pending_to_node_result(const PendingWrite& pw) {
    NodeResult nr;
    nr.writes  = detail::deserialize_channel_writes(pw.writes);
    nr.command = deserialize_command(pw.command);
    nr.sends   = deserialize_sends(pw.sends);
    return nr;
}

inline int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

int resume_start_step(const Checkpoint& checkpoint) {
    if (checkpoint.step < 0) {
        throw std::runtime_error("Checkpoint step must not be negative");
    }
    const bool advances =
        checkpoint.interrupt_phase == CheckpointPhase::After ||
        checkpoint.interrupt_phase == CheckpointPhase::Completed ||
        checkpoint.interrupt_phase == CheckpointPhase::Updated;
    const auto maximum = static_cast<std::int64_t>(
        std::numeric_limits<int>::max());
    if (checkpoint.step >= maximum) {
        throw std::runtime_error("Checkpoint step exceeds the executable range");
    }
    const auto start = checkpoint.step + (advances ? 1 : 0);
    return static_cast<int>(start);
}

}  // namespace

// =========================================================================
// CheckpointCoordinator
// =========================================================================

CheckpointCoordinator::CheckpointCoordinator(std::shared_ptr<CheckpointStore> store,
                                              std::string                      thread_id,
                                              std::shared_ptr<::neograph::HookRuntime> hook_runtime,
                                              CheckpointHookContext hook_context)
    : store_(std::move(store)), thread_id_(std::move(thread_id)),
      hook_runtime_(std::move(hook_runtime)), hook_context_(std::move(hook_context)) {}

asio::awaitable<void> CheckpointCoordinator::publish_checkpoint_async(
    const Checkpoint& checkpoint) const {
    if (!hook_runtime_) co_return;
    json data;
    data["checkpoint_id"] = checkpoint.id;
    data["thread_id"] = checkpoint.thread_id;
    data["run_id"] = hook_context_.run_id;
    data["checkpoint_parent_id"] = checkpoint.parent_id;
    data["checkpoint_current_node"] = checkpoint.current_node;
    data["checkpoint_next_nodes"] = checkpoint.next_nodes;
    data["checkpoint_phase"] = to_string(checkpoint.interrupt_phase);
    data["checkpoint_step"] = checkpoint.step;
    data["checkpoint_timestamp"] = checkpoint.timestamp;
    data["checkpoint_schema_version"] = checkpoint.schema_version;
    data["checkpoint_metadata"] = checkpoint.metadata;
    data["run_metadata"] = json{{"owner_scope", hook_context_.owner_scope},
                                {"run_id", hook_context_.run_id},
                                {"trace_id", hook_context_.trace_id}};
    // GCC 13 ICEs when the returned awaitable is consumed as a temporary.
    auto emission = hook_runtime_->emit_async(
        HookPhase::CheckpointPublished, "checkpoint_published",
        hook_context_.owner_scope, hook_context_.run_id, std::move(data),
        hook_context_.cancellation, hook_context_.parent_deadline);
    co_await std::move(emission);
}

std::string CheckpointCoordinator::save_super_step(const GraphState&               state,
                                                   const std::string&              current_node,
                                                   const std::vector<std::string>& next_nodes,
                                                   CheckpointPhase                 phase,
                                                   int                             step,
                                                   const std::string&              parent_id,
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
    ctx.start_step = resume_start_step(*cp_opt);

    // Rehydrate in-flight super-step writes so the engine can replay
    // completed tasks instead of re-executing them.
    auto pending = store_->get_writes(thread_id_, ctx.checkpoint_id);
    for (const auto& pw : pending) {
        ctx.replay_results.emplace(pw.task_id, pending_to_node_result(pw));
    }

    return ctx;
}

void CheckpointCoordinator::record_pending_write(const std::string& parent_cp_id,
                                                 const std::string& task_id,
                                                 const std::string& task_path,
                                                 const std::string& node_name,
                                                 const NodeResult&  nr,
                                                 int                step) const {
    if (!enabled()) return;
    // Symmetric with clear_pending_writes: an empty parent_cp_id means
    // we're inside the very first super-step, before any checkpoint has
    // been committed. Pending writes are only useful as a replay log
    // anchored to a parent cp — without one, resume() would return
    // "no checkpoint found" and the user has to restart from scratch
    // anyway. Recording a leak-by-design bucket keyed at parent="" was
    // the source of a single dead row per thread observed in PG.
    if (parent_cp_id.empty()) return;
    store_->put_writes(thread_id_, parent_cp_id,
                       make_pending_write(task_id, task_path, node_name, nr, step));
}

void CheckpointCoordinator::clear_pending_writes(const std::string& parent_cp_id) const {
    if (!enabled()) return;
    if (parent_cp_id.empty()) return;
    store_->clear_writes(thread_id_, parent_cp_id);
}

// ── Async peers (Sem 3.6 incremental) ────────────────────────────────
//
// Each builds the same Checkpoint / PendingWrite shape as the sync
// peer above, then dispatches to the matching CheckpointStore::*_async
// via co_await. The cp/write construction is pure CPU; only the store
// I/O suspends.

asio::awaitable<std::string> CheckpointCoordinator::save_super_step_async(
    const GraphState&               state,
    const std::string&              current_node,
    const std::vector<std::string>& next_nodes,
    CheckpointPhase                 phase,
    int                             step,
    const std::string&              parent_id,
    const BarrierState&             barrier_state) const {
    co_return co_await save_super_step_async(state, current_node, next_nodes, phase, step,
                                             parent_id, barrier_state, json());
}

asio::awaitable<std::string> CheckpointCoordinator::save_super_step_async(
    const GraphState&               state,
    const std::string&              current_node,
    const std::vector<std::string>& next_nodes,
    CheckpointPhase                 phase,
    int                             step,
    const std::string&              parent_id,
    const BarrierState&             barrier_state,
    const json&                     metadata) const {
    if (!enabled()) co_return std::string{};

    Checkpoint cp;
    cp.id              = Checkpoint::generate_id();
    cp.thread_id       = thread_id_;
    cp.channel_values  = state.serialize();
    cp.parent_id       = parent_id;
    cp.current_node    = current_node;
    cp.next_nodes      = next_nodes;
    cp.interrupt_phase = phase;
    cp.barrier_state   = barrier_state;
    cp.metadata        = metadata;
    cp.step            = step;
    cp.timestamp       = now_ms();

    auto      id = cp.id;
    co_await  store_->save_async(cp);
    co_await publish_checkpoint_async(cp);
    co_return id;
}

asio::awaitable<Checkpoint> CheckpointCoordinator::commit_super_step_async(
    const GraphState&               state,
    const std::string&              current_node,
    const std::vector<std::string>& next_nodes,
    int                             step,
    const std::string&              parent_id,
    const BarrierState&             barrier_state,
    const json&                     metadata) const {
    if (!enabled()) {
        throw std::logic_error(
            "Completed super-step commit requires a checkpoint store and thread id");
    }

    Checkpoint checkpoint;
    checkpoint.id              = Checkpoint::generate_id();
    checkpoint.thread_id       = thread_id_;
    checkpoint.channel_values  = state.serialize();
    checkpoint.parent_id       = parent_id;
    checkpoint.current_node    = current_node;
    checkpoint.next_nodes      = next_nodes;
    checkpoint.interrupt_phase = CheckpointPhase::Completed;
    checkpoint.barrier_state   = barrier_state;
    checkpoint.metadata        = metadata;
    checkpoint.step            = step;
    checkpoint.timestamp       = now_ms();

    co_await store_->save_async(checkpoint);
    co_await publish_checkpoint_async(checkpoint);
    co_await clear_pending_writes_async(parent_id);
    co_return checkpoint;
}

asio::awaitable<void> CheckpointCoordinator::record_pending_write_async(
    const std::string& parent_cp_id,
    const std::string& task_id,
    const std::string& task_path,
    const std::string& node_name,
    const NodeResult&  nr,
    int                step) const {
    if (!enabled() || parent_cp_id.empty()) co_return;
    co_await store_->put_writes_async(thread_id_, parent_cp_id,
                                      make_pending_write(task_id, task_path, node_name, nr, step));
}

asio::awaitable<void> CheckpointCoordinator::clear_pending_writes_async(
    const std::string& parent_cp_id) const {
    if (!enabled() || parent_cp_id.empty()) co_return;
    co_await store_->clear_writes_async(thread_id_, parent_cp_id);
}

asio::awaitable<ResumeContext> CheckpointCoordinator::load_for_resume_async() const {
    ResumeContext ctx;
    if (!enabled()) co_return ctx;

    auto cp_opt = co_await store_->load_latest_async(thread_id_);
    if (!cp_opt) co_return ctx;

    ctx.have_cp        = true;
    ctx.checkpoint_id  = cp_opt->id;
    ctx.channel_values = cp_opt->channel_values;
    ctx.phase          = cp_opt->interrupt_phase;
    ctx.next_nodes     = cp_opt->next_nodes;
    ctx.barrier_state  = cp_opt->barrier_state;

    // Same phase-aware step offset as load_for_resume.
    ctx.start_step = resume_start_step(*cp_opt);

    auto pending = co_await store_->get_writes_async(thread_id_, ctx.checkpoint_id);
    for (const auto& pw : pending) {
        ctx.replay_results.emplace(pw.task_id, pending_to_node_result(pw));
    }

    co_return ctx;
}

asio::awaitable<ResumeContext> CheckpointCoordinator::load_for_resume_by_id_async(
    std::string checkpoint_id) const {
    ResumeContext ctx;
    if (!enabled()) co_return ctx;

    auto cp_opt = co_await store_->load_by_id_async(checkpoint_id);
    if (!cp_opt) co_return ctx;
    if (cp_opt->id != checkpoint_id) {
        throw std::runtime_error("Checkpoint store returned a different checkpoint id");
    }
    if (cp_opt->thread_id != thread_id_) {
        throw std::runtime_error("Checkpoint does not belong to resume thread: " + thread_id_);
    }
    if (cp_opt->schema_version != CHECKPOINT_SCHEMA_VERSION) {
        throw std::runtime_error("Checkpoint schema version is incompatible");
    }

    ctx.have_cp        = true;
    ctx.checkpoint_id  = cp_opt->id;
    ctx.channel_values = cp_opt->channel_values;
    ctx.phase          = cp_opt->interrupt_phase;
    ctx.next_nodes     = cp_opt->next_nodes;
    ctx.barrier_state  = cp_opt->barrier_state;

    ctx.start_step = resume_start_step(*cp_opt);

    auto pending = co_await store_->get_writes_async(thread_id_, ctx.checkpoint_id);
    for (const auto& pw : pending) {
        ctx.replay_results.emplace(pw.task_id, pending_to_node_result(pw));
    }

    co_return ctx;
}

}  // namespace neograph::graph
