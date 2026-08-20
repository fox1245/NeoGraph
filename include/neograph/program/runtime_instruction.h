/** @file program/runtime_instruction.h @brief Durable developer instruction planning. */
#pragma once

#include <neograph/context_store.h>
#include <neograph/program/catalog.h>
#include <neograph/program/runtime.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

struct RuntimeDeveloperInstructionData {
    std::string owner_scope;
    std::string source_run_id;
    std::string feed_id;
    std::uint64_t sequence = 0;
    std::optional<std::string> predecessor_record_id;
    std::int64_t submitted_at_ms = 0;
    std::string text;
    json payload = json::object();
    std::vector<std::string> requested_capabilities;
    std::vector<std::string> requested_effects;
};

/** Immutable developer input. It requests change but grants no authority. */
class NEOGRAPH_PROGRAM_API RuntimeDeveloperInstruction final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;
    static RuntimeDeveloperInstruction create(RuntimeDeveloperInstructionData data);
    static RuntimeDeveloperInstruction parse(std::string_view stored_bytes);

    const RuntimeDeveloperInstructionData& data() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit RuntimeDeveloperInstruction(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

enum class RuntimeInstructionAction : std::uint8_t {
    SatisfiedInPlace,
    Rejected,
    ReplaceAtHandoff,
    MigrateGraph,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(RuntimeInstructionAction action) noexcept;
NEOGRAPH_PROGRAM_API RuntimeInstructionAction runtime_instruction_action_from_string(
    std::string_view value);

struct RuntimeInstructionDecisionData {
    std::string instruction_id;
    std::string source_run_id;
    std::string expected_lineage_head_id;
    std::string policy_identity;
    RuntimeInstructionAction action = RuntimeInstructionAction::Rejected;
    std::string target_program_version_id;
    std::string target_run_id;
    std::string reason;
};

/** Immutable host decision over one exact active generation. */
class NEOGRAPH_PROGRAM_API RuntimeInstructionDecision final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;
    static RuntimeInstructionDecision create(RuntimeInstructionDecisionData data);
    static RuntimeInstructionDecision parse(std::string_view stored_bytes);

    const RuntimeInstructionDecisionData& data() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit RuntimeInstructionDecision(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

using RuntimeInstructionPlanner = std::function<RuntimeInstructionDecisionData(
    const RuntimeDeveloperInstruction&,
    const ProgramRunLineage&,
    const ProgramRunGeneration&)>;

struct RuntimeInstructionPlan {
    RuntimeHistoryRecord history_record;
    RuntimeInstructionDecision decision;
    ContextArtifact decision_artifact;
};

struct RuntimeInstructionControllerConfig {
    std::shared_ptr<DurableContextStore> context_store;
    std::shared_ptr<ProgramTransitionStore> transitions;
    std::shared_ptr<ProgramCatalog> catalog;
    RuntimeInstructionPlanner planner;
};

/** Host-owned bridge from durable developer input to existing transition APIs. */
class NEOGRAPH_PROGRAM_API RuntimeInstructionController final {
public:
    explicit RuntimeInstructionController(RuntimeInstructionControllerConfig config);

    RuntimeInstructionPlan submit_and_plan(
        const RuntimeDeveloperInstruction& instruction,
        const std::optional<std::string>& expected_history_head_id);

    ProgramHandle apply_replacement(
        const RuntimeInstructionDecision& decision,
        ProgramRuntime& runtime,
        ExactProgramHandoffReference source,
        RunInvocation invocation,
        std::shared_ptr<ProgramEventSink> events = {}) const;

    ProgramHandle apply_graph_migration(
        const RuntimeInstructionDecision& decision,
        ProgramRuntime& runtime,
        const ProgramHandle& source,
        std::shared_ptr<ProgramEventSink> events = {}) const;

private:
    RuntimeInstructionControllerConfig config_;
};

}  // namespace neograph::program
