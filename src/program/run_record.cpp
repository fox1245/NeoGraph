#include <neograph/program/run_record.h>

#include "canonical_json.h"

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
json eb(const RunBudget& b) {
    return {{"wall_time_ms", b.wall_time_ms},
            {"model_tokens", b.model_tokens},
            {"monetary_microunits", b.monetary_microunits},
            {"max_concurrency", b.max_concurrency},
            {"max_program_operations", b.max_program_operations},
            {"max_core_steps", b.max_core_steps},
            {"max_dynamic_compiles", b.max_dynamic_compiles},
            {"max_child_depth", b.max_child_depth},
            {"max_total_children", b.max_total_children}};
}
RunBudget db(const json& v) {
    if (!v.is_object()) throw std::invalid_argument("Program run budget must be an object");
    detail::reject_unknown_fields(
        v, "Program run budget",
        {"wall_time_ms", "model_tokens", "monetary_microunits", "max_concurrency",
         "max_program_operations", "max_core_steps", "max_dynamic_compiles", "max_child_depth",
         "max_total_children"});
    return {ru(v, "wall_time_ms"),           ru(v, "model_tokens"),
            ru(v, "monetary_microunits"),    r32(v, "max_concurrency"),
            ru(v, "max_program_operations"), ru(v, "max_core_steps"),
            ru(v, "max_dynamic_compiles"),   r32(v, "max_child_depth"),
            ru(v, "max_total_children")};
}
json invocation_body(const ProgramPersistedInvocation& invocation) {
    json value{{"input", invocation.input},
               {"granted_budget", eb(invocation.granted_budget)},
               {"trace_id", invocation.trace_id}};
    if (!invocation.parent_run_id.empty()) value["parent_run_id"] = invocation.parent_run_id;
    if (invocation.child_depth != 0) value["child_depth"] = invocation.child_depth;
    return value;
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

json child_body(const ProgramChildRecord& child) {
    json value{{"child_run_id", child.child_run_id},
                {"link_id", child.link_id},
                {"link_receipt", child.link_receipt},
                {"invocation", invocation_body(child.invocation)},
                {"state", std::string(child_state_string(child.state))}};
    if (child.terminal_result)
        value["terminal_result"] =
            detail::parse_json_strict(child.terminal_result->serialize_canonical());
    return value;
}

ProgramChildRecord parse_child(const json& value) {
    if (!value.is_object())
        throw std::invalid_argument("Program child record must be an object");
    detail::reject_unknown_fields(value,
                                  "Program child record",
                                  {"child_run_id", "link_id", "link_receipt", "invocation", "state",
                                   "terminal_result"});
    ProgramChildRecord result{rs(value, "child_run_id"),
                              rs(value, "link_id"),
                              rs(value, "link_receipt"),
                              parse_invocation(rv(value, "invocation"), "Program child invocation"),
                              child_state_from_string(rs(value, "state"))};
    if (value.contains("terminal_result") && !value.at("terminal_result").is_null())
        result.terminal_result = ProgramResult::parse(
            detail::canonical_json_bytes(value.at("terminal_result")));
    return result;
}
json ec(const CoreCheckpointIdentity& c) {
    return {{"core_name", c.core_name},
            {"core_generation_id", c.core_generation_id},
            {"core_thread_id", c.core_thread_id},
            {"checkpoint_id", c.checkpoint_id},
            {"checkpoint_schema_version", c.checkpoint_schema_version}};
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
    detail::validate_token(d.continuation.operation_id, "Program run operation_id");
    if (!d.continuation.attempt) {
        throw std::invalid_argument("Program run attempt must be positive");
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
    }
    std::set<std::string, std::less<>> child_ids;
    for (const auto& child : d.children) {
        detail::validate_token(child.child_run_id, "Program child run_id");
        if (child.child_run_id == d.run_id || !child_ids.insert(child.child_run_id).second)
            throw std::invalid_argument("Program run child identities must be unique");
        if (!detail::is_sha256_identity(child.link_id))
            throw std::invalid_argument("Program run child link identity must be a sha256 value");
        if (child.link_receipt.empty())
            throw std::invalid_argument("Program child link receipt must not be empty");
        detail::validate_utf8(child.link_receipt);
        if (child.invocation.parent_run_id != d.run_id ||
            d.child_depth == std::numeric_limits<std::uint32_t>::max() ||
            child.invocation.child_depth != d.child_depth + 1) {
            throw std::invalid_argument("Program child invocation does not bind its parent");
        }
        if ((child.state == ProgramChildState::Publishing && child.terminal_result) ||
            ((child.state == ProgramChildState::Completed ||
              child.state == ProgramChildState::Failed) &&
             !child.terminal_result)) {
            throw std::invalid_argument(
                "Program child terminal result does not match its lifecycle state");
        }
        if (child.terminal_result) {
            const auto& result = *child.terminal_result;
            const bool failed_status =
                result.status() == ProgramTerminalStatus::Failed ||
                result.status() == ProgramTerminalStatus::BudgetExhausted ||
                result.status() == ProgramTerminalStatus::TimedOut ||
                result.status() == ProgramTerminalStatus::CheckpointIncompatible;
            if (result.run_id() != child.child_run_id ||
                result.program_version_id().empty() ||
                result.attempt() == 0 ||
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
}
json body(const ProgramRunRecordData& d) {
    json value{{"format", std::string(FORMAT)},
               {"storage_schema_version", ProgramRunRecord::STORAGE_SCHEMA_VERSION},
               {"owner_scope", d.owner_scope},
               {"run_id", d.run_id},
               {"program_version_id", d.program_version_id},
               {"bundle_id", d.bundle_id},
               {"binding_fingerprint", d.binding_fingerprint},
               {"invocation", nested(d.invocation.serialize_canonical())},
               {"child_depth", d.child_depth},
               {"continuation", json{{"operation_id", d.continuation.operation_id},
                                     {"state", std::string(to_string(d.continuation.state))},
                                     {"attempt", d.continuation.attempt}}},
               {"remaining_budget", eb(d.remaining_budget)},
               {"exact_checkpoint", d.exact_checkpoint ? ec(*d.exact_checkpoint) : json(nullptr)},
               {"pending_input",
                d.pending_input ? nested(d.pending_input->serialize_canonical()) : json(nullptr)},
               {"pending_effect",
                d.pending_effect ? nested(d.pending_effect->serialize_canonical()) : json(nullptr)},
               {"terminal_result",
                d.terminal_result ? nested(d.terminal_result->serialize_canonical()) : json(nullptr)},
               {"fork_receipt",
                d.fork_receipt ? nested(d.fork_receipt->serialize_canonical()) : json(nullptr)},
               {"fork_source_run_id",
                d.fork_source_run_id ? json(*d.fork_source_run_id) : json(nullptr)},
               {"fork_source_program_version_id",
                d.fork_source_program_version_id ? json(*d.fork_source_program_version_id)
                                                  : json(nullptr)},
               {"fork_source_checkpoint_id",
                d.fork_source_checkpoint_id ? json(*d.fork_source_checkpoint_id) : json(nullptr)},
               {"journal_head", d.journal_head},
               {"event_sequence", d.event_sequence},
               {"effect_sequence", d.effect_sequence},
               {"created_at_ms", d.created_at_ms},
               {"updated_at_ms", d.updated_at_ms}};
    if (!d.children.empty()) {
        value["children"] = json::array();
        for (const auto& child : d.children) value["children"].push_back(child_body(child));
    }
    return value;
}
json stored_body(const ProgramRunRecordData& d) {
    auto value                                = body(d);
    value["recorded_binding_set_fingerprint"] = d.recorded_binding_set_fingerprint
                                                    ? json(*d.recorded_binding_set_fingerprint)
                                                    : json(nullptr);
    return value;
}
std::string hash(const ProgramRunRecordData& d) {
    return detail::sha256_identity("program-run-record/v1",
                                   detail::canonical_json_bytes(stored_body(d)));
}
}  // namespace
std::string_view to_string(ProgramChildState state) noexcept {
    return child_state_string(state);
}

ProgramChildState program_child_state_from_string(std::string_view value) {
    return child_state_from_string(value);
}
struct ProgramRunRecord::Impl {
    explicit Impl(ProgramRunRecordData d) : data(std::move(d)), id(hash(data)) {}
    ProgramRunRecordData data;
    std::string          id;
};
ProgramRunRecord::ProgramRunRecord(std::shared_ptr<const Impl> p) : impl_(std::move(p)) {}
ProgramRunRecord ProgramRunRecord::create(ProgramRunRecordData d) {
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
    return ProgramRunRecord(std::make_shared<const Impl>(std::move(d)));
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
                                   "children"});
    if (r32(v, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored ProgramRunRecord schema version is unsupported");
    }
    ProgramRunRecordData d;
    d.owner_scope         = rs(v, "owner_scope");
    d.run_id              = rs(v, "run_id");
    d.program_version_id  = rs(v, "program_version_id");
    d.bundle_id           = rs(v, "bundle_id");
    d.binding_fingerprint = rs(v, "binding_fingerprint");
    d.invocation = RunInvocation::parse(detail::canonical_json_bytes(rv(v, "invocation")));
    d.child_depth = r32(v, "child_depth");
    const auto& continuation = rv(v, "continuation");
    if (!continuation.is_object()) {
        throw std::invalid_argument("Program continuation must be an object");
    }
    detail::reject_unknown_fields(continuation, "Program continuation",
                                  {"operation_id", "state", "attempt"});
    d.continuation         = {rs(continuation, "operation_id"),
                              continuation_state_from_string(rs(continuation, "state")),
                              ru(continuation, "attempt")};
    d.remaining_budget     = db(rv(v, "remaining_budget"));
    const auto& checkpoint = rv(v, "exact_checkpoint");
    if (!checkpoint.is_null()) d.exact_checkpoint = dc(checkpoint);
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
    }
    d.journal_head                     = rs(v, "journal_head");
    d.event_sequence                   = ru(v, "event_sequence");
    d.effect_sequence                  = ru(v, "effect_sequence");
    d.created_at_ms                    = ri(v, "created_at_ms");
    d.updated_at_ms                    = ri(v, "updated_at_ms");
    auto out                           = create(std::move(d));
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
std::optional<ProgramPendingInput> ProgramRunRecord::pending_input() const {
    return impl_->data.pending_input;
}
std::optional<ProgramPendingEffect> ProgramRunRecord::pending_effect() const {
    return impl_->data.pending_effect;
}
std::optional<ProgramResult> ProgramRunRecord::terminal_result() const {
    return impl_->data.terminal_result;
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
std::vector<ProgramChildRecord> ProgramRunRecord::children() const {
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
std::string ProgramRunRecord::serialize_canonical() const {
    validate(impl_->data);
    validate_continuation_state(impl_->data);
    validate_result_state(impl_->data);
    auto value = stored_body(impl_->data);
    const auto bytes = detail::canonical_json_bytes(value);
    if (impl_->id != detail::sha256_identity("program-run-record/v1", bytes))
        throw std::invalid_argument("Program run record id does not match canonical body");
    value["id"] = impl_->id;
    return detail::canonical_json_bytes(value);
}
}  // namespace neograph::program
