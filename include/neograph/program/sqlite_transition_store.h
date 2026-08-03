/**
 * @file program/sqlite_transition_store.h
 * @brief SQLite-backed atomic Program transition publication.
 */
#pragma once

#include <neograph/program/transition_store.h>

#include <memory>
#include <string>
#include <string_view>

namespace neograph::program {

/**
 * Durable owner-scoped ProgramTransitionStore.
 *
 * Each publication is stored as one canonical immutable blob and committed in
 * one SQLite transaction.  The blob boundary deliberately mirrors the
 * backend-neutral publication contract: a restart can expose either the
 * previous complete publication or the next complete publication, never a
 * partially written run/journal/event/effect set.
 */
class NEOGRAPH_PROGRAM_API SQLiteProgramTransitionStore final
    : public ProgramTransitionStore {
public:
    explicit SQLiteProgramTransitionStore(std::string database_path);
    SQLiteProgramTransitionStore(SQLiteProgramTransitionStore&&) noexcept;
    SQLiteProgramTransitionStore& operator=(SQLiteProgramTransitionStore&&) noexcept;
    SQLiteProgramTransitionStore(const SQLiteProgramTransitionStore&) = delete;
    SQLiteProgramTransitionStore& operator=(const SQLiteProgramTransitionStore&) = delete;
    ~SQLiteProgramTransitionStore() override;

    std::optional<ProgramRunRecord> load(std::string_view owner_scope,
                                         std::string_view run_id) const override;
    std::optional<ProgramJournalRecord> latest(std::string_view owner_scope,
                                               std::string_view run_id) const override;
    std::vector<ProgramEvent> load_events(std::string_view owner_scope,
                                           std::string_view run_id,
                                           std::uint64_t after_sequence = 0) const override;
    std::vector<ProgramEffectOutboxEntry>
    load_effects(std::string_view owner_scope,
                 std::string_view run_id,
                 std::uint64_t after_sequence = 0) const override;
    ProgramTransitionPublishResult
    compare_publish(std::string_view owner_scope,
                    std::string_view expected_journal_head,
                    ProgramTransitionPublication publication) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
