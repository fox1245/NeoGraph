#pragma once

#include <neograph/program/core_provider_call.h>
#include <neograph/types.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::program {

/** Host-owned SQLite dispatch journal. Share the journal across resolver calls/runs.
 * A marker means transport MAY have occurred; it does not prove provider receipt
 * or exactly-once execution. Pending/uncertain calls require external reconciliation.
 */
class NEOGRAPH_PROGRAM_API SQLiteProgramProviderCallJournal final {
public:
    explicit SQLiteProgramProviderCallJournal(std::string database_path);
    ~SQLiteProgramProviderCallJournal();
    SQLiteProgramProviderCallJournal(const SQLiteProgramProviderCallJournal&) = delete;
    SQLiteProgramProviderCallJournal& operator=(const SQLiteProgramProviderCallJournal&) = delete;

    /** The provider identity is the host's immutable deployment/credential-route digest.
     * It must change when any dispatch authority or deployment changes.
     */
    ProgramCoreProviderCallBinding bind(const ProgramCoreProviderCallContext& context,
                                        std::string provider_binding_identity);
    /** Stable across Program attempts; request content is bound to the slot. */
    static std::string logical_call_id(const ProgramCoreProviderCallContext& context,
                                       const graph::ProviderCallIdentity& call);

    enum class State { Dispatched, Succeeded, ReconciliationRequired };
    struct Record {
        State state;
        std::string request_digest;
        std::string provider_binding_identity;
        std::string original_attempt;
        std::string outcome_digest;
    };
    std::optional<Record> inspect(std::string_view owner_scope,
                                  std::string_view dispatch_id) const;

    /** Only after independent provider-side evidence establishes this exact result.
     * The evidence identity is retained, immutable and must not be empty. This
     * method never retries transport; a later invoke replays the verified result.
     */
    void reconcile_success(std::string_view owner_scope, std::string_view dispatch_id,
                           std::string evidence_identity, ChatCompletion completion);

    struct Impl;
private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace neograph::program
