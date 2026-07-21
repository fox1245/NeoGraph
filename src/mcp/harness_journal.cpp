#include "harness_journal_internal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <sstream>
#include <utility>

namespace neograph::mcp::detail {
namespace {

thread_local const HarnessJournalContext* current_context = nullptr;

int64_t unix_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

ScopedHarnessJournalContext::ScopedHarnessJournalContext(HarnessJournalContext context)
    : context_(std::move(context)), previous_(current_context) {
    current_context = &context_;
}

ScopedHarnessJournalContext::~ScopedHarnessJournalContext() {
    current_context = previous_;
}

std::string journal_correlation_id(const char* prefix) {
    static const std::uint64_t        nonce = std::random_device{}();
    static std::atomic<std::uint64_t> sequence{1};
    std::ostringstream                out;
    out << prefix << '_' << std::hex << nonce << '_'
        << sequence.fetch_add(1, std::memory_order_relaxed);
    return out.str();
}

void append_harness_journal_event(const HarnessJournalContext& context,
                                  const std::string&            event_type,
                                  json                          payload,
                                  std::string                   correlation_id) {
    if (!context.journal) return;
    json event = {
        {"run_id", context.run_id},
        {"artifact_id", context.artifact_id},
        {"revision_digest", context.revision_digest},
        {"protocol_version", context.protocol_version},
        {"profile", context.profile},
        {"event_type", event_type},
        {"created_at_ms", unix_millis()},
        {"payload", std::move(payload)},
    };
    if (!context.node_id.empty()) event["node_id"] = context.node_id;
    if (!context.worker_id.empty()) event["worker_id"] = context.worker_id;
    if (context.attempt > 0) event["attempt"] = context.attempt;
    if (!correlation_id.empty()) event["correlation_id"] = std::move(correlation_id);
    context.journal->append_event(event);
}

void append_current_harness_journal_event(const std::string& event_type,
                                          json               payload,
                                          std::string        correlation_id) {
    if (!current_context) return;
    append_harness_journal_event(*current_context, event_type, std::move(payload),
                                 std::move(correlation_id));
}

}  // namespace neograph::mcp::detail
