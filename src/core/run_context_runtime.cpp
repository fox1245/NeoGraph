#include "run_context_runtime.h"

#include "channel_write_codec.h"

#include <mutex>
#include <unordered_map>
#include <utility>

namespace neograph::graph::detail {

namespace {

std::mutex runtime_mutex;
std::unordered_map<const RunContext*, std::shared_ptr<const RunContextRuntime>> runtimes;

constexpr const char* kMetadataNamespace = "_neograph";
constexpr const char* kJournalVersion = "subgraph_write_journal_version";
constexpr const char* kJournal = "subgraph_write_journal";

}  // namespace

std::shared_ptr<const RunContextRuntime> runtime_for(const RunContext& context) {
    std::lock_guard<std::mutex> lock(runtime_mutex);
    const auto it = runtimes.find(&context);
    return it == runtimes.end() ? nullptr : it->second;
}

std::shared_ptr<const RunContextRuntime> runtime_for_invocation(
    const RunContext& context, const std::string& invocation_id) {
    auto parent = runtime_for(context);
    auto runtime = parent
        ? std::make_shared<RunContextRuntime>(*parent)
        : std::make_shared<RunContextRuntime>();
    runtime->invocation_id = invocation_id;
    return runtime;
}

void append_applied_writes(const RunContext& context,
                           const std::vector<ChannelWrite>& writes) {
    if (writes.empty()) return;
    auto runtime = runtime_for(context);
    if (!runtime || !runtime->subgraph_write_journal) return;
    auto& journal = runtime->subgraph_write_journal->writes;
    journal.insert(journal.end(), writes.begin(), writes.end());
}

json checkpoint_metadata_for(const RunContext& context) {
    auto runtime = runtime_for(context);
    if (!runtime || !runtime->subgraph_write_journal) return json();

    json metadata;
    metadata[kMetadataNamespace][kJournalVersion] = 1;
    metadata[kMetadataNamespace][kJournal] =
        serialize_channel_writes(runtime->subgraph_write_journal->writes);
    return metadata;
}

void restore_subgraph_write_journal(
    const Checkpoint& checkpoint,
    const std::shared_ptr<SubgraphWriteJournal>& journal) {
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

    journal->writes = deserialize_channel_writes(
        checkpoint.metadata[kMetadataNamespace][kJournal]);
}

ScopedRunContextRuntime::ScopedRunContextRuntime(
    const RunContext& context,
    std::shared_ptr<const RunContextRuntime> runtime)
    : context_(&context) {
    if (!runtime) return;

    std::lock_guard<std::mutex> lock(runtime_mutex);
    const auto it = runtimes.find(context_);
    if (it != runtimes.end()) previous_ = it->second;
    runtime_ = std::move(runtime);
    runtimes[context_] = runtime_;
    installed_ = true;
}

ScopedRunContextRuntime::~ScopedRunContextRuntime() {
    if (!installed_) return;

    std::lock_guard<std::mutex> lock(runtime_mutex);
    const auto current = runtimes.find(context_);
    if (current == runtimes.end() || current->second != runtime_) return;
    if (previous_) {
        runtimes[context_] = std::move(previous_);
    } else {
        runtimes.erase(context_);
    }
}

}  // namespace neograph::graph::detail
