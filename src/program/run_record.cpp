#include <neograph/program/run_record.h>
#include <neograph/program/child_relation.h>

#include "canonical_json.h"
#include "../core/json_builder.h"
#include "node_retry_budget.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
namespace neograph::program {
namespace {
constexpr std::string_view FORMAT = "neograph-program-run-record";
std::string                rs(const json& v, std::string_view key) {
    std::string k(key);
    if (!v.contains(k) || !v[k].is_string())
        throw std::invalid_argument("Program run record field '" + k + "' must be a string");
    return v[k].get<std::string>();
}
std::uint64_t ru(const json& v, std::string_view key) {
    std::string k(key);
    if (!v.contains(k) || !v[k].is_number_unsigned())
        throw std::invalid_argument("Program run record field '" + k + "' must be unsigned");
    return v[k].get<std::uint64_t>();
}
std::uint32_t r32(const json& v, std::string_view key) {
    auto n = ru(v, key);
    if (n > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("Program run record integer exceeds uint32 range");
    return static_cast<std::uint32_t>(n);
}
std::int64_t ri(const json& v, std::string_view key) {
    std::string k(key);
    if (!v.contains(k) || !v[k].is_number_integer())
        throw std::invalid_argument("Program run record field '" + k + "' must be an integer");
    if (v[k].is_number_unsigned()) {
        auto n = v[k].get<std::uint64_t>();
        if (n > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::invalid_argument("Program run record integer exceeds int64 range");
        return static_cast<std::int64_t>(n);
    }
    return v[k].get<std::int64_t>();
}
json rv(const json& v, std::string_view key) {
    std::string k(key);
    if (!v.contains(k))
        throw std::invalid_argument("Program run record requires field '" + k + "'");
    return v[k];
}
// Run record bodies are built in one document (#478). Each make() takes the
// argument the json initializer list took, so the members keep their types.
using Builder = ::neograph::detail::JsonBuilder;
using BV      = Builder::Value;
BV eb(Builder& j, const RunBudget& b) {
    auto* encoded = j.object({{"wall_time_ms", j.make(b.wall_time_ms)},
                              {"model_tokens", j.make(b.model_tokens)},
                              {"monetary_microunits", j.make(b.monetary_microunits)},
                              {"max_concurrency", j.make(b.max_concurrency)},
                              {"max_program_operations", j.make(b.max_program_operations)},
                              {"max_core_steps", j.make(b.max_core_steps)},
                              {"max_dynamic_compiles", j.make(b.max_dynamic_compiles)},
                              {"max_child_depth", j.make(b.max_child_depth)},
                              {"max_total_children", j.make(b.max_total_children)}});
    detail::encode_node_retries(j, encoded, b);
    return encoded;
}
RunBudget db(const json& v) {
    if (!v.is_object()) throw std::invalid_argument("Program run budget must be an object");
    detail::reject_unknown_fields(
        v, "Program run budget",
        {"wall_time_ms", "model_tokens", "monetary_microunits", "max_concurrency",
         "max_program_operations", "max_core_steps", "max_dynamic_compiles", "max_child_depth",
         "max_total_children", "max_node_retries"});
    RunBudget budget{ru(v, "wall_time_ms"),           ru(v, "model_tokens"),
                     ru(v, "monetary_microunits"),    r32(v, "max_concurrency"),
                     ru(v, "max_program_operations"), ru(v, "max_core_steps"),
                     ru(v, "max_dynamic_compiles"),   r32(v, "max_child_depth"),
                     ru(v, "max_total_children")};
    budget.max_node_retries = detail::decode_node_retries(v);
    return budget;
}
BV invocation_body(Builder& j, const ProgramPersistedInvocation& invocation) {
    auto* value = j.object({{"input", j.make(invocation.input)},
                            {"granted_budget", eb(j, invocation.granted_budget)},
                            {"trace_id", j.make(invocation.trace_id)}});
    if (!invocation.parent_run_id.empty())
        j.set(value, "parent_run_id", j.make(invocation.parent_run_id));
    if (invocation.child_depth != 0) j.set(value, "child_depth", j.make(invocation.child_depth));
    return value;
}
json invocation_body(const ProgramPersistedInvocation& invocation) {
    Builder j;
    auto*   root = invocation_body(j, invocation);
    return std::move(j).finish(root);
}

ProgramPersistedInvocation parse_invocation(const json& value, std::string_view label) {
    if (!value.is_object())
        throw std::invalid_argument(std::string(label) + " must be an object");
    detail::reject_unknown_fields(value,
                                  label,
                                  {"input", "granted_budget", "trace_id", "parent_run_id",
                                   "child_depth"});
    ProgramPersistedInvocation invocation{
        rv(value, "input"), db(rv(value, "granted_budget")), rs(value, "trace_id")};
    if (value.contains("parent_run_id"))
        invocation.parent_run_id = rs(value, "parent_run_id");
    if (value.contains("child_depth"))
        invocation.child_depth = r32(value, "child_depth");
    return invocation;
}

std::string_view child_state_string(ProgramChildState state) noexcept {
    switch (state) {
        case ProgramChildState::Publishing:
            return "publishing";
        case ProgramChildState::Dispatched:
            return "dispatched";
        case ProgramChildState::Completed:
            return "completed";
        case ProgramChildState::Cancelled:
            return "cancelled";
        case ProgramChildState::Failed:
            return "failed";
    }
    return "unknown";
}

ProgramChildState child_state_from_string(std::string_view value) {
    if (value == "publishing") return ProgramChildState::Publishing;
    if (value == "dispatched") return ProgramChildState::Dispatched;
    if (value == "completed") return ProgramChildState::Completed;
    if (value == "cancelled") return ProgramChildState::Cancelled;
    if (value == "failed") return ProgramChildState::Failed;
    throw std::invalid_argument("Unknown Program child state: " + std::string(value));
}

BV child_body(Builder& j, const ProgramChildRecord& child) {
    auto* value = j.object({{"child_run_id", j.make(child.child_run_id)},
                            {"link_id", j.make(child.link_id)},
                            {"link_receipt", j.make(child.link_receipt)},
                            {"invocation", invocation_body(j, child.invocation)},
                            {"state", j.make(std::string(child_state_string(child.state)))}});
    if (child.terminal_generation)
        j.set(value, "terminal_generation", j.make(child.terminal_generation));
    if (child.terminal_result)
        j.set(value, "terminal_result",
              j.make(detail::parse_json_strict(child.terminal_result->serialize_canonical())));
    return value;
}
json child_body(const ProgramChildRecord& child) {
    Builder j;
    auto*   root = child_body(j, child);
    return std::move(j).finish(root);
}

ProgramChildRecord parse_child(const json& value) {
    if (!value.is_object())
        throw std::invalid_argument("Program child record must be an object");
    detail::reject_unknown_fields(value, "Program child record",
                                  {"child_run_id", "link_id", "link_receipt", "invocation", "state",
                                   "terminal_result", "terminal_generation"});
    ProgramChildRecord result{rs(value, "child_run_id"),
                              rs(value, "link_id"),
                              rs(value, "link_receipt"),
                              parse_invocation(rv(value, "invocation"), "Program child invocation"),
                              child_state_from_string(rs(value, "state"))};
    if (value.contains("terminal_generation"))
        result.terminal_generation = ru(value, "terminal_generation");
    if (value.contains("terminal_result") && !value.at("terminal_result").is_null())
        result.terminal_result = ProgramResult::parse(
            detail::canonical_json_bytes(value.at("terminal_result")));
    return result;
}
BV ec(Builder& j, const CoreCheckpointIdentity& c) {
    return j.object({{"core_name", j.make(c.core_name)},
                     {"core_generation_id", j.make(c.core_generation_id)},
                     {"core_thread_id", j.make(c.core_thread_id)},
                     {"checkpoint_id", j.make(c.checkpoint_id)},
                     {"checkpoint_schema_version", j.make(c.checkpoint_schema_version)}});
}
CoreCheckpointIdentity dc(const json& v) {
    if (!v.is_object()) throw std::invalid_argument("Program run checkpoint must be an object");
    detail::reject_unknown_fields(v, "Program run checkpoint",
                                  {"core_name", "core_generation_id", "core_thread_id",
                                   "checkpoint_id", "checkpoint_schema_version"});
    return {rs(v, "core_name"), rs(v, "core_generation_id"), rs(v, "core_thread_id"),
            rs(v, "checkpoint_id"), r32(v, "checkpoint_schema_version")};
}
json nested(std::string bytes) {
    return detail::parse_json_strict(bytes);
}
template <class T>
std::optional<T> opt(const json& v) {
    if (v.is_null()) return std::nullopt;
    return T::parse(detail::canonical_json_bytes(v));
}
std::optional<std::string> os(const json& v) {
    if (v.is_null()) return std::nullopt;
    if (!v.is_string())
        throw std::invalid_argument("Program fork lineage must be a string or null");
    return v.get<std::string>();
}
bool terminal(ContinuationState s) {
    return s != ContinuationState::Running && s != ContinuationState::Interrupted &&
           s != ContinuationState::AmbiguousEffect;
}
void validate_continuation_state(const ProgramRunRecordData& d) {
    const auto state = d.continuation.state;
    if (state == ContinuationState::Interrupted) {
        const bool awaiting_input =
            d.pending_input && d.pending_input->state() == ProgramPendingState::Awaiting;
        const bool awaiting_effect =
            d.pending_effect && d.pending_effect->state() == ProgramPendingState::Awaiting;
        if (awaiting_input == awaiting_effect || !d.terminal_result ||
            d.terminal_result->status() != ProgramTerminalStatus::Interrupted) {
            throw std::invalid_argument(
                "Interrupted Program run requires one awaiting value and result");
        }
        return;
    }
    if (state == ContinuationState::AmbiguousEffect) {
        if (d.pending_input || !d.pending_effect ||
            d.pending_effect->state() != ProgramPendingState::Ambiguous || !d.terminal_result ||
            d.terminal_result->status() != ProgramTerminalStatus::AmbiguousEffect) {
            throw std::invalid_argument(
                "Ambiguous Program run requires one ambiguous effect and result");
        }
        return;
    }
    if (state == ContinuationState::Running) {
        if (d.terminal_result) {
            throw std::invalid_argument("Running Program run cannot contain a terminal result");
        }
        if ((d.pending_input && d.pending_input->state() != ProgramPendingState::Consumed) ||
            (d.pending_effect && d.pending_effect->state() != ProgramPendingState::Consumed)) {
            throw std::invalid_argument(
                "Running Program run may retain only consumed pending values");
        }
    }
}
void validate_result_state(const ProgramRunRecordData& d) {
    if (!d.terminal_result) return;
    ContinuationState expected = ContinuationState::Failed;
    switch (d.terminal_result->status()) {
        case ProgramTerminalStatus::Completed:
            expected = ContinuationState::Completed;
            break;
        case ProgramTerminalStatus::Interrupted:
            expected = ContinuationState::Interrupted;
            break;
        case ProgramTerminalStatus::Cancelled:
            expected = ContinuationState::Cancelled;
            break;
        case ProgramTerminalStatus::BudgetExhausted:
            expected = ContinuationState::BudgetExhausted;
            break;
        case ProgramTerminalStatus::TimedOut:
            expected = ContinuationState::TimedOut;
            break;
        case ProgramTerminalStatus::Failed:
            expected = ContinuationState::Failed;
            break;
        case ProgramTerminalStatus::AmbiguousEffect:
            expected = ContinuationState::AmbiguousEffect;
            break;
        case ProgramTerminalStatus::CheckpointIncompatible:
            expected = ContinuationState::CheckpointIncompatible;
    }
    if (d.continuation.state != expected) {
        throw std::invalid_argument("Program result status does not match continuation state");
    }
}

// What a child record must satisfy by itself, wherever it is stored: a run record's child list or a relation row.
void validate_child_shape(const ProgramChildRecord& child) {
    detail::validate_token(child.child_run_id, "Program child run_id");
    if (!detail::is_sha256_identity(child.link_id))
        throw std::invalid_argument("Program run child link identity must be a sha256 value");
    if (child.link_receipt.empty())
        throw std::invalid_argument("Program child link receipt must not be empty");
    detail::validate_utf8(child.link_receipt);
    if ((child.state == ProgramChildState::Publishing && child.terminal_result) ||
        ((child.state == ProgramChildState::Completed ||
          child.state == ProgramChildState::Failed) &&
         !child.terminal_result)) {
        throw std::invalid_argument(
            "Program child terminal result does not match its lifecycle state");
    }
    if (child.terminal_generation && !child.terminal_result)
        throw std::invalid_argument("Child generation requires a result");
    if (child.terminal_result) {
        const auto& result = *child.terminal_result;
        const bool failed_status =
            result.status() == ProgramTerminalStatus::Failed ||
            result.status() == ProgramTerminalStatus::BudgetExhausted ||
            result.status() == ProgramTerminalStatus::TimedOut ||
            result.status() == ProgramTerminalStatus::CheckpointIncompatible;
        if ((result.run_id() != child.child_run_id && child.terminal_generation < 2) ||
            (result.run_id() == child.child_run_id && child.terminal_generation != 0) ||
            result.program_version_id().empty() || result.attempt() == 0 ||
            (child.state == ProgramChildState::Completed &&
             result.status() != ProgramTerminalStatus::Completed) ||
            (child.state == ProgramChildState::Cancelled &&
             result.status() != ProgramTerminalStatus::Cancelled) ||
            (child.state == ProgramChildState::Failed && !failed_status) ||
            (child.state == ProgramChildState::Dispatched &&
             result.status() != ProgramTerminalStatus::Interrupted &&
             result.status() != ProgramTerminalStatus::AmbiguousEffect)) {
            throw std::invalid_argument("Program child terminal result does not bind its state");
        }
    }
    (void)invocation_body(child.invocation);
}

void validate(const ProgramRunRecordData& d) {
    // Nested values are validated by stored_body(); keep this pass structural
    // so serialization does not repeat their canonical work.
    if (d.owner_scope.empty()) {
        throw std::invalid_argument("Program run owner_scope must not be empty");
    }
    detail::validate_utf8(d.owner_scope);
    detail::validate_token(d.run_id, "Program run run_id");
    if (!detail::is_sha256_identity(d.program_version_id) ||
        !detail::is_sha256_identity(d.bundle_id) ||
        !detail::is_sha256_identity(d.binding_fingerprint) ||
        !detail::is_sha256_identity(d.journal_head)) {
        throw std::invalid_argument("Program run identities must be sha256 identities");
    }
    if (d.exact_checkpoint_content_id &&
        (!d.exact_checkpoint ||
         !detail::is_sha256_identity(*d.exact_checkpoint_content_id))) {
        throw std::invalid_argument(
            "Program run checkpoint content identity requires an exact checkpoint");
    }
    detail::validate_token(d.continuation.operation_id, "Program run operation_id");
    if (!d.continuation.attempt) {
        throw std::invalid_argument("Program run attempt must be positive");
    }
    if (!d.continuation.fence) {
        throw std::invalid_argument("Program run fence must be positive");
    }
    d.invocation.validate();
    if (d.invocation.owner_scope != d.owner_scope || d.invocation.run_id != d.run_id ||
        d.invocation.program_version_id != d.program_version_id) {
        throw std::invalid_argument("Program run invocation does not bind run identity");
    }
    if (!d.invocation.parent_run_id.empty()) {
        if (d.invocation.parent_run_id == d.run_id || d.child_depth == 0) {
            throw std::invalid_argument("Child Program run has invalid parent topology");
        }
    } else if (d.child_depth != 0) {
        throw std::invalid_argument("Root Program run cannot record a child depth");
    }
    if (d.pending_input && d.pending_effect) {
        throw std::invalid_argument("Program run cannot await input and effect simultaneously");
    }
    if (d.created_at_ms < 0 || d.updated_at_ms < d.created_at_ms) {
        throw std::invalid_argument("Program run timestamps are invalid");
    }
    if (d.terminal_result) {
        const auto& result = *d.terminal_result;
        if (result.run_id() != d.run_id || result.program_version_id() != d.program_version_id ||
            result.bundle_id() != d.bundle_id ||
            result.operation_id() != d.continuation.operation_id ||
            result.attempt() != d.continuation.attempt ||
            result.checkpoint() != d.exact_checkpoint) {
            throw std::invalid_argument("Program terminal result does not bind the run snapshot");
        }
    } else if (terminal(d.continuation.state)) {
        throw std::invalid_argument("Terminal run snapshot requires ProgramResult");
    }
    const bool has_fork_lineage =
        d.fork_source_run_id || d.fork_source_program_version_id || d.fork_source_checkpoint_id;
    if (static_cast<bool>(d.fork_receipt) != has_fork_lineage) {
        throw std::invalid_argument("Program fork lineage must be complete");
    }
    if (d.fork_receipt) {
        const auto& receipt = *d.fork_receipt;
        if (!receipt.compatible() || receipt.owner_scope() != d.owner_scope ||
            receipt.target_program_version_id() != d.program_version_id ||
            receipt.source_run_id() != *d.fork_source_run_id ||
            receipt.source_program_version_id() != *d.fork_source_program_version_id ||
            receipt.source_checkpoint_id() != *d.fork_source_checkpoint_id) {
            throw std::invalid_argument("Program fork receipt does not bind target lineage");
        }
        if (receipt.storage_schema_version() >= ForkCompatibilityReceipt::STORAGE_SCHEMA_VERSION &&
            !receipt.initial_resume_binding()) {
            throw std::invalid_argument("Program fork receipt has no initial resume binding");
        }
    }
    if (!d.logical_run_id.empty()) detail::validate_token(d.logical_run_id, "Program logical run");
    if (d.attempt_resume &&
        (d.continuation.state != ContinuationState::Running || !d.exact_checkpoint ||
         d.attempt_resume->checkpoint_id != d.exact_checkpoint->checkpoint_id)) {
        throw std::invalid_argument(
            "Program attempt resume value must bind the running attempt's exact checkpoint");
    }
    if (d.settled_branches &&
        (!d.settled_branches->is_object() || d.settled_branches->empty() ||
         !d.exact_checkpoint ||
         (d.continuation.state != ContinuationState::Running &&
          d.continuation.state != ContinuationState::Interrupted &&
          d.continuation.state != ContinuationState::AmbiguousEffect))) {
        throw std::invalid_argument(
            "Program settled branches belong to a paused or resumed run's exact checkpoint");
    }
    std::set<std::string, std::less<>> child_ids;
    for (const auto& child : d.children) {
        detail::validate_token(child.child_run_id, "Program child run_id");
        if (child.child_run_id == d.run_id || !child_ids.insert(child.child_run_id).second)
            throw std::invalid_argument("Program run child identities must be unique");
        if (child.invocation.parent_run_id !=
                (d.logical_run_id.empty() ? d.run_id : d.logical_run_id) ||
            d.child_depth == std::numeric_limits<std::uint32_t>::max() ||
            child.invocation.child_depth != d.child_depth + 1) {
            throw std::invalid_argument("Program child invocation does not bind its parent");
        }
        validate_child_shape(child);
    }
}
BV body(Builder& j, const ProgramRunRecordData& d, std::uint32_t schema_version) {
    auto* continuation = j.object({{"operation_id", j.make(d.continuation.operation_id)},
                                   {"state", j.make(std::string(to_string(d.continuation.state)))},
                                   {"attempt", j.make(d.continuation.attempt)}});
    if (schema_version >= 7) {
        j.set(continuation, "fence", j.make(d.continuation.fence));
    }
    auto* value = j.object(
        {{"format", j.make(std::string(FORMAT))},
         {"storage_schema_version", j.make(schema_version)},
         {"owner_scope", j.make(d.owner_scope)},
         {"run_id", j.make(d.run_id)},
         {"program_version_id", j.make(d.program_version_id)},
         {"bundle_id", j.make(d.bundle_id)},
         {"binding_fingerprint", j.make(d.binding_fingerprint)},
         {"invocation", j.make(nested(d.invocation.serialize_canonical()))},
         {"child_depth", j.make(d.child_depth)},
         {"continuation", continuation},
         {"remaining_budget", eb(j, d.remaining_budget)},
         {"exact_checkpoint", d.exact_checkpoint ? ec(j, *d.exact_checkpoint) : j.null()},
         {"pending_input",
          d.pending_input ? j.make(nested(d.pending_input->serialize_canonical())) : j.null()},
         {"pending_effect",
          d.pending_effect ? j.make(nested(d.pending_effect->serialize_canonical())) : j.null()},
         {"terminal_result",
          d.terminal_result ? j.make(nested(d.terminal_result->serialize_canonical())) : j.null()},
         {"fork_receipt",
          d.fork_receipt ? j.make(nested(d.fork_receipt->serialize_canonical())) : j.null()},
         {"fork_source_run_id",
          d.fork_source_run_id ? j.make(*d.fork_source_run_id) : j.null()},
         {"fork_source_program_version_id",
          d.fork_source_program_version_id ? j.make(*d.fork_source_program_version_id) : j.null()},
         {"fork_source_checkpoint_id",
          d.fork_source_checkpoint_id ? j.make(*d.fork_source_checkpoint_id) : j.null()},
         {"journal_head", j.make(d.journal_head)},
         {"event_sequence", j.make(d.event_sequence)},
         {"effect_sequence", j.make(d.effect_sequence)},
         {"created_at_ms", j.make(d.created_at_ms)},
         {"updated_at_ms", j.make(d.updated_at_ms)}});
    if (schema_version >= 3) {
        j.set(value, "exact_checkpoint_content_id",
              d.exact_checkpoint_content_id ? j.make(*d.exact_checkpoint_content_id) : j.null());
    }
    if (!d.logical_run_id.empty()) j.set(value, "logical_run_id", j.make(d.logical_run_id));
    if (d.attempt_resume) {
        j.set(value, "attempt_resume",
              j.object({{"checkpoint_id", j.make(d.attempt_resume->checkpoint_id)},
                        {"value", j.make(d.attempt_resume->value)}}));
    }
    if (d.settled_branches) j.set(value, "settled_branches", j.make(*d.settled_branches));
    if (!d.children.empty()) {
        auto* children = j.array();
        for (const auto& child : d.children) j.push(children, child_body(j, child));
        j.set(value, "children", children);
    }
    return value;
}
json stored_body(const ProgramRunRecordData& d, std::uint32_t schema_version) {
    Builder j;
    auto*   value = body(j, d, schema_version);
    j.set(value, "recorded_binding_set_fingerprint",
          d.recorded_binding_set_fingerprint ? j.make(*d.recorded_binding_set_fingerprint)
                                             : j.null());
    return std::move(j).finish(value);
}
}  // namespace
bool ProgramPersistedInvocation::operator==(const ProgramPersistedInvocation& other) const {
    return granted_budget == other.granted_budget && trace_id == other.trace_id &&
           parent_run_id == other.parent_run_id && child_depth == other.child_depth &&
           detail::canonical_json_bytes(input) == detail::canonical_json_bytes(other.input);
}
bool ProgramAttemptResume::operator==(const ProgramAttemptResume& other) const {
    return checkpoint_id == other.checkpoint_id &&
           detail::canonical_json_bytes(value) == detail::canonical_json_bytes(other.value);
}
bool ProgramChildRecord::operator==(const ProgramChildRecord& other) const {
    return detail::canonical_json_bytes(child_body(*this)) ==
           detail::canonical_json_bytes(child_body(other));
}
std::string_view to_string(ProgramChildState state) noexcept {
    return child_state_string(state);
}

ProgramChildState program_child_state_from_string(std::string_view value) {
    return child_state_from_string(value);
}
struct ProgramRunRecord::Impl {
    explicit Impl(ProgramRunRecordData value, std::uint32_t version)
        : data(std::move(value)), schema_version(version) {
        auto body_bytes = detail::canonical_json_bytes(stored_body(data, schema_version));
        id = detail::sha256_identity("program-run-record/v1", body_bytes);
        const auto format_position =
            body_bytes.find("\"format\":\"neograph-program-run-record\"");
        const auto position = format_position == std::string::npos
                                  ? std::string::npos
                                  : body_bytes.find(",\"invocation\":", format_position);
        if (position == std::string::npos) {
            throw std::invalid_argument("Program run record canonical body is malformed");
        }
        body_bytes.insert(position, ",\"id\":\"" + id + "\"");
        canonical_bytes = std::move(body_bytes);
    }

    ProgramRunRecordData data;
    std::uint32_t        schema_version;
    std::string          id;
    std::string          canonical_bytes;
};
ProgramRunRecord::ProgramRunRecord(std::shared_ptr<const Impl> p) : impl_(std::move(p)) {}

ProgramRunRecord ProgramRunRecord::with_hydrated_terminal(ProgramResult result) const {
    if (!impl_->data.terminal_result || impl_->data.terminal_result->id() != result.id())
        throw std::invalid_argument("Hydrated terminal result differs from its canonical record");
    auto record = *this;
    record.hydrated_terminal_ = std::move(result);
    return record;
}
ProgramRunRecord ProgramRunRecord::create(ProgramRunRecordData d) {
    if (d.logical_run_id == d.run_id) d.logical_run_id.clear();
    if (d.fork_receipt) {
        if (!d.fork_source_run_id) {
            d.fork_source_run_id = d.fork_receipt->source_run_id();
        }
        if (!d.fork_source_program_version_id) {
            d.fork_source_program_version_id = d.fork_receipt->source_program_version_id();
        }
        if (!d.fork_source_checkpoint_id) {
            d.fork_source_checkpoint_id = d.fork_receipt->source_checkpoint_id();
        }
    }
    validate(d);
    validate_continuation_state(d);
    validate_result_state(d);
    if (d.recorded_binding_set_fingerprint &&
        !detail::is_sha256_identity(*d.recorded_binding_set_fingerprint)) {
        throw std::invalid_argument("Recorded binding set fingerprint must be a sha256 identity");
    }
    // Each version is used only by the records that need it, so the ids of all other records stay unchanged: a record
    // keeps the lowest version that can hold it. Version 7 carries continuation.fence and is written only for a run
    // that was taken over (fence other than 1); a record without the field reads as fence 1. A newer version holds
    // everything an older one does, so a taken-over record takes version 7 whatever else it carries.
    constexpr std::uint32_t kSchemaSettledBranches = 6;
    constexpr std::uint32_t kSchemaAttemptResume   = 5;
    constexpr std::uint32_t kSchemaLogicalRun      = 4;
    constexpr std::uint32_t kSchemaBase            = 3;
    const auto schema =
        d.continuation.fence != 1 ? STORAGE_SCHEMA_VERSION
        : d.settled_branches      ? kSchemaSettledBranches
        : d.attempt_resume        ? kSchemaAttemptResume
        : !d.logical_run_id.empty() ||
                std::any_of(d.children.begin(), d.children.end(),
                            [](const auto& child) { return child.terminal_generation != 0; })
            ? kSchemaLogicalRun
            : kSchemaBase;
    return ProgramRunRecord(std::make_shared<const Impl>(std::move(d), schema));
}
ProgramRunRecord ProgramRunRecord::parse(std::string_view bytes) {
    json v;
    try {
        v = detail::parse_json_strict(bytes);
    } catch (const std::exception& e) {
        throw std::invalid_argument(std::string("Invalid stored ProgramRunRecord JSON: ") +
                                    e.what());
    }
    if (!v.is_object() || rs(v, "format") != FORMAT) {
        throw std::invalid_argument("Stored ProgramRunRecord has unknown format");
    }
    const auto schema_version = r32(v, "storage_schema_version");
    if (schema_version < 2 || schema_version > STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored ProgramRunRecord schema version is unsupported");
    }
    if (schema_version < 4 && v.contains("logical_run_id"))
        throw std::invalid_argument("Legacy run cannot carry logical identity");
    if (schema_version < 5 && v.contains("attempt_resume"))
        throw std::invalid_argument("Legacy run cannot carry an attempt resume value");
    if (schema_version < 6 && v.contains("settled_branches"))
        throw std::invalid_argument("Legacy run cannot carry settled branches");
    if (schema_version == 2) {
        detail::reject_unknown_fields(
            v, "Stored ProgramRunRecord",
            {"format", "storage_schema_version", "id", "owner_scope", "run_id",
             "program_version_id", "bundle_id", "binding_fingerprint", "invocation",
             "child_depth", "continuation", "remaining_budget", "exact_checkpoint",
             "pending_input", "pending_effect", "terminal_result", "fork_receipt",
             "fork_source_run_id", "fork_source_program_version_id",
             "fork_source_checkpoint_id", "recorded_binding_set_fingerprint",
             "journal_head", "event_sequence", "effect_sequence", "created_at_ms",
             "updated_at_ms", "children"});
    } else {
        detail::reject_unknown_fields(v, "Stored ProgramRunRecord",
                                      {"format",
                                       "storage_schema_version",
                                       "id",
                                       "owner_scope",
                                       "run_id",
                                       "program_version_id",
                                       "bundle_id",
                                       "binding_fingerprint",
                                       "invocation",
                                       "child_depth",
                                       "continuation",
                                       "remaining_budget",
                                       "exact_checkpoint",
                                       "exact_checkpoint_content_id",
                                       "pending_input",
                                       "pending_effect",
                                       "terminal_result",
                                       "fork_receipt",
                                       "fork_source_run_id",
                                       "fork_source_program_version_id",
                                       "fork_source_checkpoint_id",
                                       "recorded_binding_set_fingerprint",
                                       "journal_head",
                                       "event_sequence",
                                       "effect_sequence",
                                       "created_at_ms",
                                       "updated_at_ms",
                                       "children",
                                       "logical_run_id",
                                       "attempt_resume",
                                       "settled_branches"});
    }
    ProgramRunRecordData d;
    d.owner_scope         = rs(v, "owner_scope");
    d.run_id              = rs(v, "run_id");
    d.program_version_id  = rs(v, "program_version_id");
    d.bundle_id           = rs(v, "bundle_id");
    d.binding_fingerprint = rs(v, "binding_fingerprint");
    d.invocation = RunInvocation::parse(detail::canonical_json_bytes(rv(v, "invocation")));
    d.child_depth = r32(v, "child_depth");
    if (v.contains("logical_run_id")) d.logical_run_id = rs(v, "logical_run_id");
    if (v.contains("attempt_resume")) {
        const auto& resume = rv(v, "attempt_resume");
        if (!resume.is_object())
            throw std::invalid_argument("Program attempt resume must be an object");
        detail::reject_unknown_fields(resume, "Program attempt resume", {"checkpoint_id", "value"});
        d.attempt_resume = ProgramAttemptResume{rs(resume, "checkpoint_id"), rv(resume, "value")};
    }
    if (v.contains("settled_branches"))
        d.settled_branches = detail::owned_json_copy(rv(v, "settled_branches"));
    const auto& continuation = rv(v, "continuation");
    if (!continuation.is_object()) {
        throw std::invalid_argument("Program continuation must be an object");
    }
    if (schema_version >= 7) {
        detail::reject_unknown_fields(continuation, "Program continuation",
                                      {"operation_id", "state", "attempt", "fence"});
        d.continuation = {rs(continuation, "operation_id"),
                          continuation_state_from_string(rs(continuation, "state")),
                          ru(continuation, "attempt"),
                          ru(continuation, "fence")};
        // One record has one encoding: the version with the fence field belongs to a fence other than 1 only.
        if (d.continuation.fence == 1)
            throw std::invalid_argument("Program run record version 7 carries a fence other than 1");
    } else {
        detail::reject_unknown_fields(continuation, "Program continuation",
                                      {"operation_id", "state", "attempt"});
        d.continuation = {rs(continuation, "operation_id"),
                          continuation_state_from_string(rs(continuation, "state")),
                          ru(continuation, "attempt"),
                          1};
    }
    d.remaining_budget     = db(rv(v, "remaining_budget"));
    const auto& checkpoint = rv(v, "exact_checkpoint");
    if (!checkpoint.is_null()) d.exact_checkpoint = dc(checkpoint);
    if (schema_version >= 3)
        d.exact_checkpoint_content_id = os(rv(v, "exact_checkpoint_content_id"));
    d.pending_input                    = opt<ProgramPendingInput>(rv(v, "pending_input"));
    d.pending_effect                   = opt<ProgramPendingEffect>(rv(v, "pending_effect"));
    d.terminal_result                  = opt<ProgramResult>(rv(v, "terminal_result"));
    d.fork_receipt                     = opt<ForkCompatibilityReceipt>(rv(v, "fork_receipt"));
    d.fork_source_run_id               = os(rv(v, "fork_source_run_id"));
    d.fork_source_program_version_id   = os(rv(v, "fork_source_program_version_id"));
    d.fork_source_checkpoint_id        = os(rv(v, "fork_source_checkpoint_id"));
    d.recorded_binding_set_fingerprint = os(rv(v, "recorded_binding_set_fingerprint"));
    if (v.contains("children")) {
        const auto& children = rv(v, "children");
        if (!children.is_array())
            throw std::invalid_argument("Program run children must be an array");
        d.children.reserve(children.size());
        for (const auto& child : children) d.children.push_back(parse_child(child));
        if (schema_version < 4 &&
            std::any_of(
                d.children.begin(), d.children.end(),
                [](const auto& child) { return child.terminal_generation != 0; }))
            throw std::invalid_argument("Legacy child result cannot carry generation proof");
    }
    d.journal_head                     = rs(v, "journal_head");
    d.event_sequence                   = ru(v, "event_sequence");
    d.effect_sequence                  = ru(v, "effect_sequence");
    d.created_at_ms                    = ri(v, "created_at_ms");
    d.updated_at_ms                    = ri(v, "updated_at_ms");
    validate(d);
    validate_continuation_state(d);
    validate_result_state(d);
    if (d.recorded_binding_set_fingerprint &&
        !detail::is_sha256_identity(*d.recorded_binding_set_fingerprint)) {
        throw std::invalid_argument("Recorded binding set fingerprint must be a sha256 identity");
    }
    auto out = ProgramRunRecord(
        std::make_shared<const Impl>(std::move(d), schema_version));
    if (out.id() != rs(v, "id")) {
        throw std::invalid_argument("Stored ProgramRunRecord id does not match canonical body");
    }
    return out;
}
const std::string& ProgramRunRecord::owner_scope() const noexcept {
    return impl_->data.owner_scope;
}
const std::string& ProgramRunRecord::run_id() const noexcept {
    return impl_->data.run_id;
}
const std::string& ProgramRunRecord::program_version_id() const noexcept {
    return impl_->data.program_version_id;
}
const std::string& ProgramRunRecord::bundle_id() const noexcept {
    return impl_->data.bundle_id;
}
const std::string& ProgramRunRecord::binding_fingerprint() const noexcept {
    return impl_->data.binding_fingerprint;
}
const RunInvocation& ProgramRunRecord::invocation() const noexcept {
    return impl_->data.invocation;
}
const std::string& ProgramRunRecord::logical_run_id() const noexcept {
    return impl_->data.logical_run_id.empty() ? impl_->data.run_id : impl_->data.logical_run_id;
}
std::uint32_t ProgramRunRecord::child_depth() const noexcept {
    return impl_->data.child_depth;
}
ProgramContinuation ProgramRunRecord::continuation() const noexcept {
    return impl_->data.continuation;
}
RunBudget ProgramRunRecord::remaining_budget() const noexcept {
    return impl_->data.remaining_budget;
}
std::optional<CoreCheckpointIdentity> ProgramRunRecord::exact_checkpoint() const {
    return impl_->data.exact_checkpoint;
}
const std::optional<std::string>&
ProgramRunRecord::exact_checkpoint_content_id() const noexcept {
    return impl_->data.exact_checkpoint_content_id;
}
const std::optional<ProgramAttemptResume>& ProgramRunRecord::attempt_resume() const noexcept {
    return impl_->data.attempt_resume;
}
std::optional<json> ProgramRunRecord::settled_branches() const {
    return impl_->data.settled_branches
               ? std::optional<json>(detail::owned_json_copy(*impl_->data.settled_branches))
               : std::nullopt;
}
std::optional<ProgramPendingInput> ProgramRunRecord::pending_input() const {
    return impl_->data.pending_input;
}
std::optional<ProgramPendingEffect> ProgramRunRecord::pending_effect() const {
    return impl_->data.pending_effect;
}
std::optional<ProgramResult> ProgramRunRecord::terminal_result() const {
    return hydrated_terminal_ ? hydrated_terminal_ : impl_->data.terminal_result;
}
std::optional<ForkCompatibilityReceipt> ProgramRunRecord::fork_receipt() const {
    return impl_->data.fork_receipt;
}
const std::string& ProgramRunRecord::journal_head() const noexcept {
    return impl_->data.journal_head;
}
std::uint64_t ProgramRunRecord::event_sequence() const noexcept {
    return impl_->data.event_sequence;
}
std::uint64_t ProgramRunRecord::effect_sequence() const noexcept {
    return impl_->data.effect_sequence;
}
bool ProgramRunRecord::has_children() const noexcept {
    return !impl_->data.children.empty();
}
const std::vector<ProgramChildRecord>& ProgramRunRecord::children() const& {
    return impl_->data.children;
}
std::vector<ProgramChildRecord> ProgramRunRecord::children() const&& {
    return impl_->data.children;
}
std::int64_t ProgramRunRecord::created_at_ms() const noexcept {
    return impl_->data.created_at_ms;
}
std::int64_t ProgramRunRecord::updated_at_ms() const noexcept {
    return impl_->data.updated_at_ms;
}
const std::string& ProgramRunRecord::id() const noexcept {
    return impl_->id;
}
std::optional<std::string> ProgramRunRecord::fork_source_run_id() const {
    return impl_->data.fork_source_run_id;
}
std::optional<std::string> ProgramRunRecord::fork_source_program_version_id() const {
    return impl_->data.fork_source_program_version_id;
}
std::optional<std::string> ProgramRunRecord::fork_source_checkpoint_id() const {
    return impl_->data.fork_source_checkpoint_id;
}
std::optional<std::string> ProgramRunRecord::recorded_binding_set_fingerprint() const {
    return impl_->data.recorded_binding_set_fingerprint;
}
std::string ProgramRunRecord::serialize_canonical() const { return impl_->canonical_bytes; }

// ---- Child relation rows and heads (#661) ----

namespace {
constexpr std::string_view RELATION_FORMAT      = "neograph-program-child-relation";
constexpr std::string_view RELATION_HEAD_FORMAT = "neograph-program-child-relation-head";

json budget_body(const RunBudget& budget) {
    Builder j;
    auto*   root = eb(j, budget);
    return std::move(j).finish(root);
}

bool relation_child_is_open(ProgramChildState state) noexcept {
    return state == ProgramChildState::Publishing || state == ProgramChildState::Dispatched;
}

void validate_relation(const ProgramChildRelationData& d) {
    if (d.owner_scope.empty())
        throw std::invalid_argument("Program child relation owner_scope must not be empty");
    detail::validate_utf8(d.owner_scope);
    detail::validate_token(d.parent_run_id, "Program child relation parent run_id");
    if (d.row_version == 0)
        throw std::invalid_argument("Program child relation row version must be positive");
    if (d.child.child_run_id == d.parent_run_id)
        throw std::invalid_argument("Program child relation cannot name its parent as the child");
    if (d.child.invocation.parent_run_id != d.parent_run_id || d.child.invocation.child_depth == 0)
        throw std::invalid_argument("Program child relation invocation does not bind its parent");
    validate_child_shape(d.child);
}

void validate_relation_head(const ProgramChildRelationHeadData& d) {
    if (d.owner_scope.empty())
        throw std::invalid_argument("Program child relation head owner_scope must not be empty");
    detail::validate_utf8(d.owner_scope);
    detail::validate_token(d.parent_run_id, "Program child relation head parent run_id");
    if (d.open_count > d.count)
        throw std::invalid_argument("Program child relation head counts more open rows than rows");
    if (d.epoch < d.count)
        throw std::invalid_argument("Program child relation head epoch is below its row count");
    if (d.closed == d.closed_by.empty())
        throw std::invalid_argument("Program child relation head closed state and closer disagree");
    if (d.committed_budget.max_concurrency != 0 || d.committed_budget.max_child_depth != 0 ||
        d.committed_budget.max_node_retries)
        throw std::invalid_argument("Program child relation head holds only permanent budget");
    if (d.count == 0 && (d.open_count != 0 || d.committed_budget != RunBudget{}))
        throw std::invalid_argument("Program child relation head without rows commits nothing");
}
}  // namespace

struct ProgramChildRelation::Impl {
    explicit Impl(ProgramChildRelationData value) : data(std::move(value)) {
        json body{{"format", std::string(RELATION_FORMAT)},
                  {"storage_schema_version", STORAGE_SCHEMA_VERSION},
                  {"owner_scope", data.owner_scope},
                  {"parent_run_id", data.parent_run_id},
                  {"ordinal", data.ordinal},
                  {"row_version", data.row_version},
                  {"child", child_body(data.child)}};
        id = detail::sha256_identity("program-child-relation/v1", detail::canonical_json_bytes(body));
        body["id"]      = id;
        canonical_bytes = detail::canonical_json_bytes(body);
    }
    ProgramChildRelationData data;
    std::string              id;
    std::string              canonical_bytes;
};

ProgramChildRelation::ProgramChildRelation(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

std::string serialize_program_child_record(const ProgramChildRecord& child) {
    validate_child_shape(child);
    return detail::canonical_json_bytes(child_body(child));
}

ProgramChildRecord parse_program_child_record(std::string_view stored_bytes) {
    auto child = parse_child(detail::parse_json_strict(stored_bytes));
    validate_child_shape(child);
    return child;
}

ProgramChildRelation ProgramChildRelation::create(ProgramChildRelationData data) {
    validate_relation(data);
    return ProgramChildRelation(std::make_shared<const Impl>(std::move(data)));
}

ProgramChildRelation ProgramChildRelation::parse(std::string_view stored_bytes) {
    json v;
    try {
        v = detail::parse_json_strict(stored_bytes);
    } catch (const std::exception& e) {
        throw std::invalid_argument(std::string("Invalid stored ProgramChildRelation JSON: ") + e.what());
    }
    if (!v.is_object() || rs(v, "format") != RELATION_FORMAT)
        throw std::invalid_argument("Stored ProgramChildRelation has unknown format");
    if (r32(v, "storage_schema_version") != STORAGE_SCHEMA_VERSION)
        throw std::invalid_argument("Stored ProgramChildRelation schema version is unsupported");
    detail::reject_unknown_fields(v, "Stored ProgramChildRelation",
                                  {"format", "storage_schema_version", "id", "owner_scope",
                                   "parent_run_id", "ordinal", "row_version", "child"});
    ProgramChildRelationData d;
    d.owner_scope   = rs(v, "owner_scope");
    d.parent_run_id = rs(v, "parent_run_id");
    d.ordinal       = ru(v, "ordinal");
    d.row_version   = ru(v, "row_version");
    d.child         = parse_child(rv(v, "child"));
    auto out        = ProgramChildRelation::create(std::move(d));
    if (out.id() != rs(v, "id"))
        throw std::invalid_argument("Stored ProgramChildRelation id does not match canonical body");
    return out;
}

const ProgramChildRelationData& ProgramChildRelation::data() const noexcept { return impl_->data; }
const std::string& ProgramChildRelation::owner_scope() const noexcept { return impl_->data.owner_scope; }
const std::string& ProgramChildRelation::parent_run_id() const noexcept { return impl_->data.parent_run_id; }
const std::string& ProgramChildRelation::child_run_id() const noexcept {
    return impl_->data.child.child_run_id;
}
std::uint64_t ProgramChildRelation::ordinal() const noexcept { return impl_->data.ordinal; }
std::uint64_t ProgramChildRelation::row_version() const noexcept { return impl_->data.row_version; }
const ProgramChildRecord& ProgramChildRelation::child() const noexcept { return impl_->data.child; }
bool ProgramChildRelation::is_open() const noexcept {
    return relation_child_is_open(impl_->data.child.state);
}
const std::string& ProgramChildRelation::id() const noexcept { return impl_->id; }
std::string ProgramChildRelation::serialize_canonical() const { return impl_->canonical_bytes; }

struct ProgramChildRelationHead::Impl {
    explicit Impl(ProgramChildRelationHeadData value) : data(std::move(value)) {
        json body{{"format", std::string(RELATION_HEAD_FORMAT)},
                  {"storage_schema_version", STORAGE_SCHEMA_VERSION},
                  {"owner_scope", data.owner_scope},
                  {"parent_run_id", data.parent_run_id},
                  {"epoch", data.epoch},
                  {"count", data.count},
                  {"open_count", data.open_count},
                  {"committed_budget", budget_body(data.committed_budget)},
                  {"closed", data.closed},
                  {"closed_by", data.closed_by}};
        id = detail::sha256_identity("program-child-relation-head/v1",
                                     detail::canonical_json_bytes(body));
        body["id"]      = id;
        canonical_bytes = detail::canonical_json_bytes(body);
    }
    ProgramChildRelationHeadData data;
    std::string                  id;
    std::string                  canonical_bytes;
};

ProgramChildRelationHead::ProgramChildRelationHead(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

ProgramChildRelationHead ProgramChildRelationHead::empty(std::string_view owner_scope,
                                                         std::string_view parent_run_id) {
    ProgramChildRelationHeadData data;
    data.owner_scope   = std::string(owner_scope);
    data.parent_run_id = std::string(parent_run_id);
    return create(std::move(data));
}

ProgramChildRelationHead ProgramChildRelationHead::create(ProgramChildRelationHeadData data) {
    validate_relation_head(data);
    return ProgramChildRelationHead(std::make_shared<const Impl>(std::move(data)));
}

ProgramChildRelationHead ProgramChildRelationHead::parse(std::string_view stored_bytes) {
    json v;
    try {
        v = detail::parse_json_strict(stored_bytes);
    } catch (const std::exception& e) {
        throw std::invalid_argument(std::string("Invalid stored ProgramChildRelationHead JSON: ") +
                                    e.what());
    }
    if (!v.is_object() || rs(v, "format") != RELATION_HEAD_FORMAT)
        throw std::invalid_argument("Stored ProgramChildRelationHead has unknown format");
    if (r32(v, "storage_schema_version") != STORAGE_SCHEMA_VERSION)
        throw std::invalid_argument("Stored ProgramChildRelationHead schema version is unsupported");
    detail::reject_unknown_fields(v, "Stored ProgramChildRelationHead",
                                  {"format", "storage_schema_version", "id", "owner_scope",
                                   "parent_run_id", "epoch", "count", "open_count",
                                   "committed_budget", "closed", "closed_by"});
    ProgramChildRelationHeadData d;
    d.owner_scope      = rs(v, "owner_scope");
    d.parent_run_id    = rs(v, "parent_run_id");
    d.epoch            = ru(v, "epoch");
    d.count            = ru(v, "count");
    d.open_count       = ru(v, "open_count");
    d.committed_budget = db(rv(v, "committed_budget"));
    if (!rv(v, "closed").is_boolean())
        throw std::invalid_argument("Program child relation head field 'closed' must be a boolean");
    d.closed    = rv(v, "closed").get<bool>();
    d.closed_by = rs(v, "closed_by");
    auto out    = ProgramChildRelationHead::create(std::move(d));
    if (out.id() != rs(v, "id"))
        throw std::invalid_argument("Stored ProgramChildRelationHead id does not match canonical body");
    return out;
}

const ProgramChildRelationHeadData& ProgramChildRelationHead::data() const noexcept {
    return impl_->data;
}
std::uint64_t ProgramChildRelationHead::epoch() const noexcept { return impl_->data.epoch; }
std::uint64_t ProgramChildRelationHead::count() const noexcept { return impl_->data.count; }
std::uint64_t ProgramChildRelationHead::open_count() const noexcept { return impl_->data.open_count; }
const RunBudget& ProgramChildRelationHead::committed_budget() const noexcept {
    return impl_->data.committed_budget;
}
bool ProgramChildRelationHead::closed() const noexcept { return impl_->data.closed; }
const std::string& ProgramChildRelationHead::id() const noexcept { return impl_->id; }
std::string ProgramChildRelationHead::serialize_canonical() const { return impl_->canonical_bytes; }

std::string_view to_string(ProgramChildRelationRefusal refusal) noexcept {
    switch (refusal) {
        case ProgramChildRelationRefusal::None: return "none";
        case ProgramChildRelationRefusal::ParentAbsent: return "parent_absent";
        case ProgramChildRelationRefusal::ParentNotRunning: return "parent_not_running";
        case ProgramChildRelationRefusal::FenceMismatch: return "fence_mismatch";
        case ProgramChildRelationRefusal::Closed: return "closed";
        case ProgramChildRelationRefusal::RowConflict: return "row_conflict";
        case ProgramChildRelationRefusal::RowMissing: return "row_missing";
        case ProgramChildRelationRefusal::RowVersionMismatch: return "row_version_mismatch";
        case ProgramChildRelationRefusal::InvalidTransition: return "invalid_transition";
        case ProgramChildRelationRefusal::BudgetExceeded: return "budget_exceeded";
        case ProgramChildRelationRefusal::ResultUnbound: return "result_unbound";
        case ProgramChildRelationRefusal::Invalid: return "invalid";
    }
    return "unknown";
}

}  // namespace neograph::program
