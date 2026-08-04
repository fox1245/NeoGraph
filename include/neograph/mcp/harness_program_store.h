#pragma once

#include <neograph/api.h>
#include <neograph/json.h>
#include <neograph/mcp/harness_program_translator.h>
#include <neograph/program/store.h>
#include <neograph/program/contract.h>
#include <neograph/program/transition_store.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::mcp {

class HarnessRecordStore;

/**
 * Strict P2 compatibility record for one Harness artifact alias.
 *
 * This is deliberately a bounded adapter value, not a general Program store format.
 * The canonical ProgramBundle and ProgramVersion bytes remain the authority and are
 * reparsed on every load. The invocation template is bound to an exact
 * RunInvocation only when the Harness generates a run ID.
 */
class NEOGRAPH_HARNESS_API HarnessProgramArtifactRecord {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 2;

    static HarnessProgramArtifactRecord create(std::string               artifact_id,
                                               std::string               owner_scope,
                                               program::ProgramBundle    bundle,
                                               program::ProgramVersion   version,
                                               HarnessInvocationTemplate invocation_template,
                                               json                      projection);
    static HarnessProgramArtifactRecord parse(const json& stored);

    const std::string&               artifact_id() const noexcept;
    const std::string&               owner_scope() const noexcept;
    const program::ProgramBundle&    bundle() const noexcept;
    const program::ProgramVersion&   version() const noexcept;
    const HarnessInvocationTemplate& invocation_template() const noexcept;
    json                             projection() const;

    json serialize() const;

private:
    struct Impl;
    explicit HarnessProgramArtifactRecord(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

/**
 * Strict adapter wrapper for one canonical ProgramRunRecord.
 *
 * The Program run bytes and exact canonical RunInvocation remain authoritative.
 * Adapter-only fields retain the Harness artifact alias and projection required
 * to reconstruct Harness views after a cold restart.
 */
class NEOGRAPH_HARNESS_API HarnessProgramRunRecord {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 2;

    static HarnessProgramRunRecord create(const HarnessProgramArtifactRecord& artifact,
                                          program::ProgramRunRecord            run_record,
                                          json                                 projection);
    static HarnessProgramRunRecord parse(const json& stored);

    const std::string&               artifact_id() const noexcept;
    const std::string&               owner_scope() const noexcept;
    const program::ProgramRunRecord& run_record() const noexcept;
    const program::RunInvocation&    invocation() const noexcept;
    json                             projection() const;
    void validate_artifact(const HarnessProgramArtifactRecord& artifact) const;
    json serialize() const;

private:
    struct Impl;
    explicit HarnessProgramRunRecord(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

/**
 * Temporary PR7--PR12 Harness alias/binding factory.
 *
 * It creates the canonical ProgramTransitionStore bounded to one exact retained
 * artifact tuple. It does not define transition semantics.
 */
class NEOGRAPH_HARNESS_API HarnessProgramAdapterStore {
public:
    virtual ~HarnessProgramAdapterStore() = default;

    virtual std::shared_ptr<program::ProgramTransitionStore>
    bind_program_transitions(HarnessProgramArtifactRecord artifact) = 0;

    /** Wrong-owner lookups are indistinguishable from absence. */
    virtual std::optional<HarnessProgramRunRecord>
    resolve_program_run(std::string_view owner_scope, std::string_view run_id) const = 0;
};

/**
 * Optional durable boundary for the Harness contract state associated with a
 * canonical Program run. This is a sibling interface so existing
 * HarnessRecordStore and Program adapter vtables remain compatible.
 */
class NEOGRAPH_HARNESS_API HarnessContractRunStore {
public:
    virtual ~HarnessContractRunStore() = default;

    virtual void save_contract_run(const HarnessProgramArtifactRecord& artifact,
                                   const program::ProgramRunRecord&     run_record,
                                   const program::ContractRun&         contract_run) = 0;
    virtual std::optional<program::ContractRun>
    load_contract_run(const HarnessProgramArtifactRecord& artifact,
                      const program::ProgramRunRecord&     run_record) const = 0;
};

/** Rejects FileHarnessRecordStore and every backend without atomic Program publication. */
NEOGRAPH_HARNESS_API std::shared_ptr<HarnessProgramAdapterStore>
require_harness_program_adapter_store(const std::shared_ptr<HarnessRecordStore>& records);

/**
 * One-artifact ProgramStore view over HarnessRecordStore.
 *
 * The adapter exposes only the exact bundle/version tuple stored under artifact_id.
 * It cannot enumerate, activate, garbage-collect, or act as the PR12 generic SQLite
 * ProgramStore.
 */
class NEOGRAPH_HARNESS_API HarnessBoundedProgramStore final : public program::ProgramStore {
public:
    HarnessBoundedProgramStore(std::shared_ptr<HarnessRecordStore> records,
                               std::string                         artifact_id,
                               std::string                         owner_scope,
                               HarnessInvocationTemplate           invocation_template,
                               json                                projection);
    ~HarnessBoundedProgramStore() override;

    HarnessBoundedProgramStore(const HarnessBoundedProgramStore&)            = delete;
    HarnessBoundedProgramStore& operator=(const HarnessBoundedProgramStore&) = delete;
    HarnessBoundedProgramStore(HarnessBoundedProgramStore&&) noexcept;
    HarnessBoundedProgramStore& operator=(HarnessBoundedProgramStore&&) noexcept;

    void publish_admitted(const program::ProgramBundle&  bundle,
                          const program::ProgramVersion& version) override;
    std::optional<program::ProgramBundle>  get_bundle(std::string_view id) const override;
    std::optional<program::ProgramVersion> get_version(std::string_view id) const override;
    std::optional<program::ProgramBundle>
    get_bundle(std::string_view owner_scope, std::string_view id) const override;
    std::optional<program::ProgramVersion>
    get_version(std::string_view owner_scope, std::string_view id) const override;

    /** Owner-scoped strict load. Wrong owners are indistinguishable from absent IDs. */
    std::optional<HarnessProgramArtifactRecord> load_artifact() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** ProgramJournal compatibility view backed exclusively by ProgramTransitionStore CAS. */
class NEOGRAPH_HARNESS_API HarnessBoundedProgramJournal final : public program::ProgramJournal {
public:
    HarnessBoundedProgramJournal(std::shared_ptr<program::ProgramTransitionStore> transitions,
                                 std::string owner_scope);
    ~HarnessBoundedProgramJournal() override;

    std::optional<program::ProgramJournalRecord> latest(std::string_view run_id) const override;
    program::JournalAppendResult compare_append(
        std::string_view expected_previous_id,
        program::ProgramJournalRecord record) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::mcp
