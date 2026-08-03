#include <neograph/program/journal.h>

#include "canonical_json.h"

#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::string_view JOURNAL_FORMAT = "neograph-program-journal-record";

std::string require_string(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_string()) {
        throw std::invalid_argument("Program journal field '" + owned_key + "' must be a string");
    }
    return value[owned_key].get<std::string>();
}

std::uint64_t require_uint64(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_number_unsigned()) {
        throw std::invalid_argument("Program journal field '" + owned_key + "' must be unsigned");
    }
    return value[owned_key].get<std::uint64_t>();
}

std::uint32_t require_uint32(const json& value, std::string_view key) {
    const auto number = require_uint64(value, key);
    if (number > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Program journal integer exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(number);
}

std::int64_t require_int64(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_number_integer()) {
        throw std::invalid_argument("Program journal field '" + owned_key + "' must be an integer");
    }
    if (value[owned_key].is_number_unsigned()) {
        const auto number = value[owned_key].get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::invalid_argument("Program journal integer exceeds int64 range");
        }
        return static_cast<std::int64_t>(number);
    }
    return value[owned_key].get<std::int64_t>();
}

json require_value(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key)) {
        throw std::invalid_argument("Program journal requires field '" + owned_key + "'");
    }
    return value[owned_key];
}

void require_token(std::string_view value, std::string_view field) {
    detail::validate_token(value, field);
}

void require_sha256(std::string_view value, std::string_view field) {
    if (!detail::is_sha256_identity(value)) {
        throw std::invalid_argument(std::string(field) + " must be a sha256 identity");
    }
}

json encode_budget(const RunBudget& budget) {
    return json{{"wall_time_ms", budget.wall_time_ms},
                {"model_tokens", budget.model_tokens},
                {"monetary_microunits", budget.monetary_microunits},
                {"max_concurrency", budget.max_concurrency},
                {"max_program_operations", budget.max_program_operations},
                {"max_core_steps", budget.max_core_steps},
                {"max_dynamic_compiles", budget.max_dynamic_compiles},
                {"max_child_depth", budget.max_child_depth},
                {"max_total_children", budget.max_total_children}};
}

RunBudget parse_budget(const json& value, std::string_view name) {
    if (!value.is_object()) throw std::invalid_argument(std::string(name) + " must be an object");
    detail::reject_unknown_fields(
        value, name,
        {"wall_time_ms", "model_tokens", "monetary_microunits", "max_concurrency",
         "max_program_operations", "max_core_steps", "max_dynamic_compiles", "max_child_depth",
         "max_total_children"});
    return RunBudget{
        require_uint64(value, "wall_time_ms"),           require_uint64(value, "model_tokens"),
        require_uint64(value, "monetary_microunits"),    require_uint32(value, "max_concurrency"),
        require_uint64(value, "max_program_operations"), require_uint64(value, "max_core_steps"),
        require_uint64(value, "max_dynamic_compiles"),   require_uint32(value, "max_child_depth"),
        require_uint64(value, "max_total_children")};
}

bool budget_is_empty(const RunBudget& budget) noexcept {
    return budget.wall_time_ms == 0 && budget.model_tokens == 0 &&
           budget.monetary_microunits == 0 && budget.max_concurrency == 0 &&
           budget.max_program_operations == 0 && budget.max_core_steps == 0 &&
           budget.max_dynamic_compiles == 0 && budget.max_child_depth == 0 &&
           budget.max_total_children == 0;
}

bool budget_at_most(const RunBudget& value, const RunBudget& limit) noexcept {
    return value.wall_time_ms <= limit.wall_time_ms && value.model_tokens <= limit.model_tokens &&
           value.monetary_microunits <= limit.monetary_microunits &&
           value.max_concurrency <= limit.max_concurrency &&
           value.max_program_operations <= limit.max_program_operations &&
           value.max_core_steps <= limit.max_core_steps &&
           value.max_dynamic_compiles <= limit.max_dynamic_compiles &&
           value.max_child_depth <= limit.max_child_depth &&
           value.max_total_children <= limit.max_total_children;
}

json encode_checkpoint(const CoreCheckpointIdentity& checkpoint) {
    return json{{"core_name", checkpoint.core_name},
                {"core_generation_id", checkpoint.core_generation_id},
                {"core_thread_id", checkpoint.core_thread_id},
                {"checkpoint_id", checkpoint.checkpoint_id},
                {"checkpoint_schema_version", checkpoint.checkpoint_schema_version}};
}

CoreCheckpointIdentity parse_checkpoint(const json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("Program journal core_checkpoint must be an object or null");
    }
    detail::reject_unknown_fields(value, "Program journal core_checkpoint",
                                  {"core_name", "core_generation_id", "core_thread_id",
                                   "checkpoint_id", "checkpoint_schema_version"});
    return CoreCheckpointIdentity{
        require_string(value, "core_name"), require_string(value, "core_generation_id"),
        require_string(value, "core_thread_id"), require_string(value, "checkpoint_id"),
        require_uint32(value, "checkpoint_schema_version")};
}

void validate_checkpoint(const CoreCheckpointIdentity& checkpoint) {
    require_token(checkpoint.core_name, "Program journal checkpoint core_name");
    require_sha256(checkpoint.core_generation_id, "Program journal checkpoint core_generation_id");
    require_token(checkpoint.core_thread_id, "Program journal checkpoint core_thread_id");
    require_token(checkpoint.checkpoint_id, "Program journal checkpoint checkpoint_id");
    if (checkpoint.checkpoint_schema_version == 0) {
        throw std::invalid_argument("Program journal checkpoint schema version must be positive");
    }
}

bool same_checkpoint(const CoreCheckpointIdentity& lhs,
                     const CoreCheckpointIdentity& rhs) noexcept {
    return lhs.core_name == rhs.core_name && lhs.core_generation_id == rhs.core_generation_id &&
           lhs.core_thread_id == rhs.core_thread_id && lhs.checkpoint_id == rhs.checkpoint_id &&
           lhs.checkpoint_schema_version == rhs.checkpoint_schema_version;
}

bool same_core(const CoreCheckpointIdentity& lhs, const CoreCheckpointIdentity& rhs) noexcept {
    return lhs.core_name == rhs.core_name && lhs.core_generation_id == rhs.core_generation_id &&
           lhs.core_thread_id == rhs.core_thread_id &&
           lhs.checkpoint_schema_version == rhs.checkpoint_schema_version;
}

json encode_continuation(const ProgramContinuation& continuation) {
    return json{{"operation_id", continuation.operation_id},
                {"state", std::string(to_string(continuation.state))},
                {"attempt", continuation.attempt}};
}

ProgramContinuation parse_continuation(const json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("Program journal continuation must be an object");
    }
    detail::reject_unknown_fields(value, "Program journal continuation",
                                  {"operation_id", "state", "attempt"});
    return ProgramContinuation{require_string(value, "operation_id"),
                               continuation_state_from_string(require_string(value, "state")),
                               require_uint64(value, "attempt")};
}

bool checkpoint_required(const ProgramJournalRecord& record) noexcept {
    if (record.continuation.state != ContinuationState::Running) return true;
    return record.sequence > 1;
}

bool is_known_state(ContinuationState state) noexcept {
    switch (state) {
        case ContinuationState::Running:
        case ContinuationState::Interrupted:
        case ContinuationState::Completed:
        case ContinuationState::Cancelled:
        case ContinuationState::BudgetExhausted:
        case ContinuationState::TimedOut:
        case ContinuationState::Failed:
        case ContinuationState::AmbiguousEffect:
        case ContinuationState::CheckpointIncompatible:
            return true;
    }
    return false;
}

void validate_record_body(const ProgramJournalRecord& record) {
    require_token(record.run_id, "Program journal run_id");
    require_sha256(record.program_version_id, "Program journal program_version_id");
    require_sha256(record.bundle_id, "Program journal bundle_id");
    if (!is_known_state(record.continuation.state)) {
        throw std::invalid_argument("Program journal continuation state is invalid");
    }
    if (record.sequence == 0) {
        throw std::invalid_argument("Program journal sequence must be positive");
    }
    if (record.sequence == 1) {
        if (!record.previous_id.empty()) {
            throw std::invalid_argument(
                "First Program journal record must have an empty previous_id");
        }
        if (record.continuation.state != ContinuationState::Running) {
            throw std::invalid_argument(
                "First Program journal record must start Running");
        }
    } else {
        require_sha256(record.previous_id, "Program journal previous_id");
    }
    if (record.continuation.operation_id != "root") {
        throw std::invalid_argument("Program journal operation_id must be 'root'");
    }
    if (record.continuation.attempt == 0) {
        throw std::invalid_argument("Program journal attempt must be positive");
    }
    if (!budget_at_most(record.inflight_reservation, record.remaining_budget)) {
        throw std::invalid_argument("Program journal reservation exceeds remaining budget");
    }
    if (record.continuation.state != ContinuationState::Running &&
        !budget_is_empty(record.inflight_reservation)) {
        throw std::invalid_argument(
            "Program journal non-running records must have an empty reservation");
    }
    if (record.timestamp_ms < 0) {
        throw std::invalid_argument("Program journal timestamp_ms must be non-negative");
    }
    if (record.core_checkpoint) validate_checkpoint(*record.core_checkpoint);
    if (checkpoint_required(record) && !record.core_checkpoint) {
        throw std::invalid_argument("Program journal state requires an exact Core checkpoint");
    }
}

json record_body(const ProgramJournalRecord& record) {
    json value                      = json::object();
    value["format"]                 = std::string(JOURNAL_FORMAT);
    value["storage_schema_version"] = ProgramJournalRecord::STORAGE_SCHEMA_VERSION;
    value["previous_id"]            = record.previous_id;
    value["run_id"]                 = record.run_id;
    value["program_version_id"]     = record.program_version_id;
    value["bundle_id"]              = record.bundle_id;
    value["sequence"]               = record.sequence;
    value["continuation"]           = encode_continuation(record.continuation);
    value["remaining_budget"]       = encode_budget(record.remaining_budget);
    value["inflight_reservation"]   = encode_budget(record.inflight_reservation);
    value["core_checkpoint"] =
        record.core_checkpoint ? encode_checkpoint(*record.core_checkpoint) : json(nullptr);
    value["timestamp_ms"] = record.timestamp_ms;
    return value;
}

std::string computed_record_id(const ProgramJournalRecord& record) {
    return detail::sha256_identity("program-journal-record/v1",
                                   detail::canonical_json_bytes(record_body(record)));
}

void validate_sealed_record(const ProgramJournalRecord& record) {
    validate_record_body(record);
    require_sha256(record.id, "Program journal id");
    if (record.id != computed_record_id(record)) {
        throw std::invalid_argument("Program journal id does not match its canonical body");
    }
}

bool valid_transition_impl(const ProgramJournalRecord& previous,
                           const ProgramJournalRecord& next) noexcept {
    if (previous.sequence == std::numeric_limits<std::uint64_t>::max() ||
        next.previous_id != previous.id || next.sequence != previous.sequence + 1 ||
        next.run_id != previous.run_id || next.program_version_id != previous.program_version_id ||
        next.bundle_id != previous.bundle_id ||
        next.continuation.operation_id != previous.continuation.operation_id ||
        !budget_at_most(next.remaining_budget, previous.remaining_budget)) {
        return false;
    }

    if (previous.core_checkpoint && !next.core_checkpoint) return false;
    if (previous.core_checkpoint && next.core_checkpoint &&
        !same_core(*previous.core_checkpoint, *next.core_checkpoint)) {
        return false;
    }

    if (previous.continuation.state == ContinuationState::Running) {
        return next.continuation.state != ContinuationState::Running &&
               next.continuation.attempt == previous.continuation.attempt;
    }
    if (previous.continuation.state == ContinuationState::Interrupted) {
        if (next.continuation.state == ContinuationState::Failed ||
            next.continuation.state == ContinuationState::Cancelled) {
            return next.continuation.attempt == previous.continuation.attempt &&
                   previous.core_checkpoint && next.core_checkpoint &&
                   same_checkpoint(*previous.core_checkpoint, *next.core_checkpoint);
        }
        if (next.continuation.state != ContinuationState::Running ||
            previous.continuation.attempt == std::numeric_limits<std::uint64_t>::max() ||
            next.continuation.attempt != previous.continuation.attempt + 1 ||
            !previous.core_checkpoint || !next.core_checkpoint) {
            return false;
        }
        return same_checkpoint(*previous.core_checkpoint, *next.core_checkpoint);
    }
    if (previous.continuation.state == ContinuationState::AmbiguousEffect) {
        if (!previous.core_checkpoint || !next.core_checkpoint ||
            !same_checkpoint(*previous.core_checkpoint, *next.core_checkpoint)) {
            return false;
        }
        if (next.continuation.state == ContinuationState::AmbiguousEffect ||
            next.continuation.state == ContinuationState::Failed) {
            return next.continuation.attempt == previous.continuation.attempt;
        }
        return next.continuation.state == ContinuationState::Running &&
               previous.continuation.attempt !=
                   std::numeric_limits<std::uint64_t>::max() &&
               next.continuation.attempt == previous.continuation.attempt + 1;
    }
    if (next.continuation.state != ContinuationState::Failed ||
        next.continuation.attempt != previous.continuation.attempt ||
        previous.continuation.state == ContinuationState::Failed) {
        return false;
    }
    if (previous.core_checkpoint.has_value() != next.core_checkpoint.has_value()) {
        return false;
    }
    return !previous.core_checkpoint ||
           same_checkpoint(*previous.core_checkpoint, *next.core_checkpoint);
}

}  // namespace

std::string_view to_string(ContinuationState state) noexcept {
    switch (state) {
        case ContinuationState::Running:
            return "running";
        case ContinuationState::Interrupted:
            return "interrupted";
        case ContinuationState::Completed:
            return "completed";
        case ContinuationState::Cancelled:
            return "cancelled";
        case ContinuationState::BudgetExhausted:
            return "budget_exhausted";
        case ContinuationState::TimedOut:
            return "timed_out";
        case ContinuationState::Failed:
            return "failed";
        case ContinuationState::AmbiguousEffect:
            return "ambiguous_effect";
        case ContinuationState::CheckpointIncompatible:
            return "checkpoint_incompatible";
    }
    return "unknown";
}

ContinuationState continuation_state_from_string(std::string_view value) {
    if (value == "running") return ContinuationState::Running;
    if (value == "interrupted") return ContinuationState::Interrupted;
    if (value == "completed") return ContinuationState::Completed;
    if (value == "cancelled") return ContinuationState::Cancelled;
    if (value == "budget_exhausted") return ContinuationState::BudgetExhausted;
    if (value == "timed_out") return ContinuationState::TimedOut;
    if (value == "failed") return ContinuationState::Failed;
    if (value == "ambiguous_effect") return ContinuationState::AmbiguousEffect;
    if (value == "checkpoint_incompatible") return ContinuationState::CheckpointIncompatible;
    throw std::invalid_argument("Unknown Program continuation state: " + std::string(value));
}

bool is_valid_program_journal_transition(const ProgramJournalRecord& previous,
                                         const ProgramJournalRecord& next) noexcept {
    return valid_transition_impl(previous, next);
}

ProgramJournalRecord ProgramJournalRecord::create(ProgramJournalRecordData data) {
    ProgramJournalRecord record;
    record.previous_id          = std::move(data.previous_id);
    record.run_id               = std::move(data.run_id);
    record.program_version_id   = std::move(data.program_version_id);
    record.bundle_id            = std::move(data.bundle_id);
    record.sequence             = data.sequence;
    record.continuation         = std::move(data.continuation);
    record.remaining_budget     = data.remaining_budget;
    record.inflight_reservation = data.inflight_reservation;
    record.core_checkpoint      = std::move(data.core_checkpoint);
    record.timestamp_ms         = data.timestamp_ms;
    validate_record_body(record);
    record.id = computed_record_id(record);
    return record;
}

ProgramJournalRecord ProgramJournalRecord::parse(std::string_view stored_bytes) {
    json value;
    try {
        value = detail::parse_json_strict(stored_bytes);
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string("Invalid stored ProgramJournalRecord JSON: ") +
                                    error.what());
    }
    if (!value.is_object() || require_string(value, "format") != JOURNAL_FORMAT) {
        throw std::invalid_argument("Stored ProgramJournalRecord has unknown format");
    }
    detail::reject_unknown_fields(
        value, "Stored ProgramJournalRecord",
        {"format", "storage_schema_version", "id", "previous_id", "run_id", "program_version_id",
         "bundle_id", "sequence", "continuation", "remaining_budget", "inflight_reservation",
         "core_checkpoint", "timestamp_ms"});
    if (require_uint32(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored ProgramJournalRecord schema version is unsupported");
    }

    const auto&                           checkpoint = require_value(value, "core_checkpoint");
    std::optional<CoreCheckpointIdentity> parsed_checkpoint;
    if (!checkpoint.is_null()) parsed_checkpoint = parse_checkpoint(checkpoint);

    auto record = create(ProgramJournalRecordData{
        require_string(value, "previous_id"), require_string(value, "run_id"),
        require_string(value, "program_version_id"), require_string(value, "bundle_id"),
        require_uint64(value, "sequence"), parse_continuation(require_value(value, "continuation")),
        parse_budget(require_value(value, "remaining_budget"), "Program journal remaining_budget"),
        parse_budget(require_value(value, "inflight_reservation"),
                     "Program journal inflight_reservation"),
        std::move(parsed_checkpoint), require_int64(value, "timestamp_ms")});
    if (record.id != require_string(value, "id")) {
        throw std::invalid_argument("Stored ProgramJournalRecord id does not match its body");
    }
    return record;
}

std::string ProgramJournalRecord::serialize_canonical() const {
    validate_sealed_record(*this);
    auto value  = record_body(*this);
    value["id"] = id;
    return detail::canonical_json_bytes(value);
}

struct InMemoryProgramJournal::Impl {
    struct StoredRecord {
        ProgramJournalRecord record;
        std::string          canonical_bytes;
    };

    mutable std::mutex                            mutex;
    std::unordered_map<std::string, StoredRecord> records_by_id;
    std::unordered_map<std::string, std::string>  latest_id_by_run;
};

InMemoryProgramJournal::InMemoryProgramJournal() : impl_(std::make_unique<Impl>()) {}
InMemoryProgramJournal::~InMemoryProgramJournal()                                 = default;
InMemoryProgramJournal::InMemoryProgramJournal(InMemoryProgramJournal&&) noexcept = default;
InMemoryProgramJournal& InMemoryProgramJournal::operator=(InMemoryProgramJournal&&) noexcept =
    default;

std::optional<ProgramJournalRecord> InMemoryProgramJournal::latest(std::string_view run_id) const {
    require_token(run_id, "Program journal run_id");
    const std::lock_guard lock(impl_->mutex);
    const auto            latest = impl_->latest_id_by_run.find(std::string(run_id));
    if (latest == impl_->latest_id_by_run.end()) return std::nullopt;
    return impl_->records_by_id.at(latest->second).record;
}

JournalAppendResult InMemoryProgramJournal::compare_append(std::string_view expected_previous_id,
                                                           ProgramJournalRecord record) {
    std::string canonical_bytes;
    try {
        if (!expected_previous_id.empty() && !detail::is_sha256_identity(expected_previous_id)) {
            return JournalAppendResult::Conflict;
        }
        canonical_bytes = record.serialize_canonical();
    } catch (const std::invalid_argument&) {
        return JournalAppendResult::Conflict;
    }

    const std::lock_guard lock(impl_->mutex);
    if (const auto existing = impl_->records_by_id.find(record.id);
        existing != impl_->records_by_id.end()) {
        if (expected_previous_id != record.previous_id) return JournalAppendResult::Conflict;
        const auto latest = impl_->latest_id_by_run.find(record.run_id);
        if (latest == impl_->latest_id_by_run.end() || latest->second != record.id) {
            return JournalAppendResult::Conflict;
        }
        return existing->second.canonical_bytes == canonical_bytes
                   ? JournalAppendResult::AlreadyPresent
                   : JournalAppendResult::Conflict;
    }

    const auto latest = impl_->latest_id_by_run.find(record.run_id);
    if (latest == impl_->latest_id_by_run.end()) {
        if (!expected_previous_id.empty() || !record.previous_id.empty() || record.sequence != 1) {
            return JournalAppendResult::Conflict;
        }
    } else {
        if (latest->second != expected_previous_id || record.previous_id != expected_previous_id) {
            return JournalAppendResult::Conflict;
        }
        const auto& previous = impl_->records_by_id.at(latest->second).record;
        if (!is_valid_program_journal_transition(previous, record)) {
            return JournalAppendResult::Conflict;
        }
    }

    const auto id                    = record.id;
    const auto run_id                = record.run_id;
    auto [inserted_record, inserted] = impl_->records_by_id.emplace(
        id, Impl::StoredRecord{std::move(record), std::move(canonical_bytes)});
    if (!inserted) return JournalAppendResult::Conflict;
    try {
        if (latest == impl_->latest_id_by_run.end()) {
            impl_->latest_id_by_run.emplace(run_id, id);
        } else {
            latest->second = id;
        }
    } catch (...) {
        impl_->records_by_id.erase(inserted_record);
        throw;
    }
    return JournalAppendResult::Appended;
}

}  // namespace neograph::program
