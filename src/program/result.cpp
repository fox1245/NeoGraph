#include <neograph/program/result.h>

#include "canonical_json.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {
constexpr std::string_view RESULT_FORMAT = "neograph-program-result";
constexpr std::uint32_t RESULT_SCHEMA_VERSION = 1;

std::string require_string(const json& v, std::string_view key) {
    const std::string k(key);
    if (!v.contains(k) || !v[k].is_string()) throw std::invalid_argument("Program result field '" + k + "' must be a string");
    return v[k].get<std::string>();
}
std::uint64_t require_u64(const json& v, std::string_view key) {
    const std::string k(key);
    if (!v.contains(k) || !v[k].is_number_unsigned()) throw std::invalid_argument("Program result field '" + k + "' must be unsigned");
    return v[k].get<std::uint64_t>();
}
bool operation_id_matches_result(std::string_view result_operation,
                                 std::string_view failure_operation) noexcept {
    if (failure_operation == result_operation) return true;
    return failure_operation.size() > result_operation.size() + 1 &&
           failure_operation.compare(0, result_operation.size(), result_operation) == 0 &&
           failure_operation[result_operation.size()] == '.';
}
std::uint32_t require_u32(const json& v, std::string_view key) {
    const auto n = require_u64(v, key);
    if (n > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("Program result integer exceeds uint32 range");
    return static_cast<std::uint32_t>(n);
}
json require_value(const json& v, std::string_view key) {
    const std::string k(key);
    if (!v.contains(k)) throw std::invalid_argument("Program result requires field '" + k + "'");
    return v[k];
}
json encode_budget(const RunBudget& b) {
    return json{{"wall_time_ms", b.wall_time_ms}, {"model_tokens", b.model_tokens}, {"monetary_microunits", b.monetary_microunits}, {"max_concurrency", b.max_concurrency}, {"max_program_operations", b.max_program_operations}, {"max_core_steps", b.max_core_steps}, {"max_dynamic_compiles", b.max_dynamic_compiles}, {"max_child_depth", b.max_child_depth}, {"max_total_children", b.max_total_children}};
}
RunBudget parse_budget(const json& v) {
    if (!v.is_object()) throw std::invalid_argument("Program result budget must be an object");
    detail::reject_unknown_fields(v, "Program result budget", {"wall_time_ms", "model_tokens", "monetary_microunits", "max_concurrency", "max_program_operations", "max_core_steps", "max_dynamic_compiles", "max_child_depth", "max_total_children"});
    return {require_u64(v,"wall_time_ms"), require_u64(v,"model_tokens"), require_u64(v,"monetary_microunits"), require_u32(v,"max_concurrency"), require_u64(v,"max_program_operations"), require_u64(v,"max_core_steps"), require_u64(v,"max_dynamic_compiles"), require_u32(v,"max_child_depth"), require_u64(v,"max_total_children")};
}
json encode_usage(const ProgramUsage& u) {
    return json{{"wall_time_ms",u.wall_time_ms},{"model_tokens",u.model_tokens},{"monetary_microunits",u.monetary_microunits},{"program_operations",u.program_operations},{"core_steps",u.core_steps},{"peak_concurrency",u.peak_concurrency}};
}
ProgramUsage parse_usage(const json& v) {
    if (!v.is_object()) throw std::invalid_argument("Program result usage must be an object");
    detail::reject_unknown_fields(v,"Program result usage",{"wall_time_ms","model_tokens","monetary_microunits","program_operations","core_steps","peak_concurrency"});
    return {require_u64(v,"wall_time_ms"),require_u64(v,"model_tokens"),require_u64(v,"monetary_microunits"),require_u64(v,"program_operations"),require_u64(v,"core_steps"),require_u32(v,"peak_concurrency")};
}
json encode_checkpoint(const CoreCheckpointIdentity& c) { return json{{"core_name",c.core_name},{"core_generation_id",c.core_generation_id},{"core_thread_id",c.core_thread_id},{"checkpoint_id",c.checkpoint_id},{"checkpoint_schema_version",c.checkpoint_schema_version}}; }
CoreCheckpointIdentity parse_checkpoint(const json& v) {
    if (!v.is_object()) throw std::invalid_argument("Program result checkpoint must be an object");
    detail::reject_unknown_fields(v,"Program result checkpoint",{"core_name","core_generation_id","core_thread_id","checkpoint_id","checkpoint_schema_version"});
    return {require_string(v,"core_name"),require_string(v,"core_generation_id"),require_string(v,"core_thread_id"),require_string(v,"checkpoint_id"),require_u32(v,"checkpoint_schema_version")};
}
json encode_interrupt(const ProgramInterrupt& i) {
    return json{{"core_node", i.core_node},
                {"value", i.value},
                {"pending_input",
                 i.pending_input
                     ? detail::parse_json_strict(i.pending_input->serialize_canonical())
                     : json(nullptr)},
                {"pending_effect",
                 i.pending_effect
                     ? detail::parse_json_strict(i.pending_effect->serialize_canonical())
                     : json(nullptr)}};
}
ProgramInterrupt parse_interrupt(const json& v) {
    if (!v.is_object()) throw std::invalid_argument("Program result interrupt must be an object");
    detail::reject_unknown_fields(
        v, "Program result interrupt",
        {"core_node", "value", "pending_input", "pending_effect"});
    ProgramInterrupt parsed{require_string(v, "core_node"), require_value(v, "value")};
    const auto& pending_input  = require_value(v, "pending_input");
    const auto& pending_effect = require_value(v, "pending_effect");
    if (!pending_input.is_null()) {
        parsed.pending_input =
            ProgramPendingInput::parse(detail::canonical_json_bytes(pending_input));
    }
    if (!pending_effect.is_null()) {
        parsed.pending_effect =
            ProgramPendingEffect::parse(detail::canonical_json_bytes(pending_effect));
    }
    return parsed;
}
json encode_failure(const ProgramFailure& f) { return json{{"code",f.code},{"message",f.message},{"operation_id",f.operation_id},{"core_node",f.core_node},{"attempts",f.attempts},{"witness",f.witness}}; }
ProgramFailure parse_failure(const json& v) { if (!v.is_object()) throw std::invalid_argument("Program result failure must be an object"); detail::reject_unknown_fields(v,"Program result failure",{"code","message","operation_id","core_node","attempts","witness"}); return {require_string(v,"code"),require_string(v,"message"),require_string(v,"operation_id"),require_string(v,"core_node"),require_u32(v,"attempts"),require_value(v,"witness")}; }
void validate_checkpoint(const CoreCheckpointIdentity& c) {
    detail::validate_token(c.core_name,"Program result checkpoint core_name");
    if (!detail::is_sha256_identity(c.core_generation_id)) throw std::invalid_argument("Program result checkpoint core_generation_id must be a sha256 identity");
    detail::validate_token(c.core_thread_id,"Program result checkpoint core_thread_id"); detail::validate_token(c.checkpoint_id,"Program result checkpoint checkpoint_id");
    if (c.checkpoint_schema_version == 0) throw std::invalid_argument("Program result checkpoint schema version must be positive");
}
void validate_data(const ProgramResultData& d) {
    detail::validate_token(d.run_id,"Program result run_id");
    if (!detail::is_sha256_identity(d.program_version_id) || !detail::is_sha256_identity(d.bundle_id)) throw std::invalid_argument("Program result version and bundle ids must be sha256 identities");
    detail::validate_token(d.operation_id,"Program result operation_id"); if (d.attempt == 0) throw std::invalid_argument("Program result attempt must be positive"); if (d.checkpoint) validate_checkpoint(*d.checkpoint);
    if (d.interrupt) {
        detail::validate_token(d.interrupt->core_node, "Program result interrupt core_node");
        if (d.interrupt->pending_input && d.interrupt->pending_effect) {
            throw std::invalid_argument(
                "Program result interrupt cannot contain both pending input and effect");
        }
        if (d.interrupt->pending_input) {
            if (d.interrupt->core_node != d.interrupt->pending_input->core_node() ||
                d.interrupt->value !=
                    d.interrupt->pending_input->core_interrupt_value()) {
                throw std::invalid_argument(
                    "Program interrupt projection disagrees with pending input");
            }
        }
        if (d.interrupt->pending_effect) {
            if (d.interrupt->core_node != d.interrupt->pending_effect->core_node() ||
                d.interrupt->value !=
                    d.interrupt->pending_effect->core_interrupt_value()) {
                throw std::invalid_argument(
                    "Program interrupt projection disagrees with pending effect");
            }
        }
    }
    if (d.status == ProgramTerminalStatus::Interrupted) {
        const bool awaiting_input =
            d.interrupt && d.interrupt->pending_input &&
            d.interrupt->pending_input->state() == ProgramPendingState::Awaiting;
        const bool awaiting_effect =
            d.interrupt && d.interrupt->pending_effect &&
            d.interrupt->pending_effect->state() == ProgramPendingState::Awaiting;
        if (awaiting_input == awaiting_effect) {
            throw std::invalid_argument(
                "Interrupted Program result requires exactly one awaiting pending value");
        }
    } else if (d.status == ProgramTerminalStatus::AmbiguousEffect) {
        if (!d.interrupt || d.interrupt->pending_input ||
            !d.interrupt->pending_effect ||
            d.interrupt->pending_effect->state() != ProgramPendingState::Ambiguous) {
            throw std::invalid_argument(
                "Ambiguous Program result requires one ambiguous pending effect");
        }
    } else if (d.interrupt) {
        throw std::invalid_argument(
            "Only interrupted or ambiguous Program results may contain an interrupt");
    }
    if (d.failure) {
        detail::validate_token(d.failure->code, "Program result failure code");
        detail::validate_token(d.failure->operation_id, "Program result failure operation_id");
        if (!operation_id_matches_result(d.operation_id, d.failure->operation_id))
            throw std::invalid_argument("Program result failure operation_id does not match result");
    }
    for (const auto& node : d.execution_trace) detail::validate_token(node,"Program result execution trace node");
}
json result_body(const ProgramResultData& d) { return json{{"format",std::string(RESULT_FORMAT)},{"storage_schema_version",RESULT_SCHEMA_VERSION},{"status",std::string(to_string(d.status))},{"run_id",d.run_id},{"program_version_id",d.program_version_id},{"bundle_id",d.bundle_id},{"operation_id",d.operation_id},{"attempt",d.attempt},{"output",d.output},{"usage",encode_usage(d.usage)},{"remaining_budget",encode_budget(d.remaining_budget)},{"checkpoint",d.checkpoint ? encode_checkpoint(*d.checkpoint) : json(nullptr)},{"interrupt",d.interrupt ? encode_interrupt(*d.interrupt) : json(nullptr)},{"failure",d.failure ? encode_failure(*d.failure) : json(nullptr)},{"execution_trace",d.execution_trace}}; }
}  // namespace

std::string_view to_string(ProgramTerminalStatus s) noexcept { switch (s) { case ProgramTerminalStatus::Completed:return "completed"; case ProgramTerminalStatus::Interrupted:return "interrupted"; case ProgramTerminalStatus::Cancelled:return "cancelled"; case ProgramTerminalStatus::BudgetExhausted:return "budget_exhausted"; case ProgramTerminalStatus::TimedOut:return "timed_out"; case ProgramTerminalStatus::Failed:return "failed"; case ProgramTerminalStatus::AmbiguousEffect:return "ambiguous_effect"; case ProgramTerminalStatus::CheckpointIncompatible:return "checkpoint_incompatible"; } return "unknown"; }
ProgramTerminalStatus program_terminal_status_from_string(std::string_view v) { if(v=="completed")return ProgramTerminalStatus::Completed; if(v=="interrupted")return ProgramTerminalStatus::Interrupted; if(v=="cancelled")return ProgramTerminalStatus::Cancelled; if(v=="budget_exhausted")return ProgramTerminalStatus::BudgetExhausted; if(v=="timed_out")return ProgramTerminalStatus::TimedOut; if(v=="failed")return ProgramTerminalStatus::Failed; if(v=="ambiguous_effect")return ProgramTerminalStatus::AmbiguousEffect; if(v=="checkpoint_incompatible")return ProgramTerminalStatus::CheckpointIncompatible; throw std::invalid_argument("Unknown Program terminal status: "+std::string(v)); }

struct ProgramResult::Impl {
    Impl(ProgramResultData value, bool cache_canonical) : data(std::move(value)) {
        auto body             = result_body(data);
        const auto body_bytes = detail::canonical_json_bytes(body);
        id = detail::sha256_identity("program-result/v1", body_bytes);
        if (cache_canonical) {
            body["id"] = id;
            canonical_bytes = detail::canonical_json_bytes(body);
        }
    }

    ProgramResultData           data;
    std::string                 id;
    std::optional<std::string> canonical_bytes;
};

ProgramResult::ProgramResult()
    : impl_(std::make_shared<const Impl>(
          ConstructionData{ProgramTerminalStatus::Failed,
                           "",
                           "",
                           "",
                           "root",
                           0,
                           json::object(),
                           {},
                           {},
                           std::nullopt,
                           std::nullopt,
                           ProgramFailure{"P_RESULT_EMPTY",
                                          "Empty Program result",
                                          "root",
                                          "",
                                          0,
                                          json::object()},
                           {}},
          false)) {}
ProgramResult::ProgramResult(ConstructionData d)
    : impl_(std::make_shared<const Impl>(std::move(d), true)) {}
ProgramResult ProgramResult::create(ProgramResultData d) {
    validate_data(d);
    return ProgramResult(std::move(d));
}
ProgramResult ProgramResult::parse(std::string_view bytes) {
    json v; try { v=detail::parse_json_strict(bytes); } catch(const std::exception& e){throw std::invalid_argument(std::string("Invalid stored ProgramResult JSON: ")+e.what());}
    if(!v.is_object()||require_string(v,"format")!=RESULT_FORMAT)throw std::invalid_argument("Stored ProgramResult has unknown format");
    detail::reject_unknown_fields(v,"Stored ProgramResult",{"format","storage_schema_version","id","status","run_id","program_version_id","bundle_id","operation_id","attempt","output","usage","remaining_budget","checkpoint","interrupt","failure","execution_trace"});
    if(require_u32(v,"storage_schema_version")!=RESULT_SCHEMA_VERSION)throw std::invalid_argument("Stored ProgramResult schema version is unsupported");
    ProgramResultData d; d.status=program_terminal_status_from_string(require_string(v,"status")); d.run_id=require_string(v,"run_id"); d.program_version_id=require_string(v,"program_version_id"); d.bundle_id=require_string(v,"bundle_id"); d.operation_id=require_string(v,"operation_id"); d.attempt=require_u64(v,"attempt"); d.output=require_value(v,"output"); d.usage=parse_usage(require_value(v,"usage")); d.remaining_budget=parse_budget(require_value(v,"remaining_budget"));
    const auto& c=require_value(v,"checkpoint"); const auto& i=require_value(v,"interrupt"); const auto& f=require_value(v,"failure"); if(!c.is_null())d.checkpoint=parse_checkpoint(c); if(!i.is_null())d.interrupt=parse_interrupt(i); if(!f.is_null())d.failure=parse_failure(f);
    const auto& trace=require_value(v,"execution_trace"); if(!trace.is_array())throw std::invalid_argument("Program result execution_trace must be an array"); for(const auto& n:trace){if(!n.is_string())throw std::invalid_argument("Program result execution trace entries must be strings");d.execution_trace.push_back(n.get<std::string>());}
    auto result=create(std::move(d)); if(result.id()!=require_string(v,"id"))throw std::invalid_argument("Stored ProgramResult id does not match its canonical body"); return result;
}
std::string ProgramResult::serialize_canonical() const {
    validate_data(impl_->data);
    if (impl_->canonical_bytes) return *impl_->canonical_bytes;

    auto value             = result_body(impl_->data);
    const auto body_bytes = detail::canonical_json_bytes(value);
    if (impl_->id != detail::sha256_identity("program-result/v1", body_bytes))
        throw std::invalid_argument("Program result id does not match canonical body");
    value["id"] = impl_->id;
    return detail::canonical_json_bytes(value);
}
const std::string& ProgramResult::id()const noexcept{return impl_->id;} ProgramTerminalStatus ProgramResult::status()const noexcept{return impl_->data.status;} const std::string& ProgramResult::run_id()const noexcept{return impl_->data.run_id;} const std::string& ProgramResult::program_version_id()const noexcept{return impl_->data.program_version_id;} const std::string& ProgramResult::bundle_id()const noexcept{return impl_->data.bundle_id;} const std::string& ProgramResult::operation_id()const noexcept{return impl_->data.operation_id;} std::uint64_t ProgramResult::attempt()const noexcept{return impl_->data.attempt;} json ProgramResult::output()const{return impl_->data.output;} ProgramUsage ProgramResult::usage()const noexcept{return impl_->data.usage;} RunBudget ProgramResult::remaining_budget()const noexcept{return impl_->data.remaining_budget;} std::optional<CoreCheckpointIdentity> ProgramResult::checkpoint()const{return impl_->data.checkpoint;} std::optional<ProgramInterrupt> ProgramResult::interrupt()const{return impl_->data.interrupt;} std::optional<ProgramFailure> ProgramResult::failure()const{return impl_->data.failure;} std::vector<std::string> ProgramResult::execution_trace()const{return impl_->data.execution_trace;}
}  // namespace neograph::program
