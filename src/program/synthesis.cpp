#include <neograph/program/synthesis.h>

#include "canonical_json.h"

#include <algorithm>
#include <stdexcept>

namespace neograph::program {
namespace {

void require_sha(std::string_view value, std::string_view field) {
    if (!detail::is_sha256_identity(value)) {
        throw std::invalid_argument(std::string(field) + " must be a sha256 identity");
    }
}

void require_token(std::string_view value, std::string_view field) {
    detail::validate_token(value, field);
}

void normalize_tokens(std::vector<std::string>& values, std::string_view field) {
    for (const auto& value : values) require_token(value, field);
    std::sort(values.begin(), values.end());
    if (std::adjacent_find(values.begin(), values.end()) != values.end()) {
        throw std::invalid_argument(std::string(field) + " contains a duplicate");
    }
}

json budget_json(const RunBudget& value) {
    return {{"wall_time_ms", value.wall_time_ms},
            {"model_tokens", value.model_tokens},
            {"monetary_microunits", value.monetary_microunits},
            {"max_concurrency", value.max_concurrency},
            {"max_program_operations", value.max_program_operations},
            {"max_core_steps", value.max_core_steps},
            {"max_dynamic_compiles", value.max_dynamic_compiles},
            {"max_child_depth", value.max_child_depth},
            {"max_total_children", value.max_total_children}};
}

RunBudget budget_from(const json& value) {
    detail::reject_unknown_fields(
        value, "Stored synthesis budget",
        {"wall_time_ms", "model_tokens", "monetary_microunits",
         "max_concurrency", "max_program_operations", "max_core_steps",
         "max_dynamic_compiles", "max_child_depth", "max_total_children"});
    RunBudget result;
    result.wall_time_ms = value.at("wall_time_ms").get<std::uint64_t>();
    result.model_tokens = value.at("model_tokens").get<std::uint64_t>();
    result.monetary_microunits =
        value.at("monetary_microunits").get<std::uint64_t>();
    result.max_concurrency = value.at("max_concurrency").get<std::uint32_t>();
    result.max_program_operations =
        value.at("max_program_operations").get<std::uint64_t>();
    result.max_core_steps = value.at("max_core_steps").get<std::uint64_t>();
    result.max_dynamic_compiles =
        value.at("max_dynamic_compiles").get<std::uint64_t>();
    result.max_child_depth = value.at("max_child_depth").get<std::uint32_t>();
    result.max_total_children =
        value.at("max_total_children").get<std::uint64_t>();
    return result;
}

bool no_budget_increase(const RunBudget& before, const RunBudget& after) {
    return after.wall_time_ms <= before.wall_time_ms &&
           after.model_tokens <= before.model_tokens &&
           after.monetary_microunits <= before.monetary_microunits &&
           after.max_concurrency <= before.max_concurrency &&
           after.max_program_operations <= before.max_program_operations &&
           after.max_core_steps <= before.max_core_steps &&
           after.max_dynamic_compiles <= before.max_dynamic_compiles &&
           after.max_child_depth <= before.max_child_depth &&
           after.max_total_children <= before.max_total_children;
}

bool budget_fits(const RunBudget& request, const RunBudget& available) {
    return request.wall_time_ms <= available.wall_time_ms &&
           request.model_tokens <= available.model_tokens &&
           request.monetary_microunits <= available.monetary_microunits &&
           request.max_concurrency <= available.max_concurrency &&
           request.max_program_operations <= available.max_program_operations &&
           request.max_core_steps <= available.max_core_steps &&
           request.max_dynamic_compiles <= available.max_dynamic_compiles &&
           request.max_child_depth <= available.max_child_depth &&
           request.max_total_children <= available.max_total_children;
}

ProgramBudgetBounds successor_budget_bounds(const RunBudget& reserved_remainder) {
    // A replacement must transfer the exact lineage remainder. These are the
    // smallest structural grants that can activate an ordinary JavaScript
    // Program; all consumable authority and child grants remain at the
    // reservation-derived ceiling. This never replenishes a debited unit.
    RunBudget structural_floor;
    structural_floor.wall_time_ms           = 1;
    structural_floor.max_concurrency        = 1;
    structural_floor.max_program_operations = 1;
    structural_floor.max_core_steps         = 1;
    return {structural_floor, reserved_remainder};
}

std::string required_string(const json& value, std::string_view name) {
    const std::string key(name);
    if (!value.contains(key) || !value.at(key).is_string()) {
        throw std::invalid_argument("Stored synthesis field '" + key +
                                    "' must be a string");
    }
    return value.at(key).get<std::string>();
}

std::vector<std::string> string_array(const json& value, std::string_view name) {
    const std::string key(name);
    if (!value.contains(key) || !value.at(key).is_array()) {
        throw std::invalid_argument("Stored synthesis field '" + key +
                                    "' must be an array");
    }
    std::vector<std::string> result;
    for (const auto& item : value.at(key)) {
        if (!item.is_string()) throw std::invalid_argument("Stored synthesis token is invalid");
        result.push_back(item.get<std::string>());
    }
    return result;
}

json proposal_body(const ProgramSynthesisProposalData& data) {
    return {{"format", "neograph-program-synthesis-proposal"},
            {"storage_schema_version", 1},
            {"owner_scope", data.owner_scope},
            {"lineage_id", data.lineage_id},
            {"parent_run_id", data.parent_run_id},
            {"instruction_id", data.instruction_id ? json(*data.instruction_id)
                                                    : json(nullptr)},
            {"source", data.source->serialize_canonical()},
            {"requested_capabilities", data.requested_capabilities},
            {"requested_effects", data.requested_effects},
            {"requested_budget", budget_json(data.requested_budget)},
            {"created_at_ms", data.created_at_ms}};
}

json reservation_body(const ProgramSynthesisReservationData& data) {
    return {{"format", "neograph-program-synthesis-reservation"},
            {"storage_schema_version", 1},
            {"proposal_id", data.proposal_id},
            {"lineage_id", data.lineage_id},
            {"source_lineage_head_id", data.source_lineage_head_id},
            {"reserved_lineage_head_id", data.reserved_lineage_head_id},
            {"source_remaining", budget_json(data.source_remaining)},
            {"remaining_after_reservation",
             budget_json(data.remaining_after_reservation)}};
}

json receipt_body(const ProgramSynthesisReceiptData& data) {
    return {{"format", "neograph-program-synthesis-receipt"},
            {"storage_schema_version", 1},
            {"proposal_id", data.proposal_id},
            {"reservation_id", data.reservation_id},
            {"bundle_id", data.bundle_id},
            {"program_version_id", data.program_version_id},
            {"policy_snapshot_fingerprint", data.policy_snapshot_fingerprint}};
}

json validation_receipt_body(const ProgramSynthesisValidationReceiptData& data) {
    return {{"format", "neograph-program-synthesis-validation-receipt"},
            {"storage_schema_version", 1},
            {"proposal_id", data.proposal_id},
            {"reservation_id", data.reservation_id},
            {"bundle_id", data.bundle_id},
            {"validator_identity", data.validator_identity},
            {"contract_identity", data.contract_identity},
            {"accepted", data.accepted},
            {"evidence_digest", data.evidence_digest}};
}

}  // namespace

struct ProgramSynthesisProposal::Impl {
    ProgramSynthesisProposalData data;
    std::string id;
    std::string canonical;
};

ProgramSynthesisProposal::ProgramSynthesisProposal(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

ProgramSynthesisProposal ProgramSynthesisProposal::create(
    ProgramSynthesisProposalData data) {
    require_token(data.owner_scope, "Synthesis proposal owner_scope");
    require_sha(data.lineage_id, "Synthesis proposal lineage_id");
    require_token(data.parent_run_id, "Synthesis proposal parent_run_id");
    if (data.instruction_id) require_sha(*data.instruction_id, "Synthesis proposal instruction_id");
    if (!data.source || data.source->kind() != SourceKind::JavaScript ||
        data.created_at_ms < 0) {
        throw std::invalid_argument(
            "Synthesis proposal requires sealed JavaScript and valid time");
    }
    normalize_tokens(data.requested_capabilities,
                     "Synthesis proposal requested_capabilities");
    normalize_tokens(data.requested_effects,
                     "Synthesis proposal requested_effects");
    auto body = proposal_body(data);
    auto impl = std::make_shared<Impl>();
    impl->id = detail::sha256_identity("program-synthesis-proposal/v1",
                                       detail::canonical_json_bytes(body));
    body["id"] = impl->id;
    impl->data = std::move(data);
    impl->canonical = detail::canonical_json_bytes(body);
    return ProgramSynthesisProposal(std::move(impl));
}

ProgramSynthesisProposal ProgramSynthesisProposal::parse(std::string_view bytes) {
    const auto value = detail::parse_json_strict(bytes);
    detail::reject_unknown_fields(
        value, "Stored ProgramSynthesisProposal",
        {"format", "storage_schema_version", "id", "owner_scope", "lineage_id",
         "parent_run_id", "instruction_id", "source", "requested_capabilities",
         "requested_effects", "requested_budget", "created_at_ms"});
    if (required_string(value, "format") != "neograph-program-synthesis-proposal" ||
        value.at("storage_schema_version").get<std::uint32_t>() != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored ProgramSynthesisProposal is unsupported");
    }
    const auto stored_id = required_string(value, "id");
    ProgramSynthesisProposalData data;
    data.owner_scope = required_string(value, "owner_scope");
    data.lineage_id = required_string(value, "lineage_id");
    data.parent_run_id = required_string(value, "parent_run_id");
    if (!value.at("instruction_id").is_null())
        data.instruction_id = value.at("instruction_id").get<std::string>();
    data.source = ProgramSource::parse(required_string(value, "source"));
    data.requested_capabilities = string_array(value, "requested_capabilities");
    data.requested_effects = string_array(value, "requested_effects");
    data.requested_budget = budget_from(value.at("requested_budget"));
    data.created_at_ms = value.at("created_at_ms").get<std::int64_t>();
    auto result = create(std::move(data));
    if (result.id() != stored_id) throw std::invalid_argument("Stored synthesis proposal id mismatch");
    return result;
}

const ProgramSynthesisProposalData& ProgramSynthesisProposal::data() const noexcept { return impl_->data; }
const ProgramSource& ProgramSynthesisProposal::source() const noexcept { return *impl_->data.source; }
const std::string& ProgramSynthesisProposal::id() const noexcept { return impl_->id; }
std::string ProgramSynthesisProposal::serialize_canonical() const { return impl_->canonical; }

struct ProgramSynthesisReservation::Impl {
    ProgramSynthesisReservationData data;
    std::string id;
    std::string canonical;
};

ProgramSynthesisReservation::ProgramSynthesisReservation(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

ProgramSynthesisReservation ProgramSynthesisReservation::create(
    ProgramSynthesisReservationData data) {
    require_sha(data.proposal_id, "Synthesis reservation proposal_id");
    require_sha(data.lineage_id, "Synthesis reservation lineage_id");
    require_sha(data.source_lineage_head_id, "Synthesis reservation source head");
    require_sha(data.reserved_lineage_head_id, "Synthesis reservation target head");
    if (data.source_lineage_head_id == data.reserved_lineage_head_id ||
        !data.source_remaining.max_dynamic_compiles ||
        data.remaining_after_reservation.max_dynamic_compiles + 1 !=
            data.source_remaining.max_dynamic_compiles ||
        !no_budget_increase(data.source_remaining,
                            data.remaining_after_reservation)) {
        throw std::invalid_argument(
            "Synthesis reservation does not debit exactly one nonrenewable compile");
    }
    auto body = reservation_body(data);
    auto impl = std::make_shared<Impl>();
    impl->id = detail::sha256_identity("program-synthesis-reservation/v1",
                                       detail::canonical_json_bytes(body));
    body["id"] = impl->id;
    impl->data = std::move(data);
    impl->canonical = detail::canonical_json_bytes(body);
    return ProgramSynthesisReservation(std::move(impl));
}

ProgramSynthesisReservation ProgramSynthesisReservation::parse(std::string_view bytes) {
    const auto value = detail::parse_json_strict(bytes);
    detail::reject_unknown_fields(
        value, "Stored ProgramSynthesisReservation",
        {"format", "storage_schema_version", "id", "proposal_id", "lineage_id",
         "source_lineage_head_id", "reserved_lineage_head_id", "source_remaining",
         "remaining_after_reservation"});
    if (required_string(value, "format") != "neograph-program-synthesis-reservation" ||
        value.at("storage_schema_version").get<std::uint32_t>() != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored ProgramSynthesisReservation is unsupported");
    }
    const auto stored_id = required_string(value, "id");
    ProgramSynthesisReservationData data;
    data.proposal_id = required_string(value, "proposal_id");
    data.lineage_id = required_string(value, "lineage_id");
    data.source_lineage_head_id = required_string(value, "source_lineage_head_id");
    data.reserved_lineage_head_id = required_string(value, "reserved_lineage_head_id");
    data.source_remaining = budget_from(value.at("source_remaining"));
    data.remaining_after_reservation = budget_from(value.at("remaining_after_reservation"));
    auto result = create(std::move(data));
    if (result.id() != stored_id) throw std::invalid_argument("Stored synthesis reservation id mismatch");
    return result;
}

const ProgramSynthesisReservationData& ProgramSynthesisReservation::data() const noexcept { return impl_->data; }
const std::string& ProgramSynthesisReservation::id() const noexcept { return impl_->id; }
std::string ProgramSynthesisReservation::serialize_canonical() const { return impl_->canonical; }

struct ProgramSynthesisReceipt::Impl {
    ProgramSynthesisReceiptData data;
    std::string id;
    std::string canonical;
};

ProgramSynthesisReceipt::ProgramSynthesisReceipt(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

ProgramSynthesisReceipt ProgramSynthesisReceipt::create(ProgramSynthesisReceiptData data) {
    require_sha(data.proposal_id, "Synthesis receipt proposal_id");
    require_sha(data.reservation_id, "Synthesis receipt reservation_id");
    require_sha(data.bundle_id, "Synthesis receipt bundle_id");
    require_sha(data.program_version_id, "Synthesis receipt program_version_id");
    require_sha(data.policy_snapshot_fingerprint,
                "Synthesis receipt policy_snapshot_fingerprint");
    auto body = receipt_body(data);
    auto impl = std::make_shared<Impl>();
    impl->id = detail::sha256_identity("program-synthesis-receipt/v1",
                                       detail::canonical_json_bytes(body));
    body["id"] = impl->id;
    impl->data = std::move(data);
    impl->canonical = detail::canonical_json_bytes(body);
    return ProgramSynthesisReceipt(std::move(impl));
}

ProgramSynthesisReceipt ProgramSynthesisReceipt::parse(std::string_view bytes) {
    const auto value = detail::parse_json_strict(bytes);
    detail::reject_unknown_fields(
        value, "Stored ProgramSynthesisReceipt",
        {"format", "storage_schema_version", "id", "proposal_id", "reservation_id",
         "bundle_id", "program_version_id", "policy_snapshot_fingerprint"});
    if (required_string(value, "format") != "neograph-program-synthesis-receipt" ||
        value.at("storage_schema_version").get<std::uint32_t>() != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored ProgramSynthesisReceipt is unsupported");
    }
    const auto stored_id = required_string(value, "id");
    ProgramSynthesisReceiptData data{required_string(value, "proposal_id"),
                                     required_string(value, "reservation_id"),
                                     required_string(value, "bundle_id"),
                                     required_string(value, "program_version_id"),
                                     required_string(value, "policy_snapshot_fingerprint")};
    auto result = create(std::move(data));
    if (result.id() != stored_id) throw std::invalid_argument("Stored synthesis receipt id mismatch");
    return result;
}

const ProgramSynthesisReceiptData& ProgramSynthesisReceipt::data() const noexcept { return impl_->data; }
const std::string& ProgramSynthesisReceipt::id() const noexcept { return impl_->id; }
std::string ProgramSynthesisReceipt::serialize_canonical() const { return impl_->canonical; }

struct ProgramSynthesisValidationReceipt::Impl {
    ProgramSynthesisValidationReceiptData data;
    std::string id;
    std::string canonical;
};

ProgramSynthesisValidationReceipt::ProgramSynthesisValidationReceipt(
    std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

ProgramSynthesisValidationReceipt ProgramSynthesisValidationReceipt::create(
    ProgramSynthesisValidationReceiptData data) {
    require_sha(data.proposal_id, "Synthesis validation proposal_id");
    require_sha(data.reservation_id, "Synthesis validation reservation_id");
    require_sha(data.bundle_id, "Synthesis validation bundle_id");
    require_sha(data.validator_identity, "Synthesis validation validator_identity");
    require_sha(data.contract_identity, "Synthesis validation contract_identity");
    require_sha(data.evidence_digest, "Synthesis validation evidence_digest");
    auto body = validation_receipt_body(data);
    auto impl = std::make_shared<Impl>();
    impl->id = detail::sha256_identity("program-synthesis-validation-receipt/v1",
                                       detail::canonical_json_bytes(body));
    body["id"] = impl->id;
    impl->data = std::move(data);
    impl->canonical = detail::canonical_json_bytes(body);
    return ProgramSynthesisValidationReceipt(std::move(impl));
}

ProgramSynthesisValidationReceipt ProgramSynthesisValidationReceipt::parse(
    std::string_view bytes) {
    const auto value = detail::parse_json_strict(bytes);
    detail::reject_unknown_fields(
        value, "Stored ProgramSynthesisValidationReceipt",
        {"format", "storage_schema_version", "id", "proposal_id", "reservation_id",
         "bundle_id", "validator_identity", "contract_identity", "accepted",
         "evidence_digest"});
    if (required_string(value, "format") !=
            "neograph-program-synthesis-validation-receipt" ||
        value.at("storage_schema_version").get<std::uint32_t>() != STORAGE_SCHEMA_VERSION ||
        !value.at("accepted").is_boolean()) {
        throw std::invalid_argument("Stored ProgramSynthesisValidationReceipt is unsupported");
    }
    const auto stored_id = required_string(value, "id");
    ProgramSynthesisValidationReceiptData data{
        required_string(value, "proposal_id"),
        required_string(value, "reservation_id"),
        required_string(value, "bundle_id"),
        required_string(value, "validator_identity"),
        required_string(value, "contract_identity"),
        value.at("accepted").get<bool>(),
        required_string(value, "evidence_digest")};
    auto result = create(std::move(data));
    if (result.id() != stored_id)
        throw std::invalid_argument("Stored synthesis validation receipt id mismatch");
    return result;
}

const ProgramSynthesisValidationReceiptData&
ProgramSynthesisValidationReceipt::data() const noexcept {
    return impl_->data;
}
const std::string& ProgramSynthesisValidationReceipt::id() const noexcept {
    return impl_->id;
}
std::string ProgramSynthesisValidationReceipt::serialize_canonical() const {
    return impl_->canonical;
}

void validate_program_synthesis_validation_evidence(
    const ProgramSynthesisValidationReceipt& receipt,
    const json& evidence) {
    const auto digest = detail::sha256_identity(
        "program-synthesis-semantic-evidence/v1",
        detail::canonical_json_bytes(evidence));
    if (receipt.data().evidence_digest != digest) {
        throw std::invalid_argument(
            "Program synthesis semantic evidence does not match its receipt");
    }
}

ProgramSynthesisValidationError::ProgramSynthesisValidationError(
    ProgramSynthesisValidationReceipt receipt, json evidence)
    : std::runtime_error("Compiled Program failed host semantic validation"),
      receipt_(std::move(receipt)),
      evidence_(detail::owned_json_copy(evidence)) {
    validate_program_synthesis_validation_evidence(receipt_, evidence_);
}

const ProgramSynthesisValidationReceipt&
ProgramSynthesisValidationError::receipt() const noexcept {
    return receipt_;
}
const json& ProgramSynthesisValidationError::evidence() const noexcept {
    return evidence_;
}

ProgramSynthesisGateway::ProgramSynthesisGateway(ProgramSynthesisGatewayConfig config)
    : config_(std::move(config)) {
    if (!config_.compiler || !config_.catalog || !config_.reserve ||
        !config_.admission || !config_.max_source_bytes || !config_.validate_semantics ||
        !config_.max_semantic_evidence_bytes) {
        throw std::invalid_argument(
            "Program synthesis gateway requires compiler, Catalog, reservation, semantic validation, admission, and limits");
    }
}

ProgramSynthesisResult ProgramSynthesisGateway::synthesize(
    const ProgramSynthesisProposal& proposal) const {
    if (proposal.source().serialize_canonical().size() > config_.max_source_bytes) {
        throw std::invalid_argument("Program synthesis source exceeds host byte limit");
    }
    auto reservation = config_.reserve(proposal);
    if (reservation.data().proposal_id != proposal.id() ||
        reservation.data().lineage_id != proposal.data().lineage_id ||
        !budget_fits(proposal.data().requested_budget,
                     reservation.data().remaining_after_reservation)) {
        throw std::runtime_error(
            "Program synthesis reservation does not bind the proposal or budget");
    }
    auto bundle = config_.compiler->compile(
        proposal.source(), successor_budget_bounds(reservation.data().remaining_after_reservation));
    if (bundle.source_hash() != proposal.source().source_hash() ||
        !std::includes(proposal.data().requested_capabilities.begin(),
                       proposal.data().requested_capabilities.end(),
                       bundle.capability_effect_closure().capabilities.begin(),
                       bundle.capability_effect_closure().capabilities.end()) ||
        !std::includes(proposal.data().requested_effects.begin(),
                       proposal.data().requested_effects.end(),
                       bundle.capability_effect_closure().effects.begin(),
                       bundle.capability_effect_closure().effects.end())) {
        throw std::runtime_error(
            "Compiled Program exceeds its synthesis proposal closure");
    }
    auto semantic = config_.validate_semantics(proposal, bundle, reservation);
    require_sha(semantic.validator_identity,
                "Synthesis semantic validator_identity");
    require_sha(semantic.contract_identity,
                "Synthesis semantic contract_identity");
    const auto semantic_evidence = detail::canonical_json_bytes(semantic.evidence);
    if (semantic_evidence.size() > config_.max_semantic_evidence_bytes) {
        throw std::runtime_error("Program synthesis semantic evidence exceeds host byte limit");
    }
    auto validation = ProgramSynthesisValidationReceipt::create(
        {proposal.id(), reservation.id(), bundle.id(), semantic.validator_identity,
         semantic.contract_identity, semantic.accepted,
         detail::sha256_identity("program-synthesis-semantic-evidence/v1",
                                 semantic_evidence)});
    if (!semantic.accepted) {
        throw ProgramSynthesisValidationError(std::move(validation),
                                              std::move(semantic.evidence));
    }
    auto admission = config_.admission(proposal, bundle, reservation);
    if (admission.owner_scope != proposal.data().owner_scope) {
        throw std::runtime_error("Program synthesis admission changed owner scope");
    }
    auto version = config_.catalog->admit(bundle, std::move(admission));
    auto receipt = ProgramSynthesisReceipt::create(
        {proposal.id(), reservation.id(), bundle.id(), version.id(),
         version.policy_snapshot().fingerprint()});
    return {std::move(reservation), std::move(bundle), std::move(validation),
            std::move(semantic.evidence), std::move(version), std::move(receipt)};
}

}  // namespace neograph::program
