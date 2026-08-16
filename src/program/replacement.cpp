#include <neograph/program/replacement.h>

#include <neograph/program/command_journal.h>
#include <neograph/program/lineage.h>
#include <neograph/program/run_record.h>

#include "canonical_json.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::string_view FORMAT = "neograph-program-replacement-receipt";
constexpr std::string_view HANDOFF_DOMAIN = "program-replacement-handoff/v1";
constexpr std::string_view INPUT_DOMAIN   = "program-replacement-input/v1";

std::string require_string(const json& value, std::string_view field) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value.at(key).is_string()) {
        throw std::invalid_argument("Program replacement receipt field '" + key +
                                    "' must be a string");
    }
    return value.at(key).get<std::string>();
}

std::uint64_t require_uint64(const json& value, std::string_view field) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value.at(key).is_number_unsigned()) {
        throw std::invalid_argument("Program replacement receipt field '" + key +
                                    "' must be unsigned");
    }
    return value.at(key).get<std::uint64_t>();
}

std::uint32_t require_uint32(const json& value, std::string_view field) {
    const auto result = require_uint64(value, field);
    if (result > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Program replacement receipt integer exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(result);
}

void require_identity(std::string_view value, std::string_view field) {
    if (!detail::is_sha256_identity(value)) {
        throw std::invalid_argument(std::string(field) + " must be a sha256 identity");
    }
}

void validate(ProgramReplacementReceiptData& data) {
    detail::validate_token(data.owner_scope, "Program replacement owner scope");
    detail::validate_token(data.source_run_id, "Program replacement source run id");
    detail::validate_token(data.target_run_id, "Program replacement target run id");
    if (data.source_run_id == data.target_run_id) {
        throw std::invalid_argument("Program replacement target run must differ from its source");
    }
    if (data.source_generation == 0 ||
        data.source_generation == std::numeric_limits<std::uint64_t>::max() ||
        data.target_generation != data.source_generation + 1) {
        throw std::invalid_argument("Program replacement generations must be contiguous");
    }
    require_identity(data.lineage_id, "Program replacement lineage id");
    require_identity(data.source_generation_id, "Program replacement source generation id");
    require_identity(data.source_lineage_head_id, "Program replacement source lineage head id");
    require_identity(data.source_run_record_id, "Program replacement source run record id");
    require_identity(data.source_journal_head, "Program replacement source journal head");
    require_identity(data.source_program_version_id,
                     "Program replacement source ProgramVersion id");
    require_identity(data.source_bundle_id, "Program replacement source bundle id");
    require_identity(data.checkpoint_coordinate_id,
                     "Program replacement checkpoint coordinate id");
    require_identity(data.checkpoint_entry_id, "Program replacement checkpoint entry id");
    require_identity(data.handoff_value_identity, "Program replacement handoff value identity");
    require_identity(data.target_program_version_id,
                     "Program replacement target ProgramVersion id");
    require_identity(data.target_bundle_id, "Program replacement target bundle id");
    require_identity(data.target_input_identity, "Program replacement target input identity");
    require_identity(data.target_invocation_id, "Program replacement target invocation id");
    require_identity(data.target_binding_fingerprint,
                     "Program replacement target binding fingerprint");
    require_identity(data.target_initial_run_record_id,
                     "Program replacement target initial run record id");
    require_identity(data.target_initial_journal_head,
                     "Program replacement target initial journal head");
}

json body(const ProgramReplacementReceiptData& data) {
    return json{{"format", std::string(FORMAT)},
                {"storage_schema_version", ProgramReplacementReceipt::STORAGE_SCHEMA_VERSION},
                {"owner_scope", data.owner_scope},
                {"lineage_id", data.lineage_id},
                {"source_generation", data.source_generation},
                {"source_generation_id", data.source_generation_id},
                {"source_lineage_head_id", data.source_lineage_head_id},
                {"source_run_id", data.source_run_id},
                {"source_run_record_id", data.source_run_record_id},
                {"source_journal_head", data.source_journal_head},
                {"source_program_version_id", data.source_program_version_id},
                {"source_bundle_id", data.source_bundle_id},
                {"checkpoint_coordinate_id", data.checkpoint_coordinate_id},
                {"checkpoint_entry_id", data.checkpoint_entry_id},
                {"handoff_value_identity", data.handoff_value_identity},
                {"target_generation", data.target_generation},
                {"target_run_id", data.target_run_id},
                {"target_program_version_id", data.target_program_version_id},
                {"target_bundle_id", data.target_bundle_id},
                {"target_input_identity", data.target_input_identity},
                {"target_invocation_id", data.target_invocation_id},
                {"target_binding_fingerprint", data.target_binding_fingerprint},
                {"target_initial_run_record_id", data.target_initial_run_record_id},
                {"target_initial_journal_head", data.target_initial_journal_head}};
}

std::string value_identity(std::string_view domain, const json& value) {
    return detail::sha256_identity(domain, detail::canonical_json_bytes(value));
}

bool is_completed_checkpoint(const ProgramJavaScriptCommandJournalEntry& checkpoint,
                             const ProgramReplacementReceipt&             receipt,
                             json&                                        handoff) {
    if (!checkpoint.completed() ||
        checkpoint.command().kind() != JavaScriptCommandKind::Checkpoint ||
        checkpoint.bundle_id() != receipt.source_bundle_id() ||
        checkpoint.coordinate_id() != receipt.checkpoint_coordinate_id() ||
        checkpoint.id() != receipt.checkpoint_entry_id()) {
        return false;
    }
    const auto arguments = checkpoint.command().arguments();
    const auto terminal  = checkpoint.terminal_result();
    if (!arguments.is_object() || !arguments.contains("value") || !terminal ||
        !terminal->is_object() || !terminal->contains("status") ||
        !terminal->at("status").is_string() ||
        terminal->at("status").get<std::string>() != "completed" ||
        !terminal->contains("output")) {
        return false;
    }
    handoff = arguments.at("value");
    return detail::canonical_json_bytes(handoff) ==
               detail::canonical_json_bytes(terminal->at("output")) &&
           receipt.matches_handoff(handoff);
}

}  // namespace

struct ProgramReplacementReceipt::Impl {
    explicit Impl(ProgramReplacementReceiptData value) : data(std::move(value)) {
        auto encoded = body(data);
        id = detail::sha256_identity("program-replacement-receipt/v1",
                                     detail::canonical_json_bytes(encoded));
        encoded["id"] = id;
        canonical_bytes = detail::canonical_json_bytes(encoded);
    }

    ProgramReplacementReceiptData data;
    std::string                   id;
    std::string                   canonical_bytes;
};

ProgramReplacementReceipt::ProgramReplacementReceipt(ProgramReplacementReceiptData data) {
    validate(data);
    impl_ = std::make_shared<const Impl>(std::move(data));
}

ProgramReplacementReceipt::ProgramReplacementReceipt(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

ProgramReplacementReceipt ProgramReplacementReceipt::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    if (!value.is_object() || require_string(value, "format") != FORMAT) {
        throw std::invalid_argument("Stored Program replacement receipt has unknown format");
    }
    detail::reject_unknown_fields(
        value, "Stored Program replacement receipt",
        {"format", "storage_schema_version", "id", "owner_scope", "lineage_id",
         "source_generation", "source_generation_id", "source_lineage_head_id",
         "source_run_id", "source_run_record_id", "source_journal_head",
         "source_program_version_id", "source_bundle_id", "checkpoint_coordinate_id",
         "checkpoint_entry_id", "handoff_value_identity", "target_generation",
         "target_run_id", "target_program_version_id", "target_bundle_id",
         "target_input_identity", "target_invocation_id", "target_binding_fingerprint",
         "target_initial_run_record_id", "target_initial_journal_head"});
    if (require_uint32(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored Program replacement receipt schema is unsupported");
    }
    ProgramReplacementReceipt parsed(ProgramReplacementReceiptData{
        require_string(value, "owner_scope"),
        require_string(value, "lineage_id"),
        require_uint64(value, "source_generation"),
        require_string(value, "source_generation_id"),
        require_string(value, "source_lineage_head_id"),
        require_string(value, "source_run_id"),
        require_string(value, "source_run_record_id"),
        require_string(value, "source_journal_head"),
        require_string(value, "source_program_version_id"),
        require_string(value, "source_bundle_id"),
        require_string(value, "checkpoint_coordinate_id"),
        require_string(value, "checkpoint_entry_id"),
        require_string(value, "handoff_value_identity"),
        require_uint64(value, "target_generation"),
        require_string(value, "target_run_id"),
        require_string(value, "target_program_version_id"),
        require_string(value, "target_bundle_id"),
        require_string(value, "target_input_identity"),
        require_string(value, "target_invocation_id"),
        require_string(value, "target_binding_fingerprint"),
        require_string(value, "target_initial_run_record_id"),
        require_string(value, "target_initial_journal_head")});
    if (parsed.id() != require_string(value, "id")) {
        throw std::invalid_argument("Stored Program replacement receipt id does not match content");
    }
    return parsed;
}

#define NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(name)                    \
    const std::string& ProgramReplacementReceipt::name() const noexcept { \
        return impl_->data.name;                                      \
    }

const std::string& ProgramReplacementReceipt::id() const noexcept { return impl_->id; }
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(owner_scope)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(lineage_id)
std::uint64_t ProgramReplacementReceipt::source_generation() const noexcept {
    return impl_->data.source_generation;
}
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(source_generation_id)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(source_lineage_head_id)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(source_run_id)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(source_run_record_id)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(source_journal_head)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(source_program_version_id)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(source_bundle_id)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(checkpoint_coordinate_id)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(checkpoint_entry_id)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(handoff_value_identity)
std::uint64_t ProgramReplacementReceipt::target_generation() const noexcept {
    return impl_->data.target_generation;
}
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(target_run_id)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(target_program_version_id)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(target_bundle_id)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(target_input_identity)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(target_invocation_id)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(target_binding_fingerprint)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(target_initial_run_record_id)
NEOGRAPH_REPLACEMENT_STRING_ACCESSOR(target_initial_journal_head)

#undef NEOGRAPH_REPLACEMENT_STRING_ACCESSOR

bool ProgramReplacementReceipt::matches_handoff(const json& value) const {
    return impl_->data.handoff_value_identity == program_replacement_handoff_identity(value);
}

bool ProgramReplacementReceipt::matches_target_input(const json& value) const {
    return impl_->data.target_input_identity == program_replacement_input_identity(value);
}

std::string ProgramReplacementReceipt::serialize_canonical() const {
    return impl_->canonical_bytes;
}

std::string program_replacement_handoff_identity(const json& value) {
    return value_identity(HANDOFF_DOMAIN, value);
}

std::string program_replacement_input_identity(const json& value) {
    return value_identity(INPUT_DOMAIN, value);
}

bool is_valid_program_replacement_transition(
    const ProgramRunGeneration&                  predecessor,
    const ProgramRunLineage&                     previous_lineage,
    const ProgramRunRecord&                      source,
    const ProgramJavaScriptCommandJournalEntry& checkpoint,
    const ProgramRunGeneration&                  successor,
    const ProgramRunLineage&                     next_lineage,
    const ProgramRunRecord&                      target) noexcept {
    try {
        const auto receipt = successor.replacement_receipt();
        if (!receipt) return false;
        json handoff;
        if (!is_completed_checkpoint(checkpoint, *receipt, handoff)) return false;

        const auto& input = target.invocation().input;
        return receipt->owner_scope() == source.owner_scope() &&
               receipt->owner_scope() == target.owner_scope() &&
               receipt->lineage_id() == previous_lineage.lineage_id() &&
               receipt->source_generation() == predecessor.generation() &&
               receipt->source_generation_id() == predecessor.id() &&
               receipt->source_lineage_head_id() == previous_lineage.id() &&
               receipt->source_run_id() == source.run_id() &&
               receipt->source_run_record_id() == source.id() &&
               receipt->source_journal_head() == source.journal_head() &&
               receipt->source_program_version_id() == source.program_version_id() &&
               receipt->source_bundle_id() == source.bundle_id() &&
               receipt->target_generation() == successor.generation() &&
               receipt->target_run_id() == target.run_id() &&
               receipt->target_program_version_id() == target.program_version_id() &&
               receipt->target_bundle_id() == target.bundle_id() &&
               receipt->target_invocation_id() == target.invocation().canonical_identity() &&
               receipt->target_binding_fingerprint() == target.binding_fingerprint() &&
               receipt->target_initial_run_record_id() == target.id() &&
               receipt->target_initial_journal_head() == target.journal_head() &&
               receipt->matches_target_input(input) && input.is_object() &&
               input.contains("handoff") && input.contains("previous_run_id") &&
               input.at("previous_run_id").is_string() &&
               input.at("previous_run_id").get<std::string>() == source.run_id() &&
               detail::canonical_json_bytes(input.at("handoff")) ==
                   detail::canonical_json_bytes(handoff) &&
               source.id() == previous_lineage.active_run_record_id() &&
               source.journal_head() == previous_lineage.active_journal_head() &&
               predecessor.id() == previous_lineage.active_generation_id() &&
                predecessor.run_id() == source.run_id() &&
                source.continuation().state == ContinuationState::Running &&
                !source.recorded_binding_set_fingerprint() &&
                !source.pending_input() && !source.pending_effect() && !source.terminal_result() &&
               source.children().empty() && source.child_depth() == 0 &&
               source.invocation().parent_run_id.empty() &&
               previous_lineage.inflight_reservation() == RunBudget{} &&
               previous_lineage.committed_descendant_budget() == RunBudget{} &&
               target.run_id() != source.run_id() && target.child_depth() == 0 &&
               target.invocation().parent_run_id.empty() &&
               target.continuation().state == ContinuationState::Running &&
               !target.pending_input() && !target.pending_effect() && !target.terminal_result() &&
               !target.exact_checkpoint() && target.children().empty() &&
               !target.fork_receipt() && target.created_at_ms() >= source.updated_at_ms() &&
               successor.child_depth() == predecessor.child_depth() &&
               next_lineage.remaining_budget() == program_replacement_remaining_budget(
                                                        source, previous_lineage,
                                                        target.created_at_ms()) &&
               next_lineage.inflight_reservation() == RunBudget{} &&
               next_lineage.committed_descendant_budget() == RunBudget{} &&
               target.remaining_budget() == next_lineage.remaining_budget() &&
               target.invocation().budget == next_lineage.remaining_budget();
    } catch (const std::exception&) {
        return false;
    }
}

RunBudget program_replacement_remaining_budget(const ProgramRunRecord&  source,
                                               const ProgramRunLineage& source_lineage,
                                               std::int64_t transition_time_ms) noexcept {
    auto result = source_lineage.remaining_budget();
    if (transition_time_ms <= source.created_at_ms()) return result;
    const auto elapsed = static_cast<std::uint64_t>(transition_time_ms - source.created_at_ms());
    const auto granted = source.invocation().budget.wall_time_ms;
    const auto already_charged =
        granted > result.wall_time_ms ? granted - result.wall_time_ms : 0;
    const auto additional = elapsed > already_charged ? elapsed - already_charged : 0;
    result.wall_time_ms = additional >= result.wall_time_ms ? 0 : result.wall_time_ms - additional;
    return result;
}

}  // namespace neograph::program
