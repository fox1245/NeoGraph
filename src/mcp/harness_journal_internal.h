#pragma once

#include <neograph/mcp/harness.h>

#include <cstddef>
#include <memory>
#include <string>

namespace neograph::mcp::detail {

inline constexpr const char* kHarnessRedactedMarker = "[REDACTED]";

struct HarnessJournalContext {
    std::shared_ptr<HarnessJournal> journal;
    std::string run_id;
    std::string artifact_id;
    std::string revision_digest;
    std::string protocol_version;
    std::string profile;
    std::string node_id;
    std::string worker_id;
    std::size_t attempt = 0;
};

class ScopedHarnessJournalContext {
public:
    explicit ScopedHarnessJournalContext(HarnessJournalContext context);
    ~ScopedHarnessJournalContext();

    ScopedHarnessJournalContext(const ScopedHarnessJournalContext&) = delete;
    ScopedHarnessJournalContext& operator=(const ScopedHarnessJournalContext&) = delete;

private:
    HarnessJournalContext context_;
    const HarnessJournalContext* previous_ = nullptr;
};

std::string journal_correlation_id(const char* prefix);

void append_harness_journal_event(const HarnessJournalContext& context,
                                  const std::string&           event_type,
                                  json                         payload = json::object(),
                                  std::string                  correlation_id = {});

void append_current_harness_journal_event(const std::string& event_type,
                                          json               payload = json::object(),
                                          std::string        correlation_id = {});

}  // namespace neograph::mcp::detail
