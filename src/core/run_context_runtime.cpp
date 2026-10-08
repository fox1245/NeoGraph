#include "run_context_runtime.h"

#include "channel_write_codec.h"
#include "managed_budget_journal.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace neograph::graph::detail {

namespace {

constexpr const char* kMetadataNamespace = "_neograph";
constexpr const char* kJournalVersion = "subgraph_write_journal_version";
constexpr const char* kJournal = "subgraph_write_journal";
constexpr const char* kGraphInvocation = "subgraph_invocation_id";

/// Maps a live RunContext to its binding. Every node invocation registers and
/// unregisters one entry, and invocations of unrelated runs overlap on many
/// threads. One lock for the whole process would serialise all of them and bounce
/// its cache line between cores, so the map is split into shards chosen by the
/// context's address: a thread normally touches only the shards of its own
/// contexts. Each shard is open-addressed over a flat array (no allocation per
/// entry, one lock round trip per operation). Entries point at bindings owned by
/// their scope objects, which unregister before they die. The registry itself is
/// never destroyed, so a scope that outlives static destruction still finds it.
class BindingRegistry {
public:
    const RuntimeBinding* find(const RunContext* key) const {
        const std::uint64_t hash = hash_of(key);
        return shard_for(hash).find(key, hash);
    }

    void install(const RunContext* key, RuntimeBinding& binding) {
        const std::uint64_t hash = hash_of(key);
        shard_for(hash).install(key, hash, binding);
    }

    /// Installs `binding` for `key` with the run-wide state `parent` is bound
    /// to (or `fallback` when it is bound to nothing). The parent's scope is
    /// alive for the whole call, so its binding cannot go away between the
    /// lookup and the install.
    void install_inheriting(const RunContext* parent, const RunContext* key, RuntimeBinding& binding,
                            const std::shared_ptr<const RunContextRuntime>& fallback) {
        const std::uint64_t parent_hash = hash_of(parent);
        std::shared_ptr<const RunContextRuntime> inherited = shard_for(parent_hash).run_of(parent, parent_hash);
        binding.run = inherited ? std::move(inherited) : fallback;
        install(key, binding);
    }

    void uninstall(const RunContext* key, const RuntimeBinding& binding) {
        const std::uint64_t hash = hash_of(key);
        shard_for(hash).uninstall(key, hash, binding);
    }

private:
    static constexpr std::size_t kShards = 64;  // power of two

    static std::uint64_t hash_of(const RunContext* key) noexcept {
        auto x = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(key) >> 4);
        x *= 0x9E3779B97F4A7C15ull;
        return x ^ (x >> 31);
    }

    /// One independently locked table. Aligned so that neighbouring shards do
    /// not share a cache line.
    class alignas(64) Shard {
    public:
        const RuntimeBinding* find(const RunContext* key, std::uint64_t hash) const {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::size_t index = locate(key, hash);
            return index == npos ? nullptr : slots_[index].binding;
        }

        std::shared_ptr<const RunContextRuntime> run_of(const RunContext* key, std::uint64_t hash) const {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::size_t index = locate(key, hash);
            return index == npos ? nullptr : slots_[index].binding->run;
        }

        void install(const RunContext* key, std::uint64_t hash, RuntimeBinding& binding) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (slots_.empty()) slots_.resize(kInitialCapacity);
            if ((used_ + 1) * 2 > slots_.size()) grow();
            const std::size_t mask = slots_.size() - 1;
            std::size_t i = slot_of(hash, mask);
            while (slots_[i].key != nullptr && slots_[i].key != key) i = (i + 1) & mask;
            if (slots_[i].key == key) {
                binding.previous = slots_[i].binding;
            } else {
                binding.previous = nullptr;
                slots_[i].key = key;
                ++used_;
            }
            slots_[i].binding = &binding;
        }

        void uninstall(const RunContext* key, std::uint64_t hash, const RuntimeBinding& binding) {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::size_t index = locate(key, hash);
            if (index == npos || slots_[index].binding != &binding) return;
            if (binding.previous != nullptr) {
                slots_[index].binding = binding.previous;
            } else {
                erase_at(index);
                // A burst of concurrent invocations must not leave its table behind.
                if (used_ == 0 && slots_.size() > kRetainedCapacity) std::vector<Slot>().swap(slots_);
            }
        }

    private:
        struct Slot {
            const RunContext* key = nullptr;
            const RuntimeBinding* binding = nullptr;
        };
        static constexpr std::size_t npos = static_cast<std::size_t>(-1);
        static std::size_t slot_of(std::uint64_t hash, std::size_t mask) noexcept {
            return static_cast<std::size_t>(hash & mask);
        }
        static constexpr std::size_t kInitialCapacity = 16;    // power of two
        static constexpr std::size_t kRetainedCapacity = 256;  // largest table kept while empty

        std::size_t locate(const RunContext* key, std::uint64_t hash) const {
            if (slots_.empty()) return npos;
            const std::size_t mask = slots_.size() - 1;
            for (std::size_t i = slot_of(hash, mask);; i = (i + 1) & mask) {
                if (slots_[i].key == key) return i;
                if (slots_[i].key == nullptr) return npos;
            }
        }

        void grow() {
            std::vector<Slot> old(slots_.size() * 2);
            old.swap(slots_);
            const std::size_t mask = slots_.size() - 1;
            for (const Slot& slot : old) {
                if (slot.key == nullptr) continue;
                std::size_t i = slot_of(hash_of(slot.key), mask);
                while (slots_[i].key != nullptr) i = (i + 1) & mask;
                slots_[i] = slot;
            }
        }

        /// Removes slot `hole` and shifts later members of its probe run back,
        /// so lookups never need tombstones.
        void erase_at(std::size_t hole) {
            const std::size_t mask = slots_.size() - 1;
            std::size_t next = hole;
            for (;;) {
                next = (next + 1) & mask;
                if (slots_[next].key == nullptr) break;
                const std::size_t home = slot_of(hash_of(slots_[next].key), mask);
                const bool home_in_gap = hole <= next ? (hole < home && home <= next)
                                                      : (hole < home || home <= next);
                if (home_in_gap) continue;
                slots_[hole] = slots_[next];
                hole = next;
            }
            slots_[hole] = Slot{};
            --used_;
        }

        mutable std::mutex mutex_;
        std::vector<Slot> slots_;
        std::size_t used_ = 0;
    };

    // The slot index inside a shard uses the low bits of the hash, the shard the
    // high ones, so the two choices are independent. The hash stays 64-bit on
    // every target: 32-bit builds still need bits 40 and up to pick a shard.
    static std::size_t shard_index(std::uint64_t hash) noexcept {
        return static_cast<std::size_t>((hash >> 40) & (kShards - 1));
    }
    const Shard& shard_for(std::uint64_t hash) const { return shards_[shard_index(hash)]; }
    Shard& shard_for(std::uint64_t hash) { return shards_[shard_index(hash)]; }

    std::array<Shard, kShards> shards_;
};

BindingRegistry& registry() {
    static BindingRegistry* const instance = new BindingRegistry();
    return *instance;
}

const std::shared_ptr<const RunContextRuntime>& empty_runtime() {
    static const std::shared_ptr<const RunContextRuntime> instance =
        std::make_shared<const RunContextRuntime>();
    return instance;
}

}  // namespace

RuntimeView runtime_for(const RunContext& context) {
    RuntimeView view;
    if (const RuntimeBinding* binding = registry().find(&context)) {
        view.run_ = binding->run.get();
        view.invocation_id_ = binding->invocation_id;
    }
    return view;
}

void append_applied_writes(const RunContext& context,
                           const std::vector<ChannelWrite>& writes) {
    if (writes.empty()) return;
    auto runtime = runtime_for(context);
    if (!runtime || !runtime->subgraph_write_journal) return;
    auto& journal = runtime->subgraph_write_journal->writes;
    journal.insert(journal.end(), writes.begin(), writes.end());
}

CheckpointPhase checkpoint_resume_phase(const Checkpoint& checkpoint) {
    if (checkpoint.interrupt_phase != CheckpointPhase::Updated)
        return checkpoint.interrupt_phase;
    const auto& metadata = checkpoint.metadata;
    if (!metadata.is_object() || !metadata.contains("_neograph") ||
        !metadata["_neograph"].is_object() ||
        !metadata["_neograph"].contains("admin_resume_phase"))
        return checkpoint.interrupt_phase;
    return parse_checkpoint_phase(
        metadata["_neograph"]["admin_resume_phase"].get<std::string>());
}

json checkpoint_metadata_for(const RunContext& context) {
    auto runtime = runtime_for(context);
    if (!runtime || !runtime->checkpoint_store) return json();

    json metadata;
    if (!runtime->graph_invocation_id.empty())
        metadata[kMetadataNamespace][kGraphInvocation] = runtime->graph_invocation_id;
    if (runtime->subgraph_write_journal) {
        metadata[kMetadataNamespace][kJournalVersion] = 1;
        metadata[kMetadataNamespace][kJournal] =
            serialize_channel_writes(runtime->subgraph_write_journal->writes, context.native_history_archive,
                context.managed_budget_lease
                    ? ManagedBudgetJournalAccess::retains_native_checkpoint(context.managed_budget_lease)
                    : runtime->checkpoint_store->retains_native_checkpoint());
        if (!runtime->subgraph_write_journal->parent_call_id.empty())
            metadata[kMetadataNamespace]["subgraph_parent_call_id"] =
                runtime->subgraph_write_journal->parent_call_id;
    }
    return metadata;
}

void restore_subgraph_write_journal(
    const Checkpoint& checkpoint,
    const std::shared_ptr<SubgraphWriteJournal>& journal,
    const std::shared_ptr<sp::NativeArchive>& archive) {
    if (!journal) return;

    const bool has_journal = checkpoint.metadata.is_object()
        && checkpoint.metadata.contains(kMetadataNamespace)
        && checkpoint.metadata[kMetadataNamespace].is_object()
        && checkpoint.metadata[kMetadataNamespace].value(kJournalVersion, 0) == 1
        && checkpoint.metadata[kMetadataNamespace].contains(kJournal);
    if (!has_journal) {
        throw std::runtime_error(
            "Cannot resume subgraph checkpoint without a write journal; "
            "restart the parent invocation with NeoGraph checkpoint schema v4 or newer");
    }

    if (checkpoint.native_subgraph_writes) {
        if (checkpoint.metadata[kMetadataNamespace][kJournal] !=
            serialize_channel_writes(*checkpoint.native_subgraph_writes, {}, true))
            throw std::invalid_argument("Subgraph journal projection differs from original typed custody");
        journal->writes = *checkpoint.native_subgraph_writes;
        return;
    }
    journal->writes = deserialize_channel_writes(
        checkpoint.metadata[kMetadataNamespace][kJournal], archive);
}

ScopedRunContextRuntime::ScopedRunContextRuntime(
    const RunContext& context,
    std::shared_ptr<const RunContextRuntime> runtime)
    : context_(&context) {
    if (runtime == nullptr) return;

    binding_.run = std::move(runtime);
    registry().install(context_, binding_);
    installed_ = true;
}

ScopedRunContextRuntime::~ScopedRunContextRuntime() {
    if (installed_) registry().uninstall(context_, binding_);
}

ScopedInvocationRuntime::ScopedInvocationRuntime(
    const RunContext& parent, const RunContext& context, std::string_view invocation_id)
    : context_(&context) {
    binding_.invocation_id.assign(invocation_id.data(), invocation_id.size());
    registry().install_inheriting(&parent, context_, binding_, empty_runtime());
}

ScopedInvocationRuntime::~ScopedInvocationRuntime() {
    registry().uninstall(context_, binding_);
}

}  // namespace neograph::graph::detail

namespace neograph::graph {

ToolExecutionContext make_tool_execution_context(const RunContext& ctx) {
    ToolExecutionContext execution;
    execution.cancel_token = ctx.cancel_token;
    execution.controller = ctx.tool_execution_controller;
    execution.identity = ctx.tool_execution_identity;
    execution.identity.thread_id = ctx.thread_id;
    execution.deadline = ctx.deadline;
    if (const auto runtime = detail::runtime_for(ctx)) {
        execution.effect_broker = runtime->tool_effect_broker;
        execution.effect_task_id.assign(runtime.invocation_id().data(), runtime.invocation_id().size());
        execution.effect_grant = runtime->tool_effect_grant;
    }
    return execution;
}

}  // namespace neograph::graph
