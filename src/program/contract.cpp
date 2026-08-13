#include <neograph/program/contract.h>

#include "canonical_json.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

using detail::canonical_json_bytes;
using detail::parse_json_strict;
using detail::sha256_identity;
using detail::validate_token;
using detail::validate_utf8;

std::string string_value(const json& object, std::string_view field) {
    const auto key = std::string(field);
    if (!object.contains(key) || !object[key].is_string()) {
        throw std::invalid_argument("Contract field '" + key + "' must be a string");
    }
    return object[key].get<std::string>();
}

std::uint64_t unsigned_value(const json& object, std::string_view field) {
    const auto key = std::string(field);
    if (!object.contains(key) || !object[key].is_number_unsigned()) {
        throw std::invalid_argument("Contract field '" + key + "' must be unsigned");
    }
    return object[key].get<std::uint64_t>();
}

bool bool_value(const json& object, std::string_view field) {
    const auto key = std::string(field);
    if (!object.contains(key) || !object[key].is_boolean()) {
        throw std::invalid_argument("Contract field '" + key + "' must be boolean");
    }
    return object[key].get<bool>();
}

json required_object(const json& object, std::string_view field) {
    const auto key = std::string(field);
    if (!object.contains(key) || !object[key].is_object()) {
        throw std::invalid_argument("Contract field '" + key + "' must be an object");
    }
    return object[key];
}

json required_array(const json& object, std::string_view field) {
    const auto key = std::string(field);
    if (!object.contains(key) || !object[key].is_array()) {
        throw std::invalid_argument("Contract field '" + key + "' must be an array");
    }
    return object[key];
}

std::vector<std::string> strings_from(const json& values, std::string_view field) {
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        if (!value.is_string()) {
            throw std::invalid_argument("Contract array '" + std::string(field) +
                                        "' must contain strings");
        }
        result.push_back(value.get<std::string>());
    }
    return result;
}

json strings_to(const std::vector<std::string>& values) {
    json result = json::array();
    for (const auto& value : values) {
        result.push_back(value);
    }
    return result;
}

json item_to_json(const ContractItem& item) {
    return json{{"id", item.id}, {"description", item.description}};
}

ContractItem item_from_json(const json& value) {
    detail::reject_unknown_fields(value, "contract.item", {"id", "description"});
    return ContractItem{string_value(value, "id"), string_value(value, "description")};
}

json acceptance_to_json(const ContractAcceptance& acceptance) {
    return json{{"id", acceptance.id},
                {"description", acceptance.description},
                {"required", acceptance.required},
                {"expected", acceptance.expected}};
}

ContractAcceptance acceptance_from_json(const json& value) {
    detail::reject_unknown_fields(
        value, "contract.acceptance", {"id", "description", "required", "expected"});
    return ContractAcceptance{string_value(value, "id"),
                              string_value(value, "description"),
                              bool_value(value, "required"),
                              value.contains("expected") ? value["expected"] : json::object()};
}

json risk_to_json(const ContractRisk& risk) {
    return json{{"id", risk.id},
                {"description", risk.description},
                {"mitigation", risk.mitigation},
                {"blocking", risk.blocking}};
}

ContractRisk risk_from_json(const json& value) {
    detail::reject_unknown_fields(
        value, "contract.risk", {"id", "description", "mitigation", "blocking"});
    return ContractRisk{string_value(value, "id"),
                        string_value(value, "description"),
                        string_value(value, "mitigation"),
                        bool_value(value, "blocking")};
}

json vector_to_json(const ContractTestVector& vector) {
    return json{{"id", vector.id}, {"input", vector.input}, {"expected", vector.expected}};
}

ContractTestVector vector_from_json(const json& value) {
    detail::reject_unknown_fields(value, "contract.test_vector", {"id", "input", "expected"});
    return ContractTestVector{string_value(value, "id"),
                              value.contains("input") ? value["input"] : json::object(),
                              value.contains("expected") ? value["expected"] : json::object()};
}

json retry_to_json(const ContractRetryPolicy& policy) {
    return json{{"max_attempts", policy.max_attempts},
                {"max_program_operations", policy.max_program_operations},
                {"max_wall_time_ms", policy.max_wall_time_ms}};
}

ContractRetryPolicy retry_from_json(const json& value) {
    detail::reject_unknown_fields(value,
                                  "contract.retry_policy",
                                  {"max_attempts", "max_program_operations", "max_wall_time_ms"});
    ContractRetryPolicy result;
    result.max_attempts = static_cast<std::uint32_t>(unsigned_value(value, "max_attempts"));
    result.max_program_operations = unsigned_value(value, "max_program_operations");
    result.max_wall_time_ms = unsigned_value(value, "max_wall_time_ms");
    return result;
}

json spec_to_json(const ContractManifestSpec& spec) {
    json requirements = json::array();
    for (const auto& item : spec.requirements) {
        requirements.push_back(item_to_json(item));
    }
    json acceptance = json::array();
    for (const auto& item : spec.acceptance) {
        acceptance.push_back(acceptance_to_json(item));
    }
    json vectors = json::array();
    for (const auto& item : spec.fixed_test_vectors) {
        vectors.push_back(vector_to_json(item));
    }
    json risks = json::array();
    for (const auto& item : spec.risk_register) {
        risks.push_back(risk_to_json(item));
    }
    return json{{"schema_version", spec.schema_version},
                {"manifest_id", spec.manifest_id},
                {"owner_scope", spec.owner_scope},
                {"scope", spec.scope},
                {"assumptions", strings_to(spec.assumptions)},
                {"requirements", requirements},
                {"non_goals", strings_to(spec.non_goals)},
                {"acceptance", acceptance},
                {"fixed_test_vectors", vectors},
                {"independent_oracles", strings_to(spec.independent_oracles)},
                {"risk_register", risks},
                {"permissions", strings_to(spec.permissions)},
                {"retry_policy", retry_to_json(spec.retry_policy)}};
}

ContractManifestSpec spec_from_json(const json& value) {
    detail::reject_unknown_fields(value,
                                  "contract.manifest_spec",
                                  {"schema_version",
                                   "manifest_id",
                                   "owner_scope",
                                   "scope",
                                   "assumptions",
                                   "requirements",
                                   "non_goals",
                                   "acceptance",
                                   "fixed_test_vectors",
                                   "independent_oracles",
                                   "risk_register",
                                   "permissions",
                                   "retry_policy"});
    ContractManifestSpec result;
    result.schema_version = static_cast<std::uint32_t>(unsigned_value(value, "schema_version"));
    result.manifest_id = string_value(value, "manifest_id");
    result.owner_scope = string_value(value, "owner_scope");
    result.scope = string_value(value, "scope");
    result.assumptions = strings_from(required_array(value, "assumptions"), "assumptions");
    for (const auto& item : required_array(value, "requirements")) {
        result.requirements.push_back(item_from_json(item));
    }
    result.non_goals = strings_from(required_array(value, "non_goals"), "non_goals");
    for (const auto& item : required_array(value, "acceptance")) {
        result.acceptance.push_back(acceptance_from_json(item));
    }
    for (const auto& item : required_array(value, "fixed_test_vectors")) {
        result.fixed_test_vectors.push_back(vector_from_json(item));
    }
    result.independent_oracles =
        strings_from(required_array(value, "independent_oracles"), "independent_oracles");
    for (const auto& item : required_array(value, "risk_register")) {
        result.risk_register.push_back(risk_from_json(item));
    }
    result.permissions = strings_from(required_array(value, "permissions"), "permissions");
    result.retry_policy = retry_from_json(required_object(value, "retry_policy"));
    return result;
}

json review_to_json(const std::optional<ContractReview>& review) {
    if (!review) {
        return nullptr;
    }
    return json{{"reviewer_id", review->reviewer_id},
                {"notes", review->notes},
                {"approved", review->approved}};
}

std::optional<ContractReview> review_from_json(const json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    detail::reject_unknown_fields(value, "contract.review", {"reviewer_id", "notes", "approved"});
    return ContractReview{string_value(value, "reviewer_id"),
                          string_value(value, "notes"),
                          bool_value(value, "approved")};
}

void validate_identifier(std::string_view value, std::string_view field) {
    if (value.empty()) {
        throw std::invalid_argument("Contract field '" + std::string(field) + "' must not be empty");
    }
    validate_utf8(value);
    validate_token(value, field);
}

template <typename Item>
void validate_unique_ids(const std::vector<Item>& values, std::string_view field) {
    std::set<std::string> ids;
    for (const auto& value : values) {
        validate_identifier(value.id, field);
        if (value.description.empty()) {
            throw std::invalid_argument("Contract " + std::string(field) + " description must not be empty");
        }
        if (!ids.insert(value.id).second) {
            throw std::invalid_argument("Contract " + std::string(field) + " contains duplicate id");
        }
    }
}

void validate_spec(const ContractManifestSpec& spec) {
    if (spec.schema_version != ContractManifest::STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Unsupported contract manifest schema version");
    }
    validate_identifier(spec.manifest_id, "manifest_id");
    validate_identifier(spec.owner_scope, "owner_scope");
    if (spec.scope.empty()) {
        throw std::invalid_argument("Contract scope must not be empty");
    }
    validate_utf8(spec.scope);
    if (spec.requirements.empty()) {
        throw std::invalid_argument("Contract must declare at least one requirement");
    }
    if (spec.acceptance.empty()) {
        throw std::invalid_argument("Contract must declare at least one acceptance gate");
    }
    validate_unique_ids(spec.requirements, "requirement");
    validate_unique_ids(spec.acceptance, "acceptance");
    {
        std::set<std::string> ids;
        for (const auto& value : spec.fixed_test_vectors) {
            validate_identifier(value.id, "test_vector");
            if (!ids.insert(value.id).second) {
                throw std::invalid_argument("Contract test_vector contains duplicate id");
            }
        }
    }
    for (const auto& value : spec.assumptions) {
        if (value.empty()) {
            throw std::invalid_argument("Contract assumptions must not be empty");
        }
        validate_utf8(value);
    }
    for (const auto& value : spec.non_goals) {
        if (value.empty()) {
            throw std::invalid_argument("Contract non-goals must not be empty");
        }
        validate_utf8(value);
    }
    for (const auto& value : spec.independent_oracles) {
        validate_identifier(value, "independent_oracle");
    }
    std::set<std::string> oracle_ids(spec.independent_oracles.begin(), spec.independent_oracles.end());
    if (oracle_ids.size() != spec.independent_oracles.size()) {
        throw std::invalid_argument("Contract independent_oracles contains duplicate id");
    }
    validate_unique_ids(spec.risk_register, "risk");
    for (const auto& value : spec.permissions) {
        validate_identifier(value, "permission");
    }
    if (spec.retry_policy.max_attempts == 0 || spec.retry_policy.max_program_operations == 0 ||
        spec.retry_policy.max_wall_time_ms == 0) {
        throw std::invalid_argument("Contract retry policy limits must be positive");
    }
}

json manifest_payload(const ContractManifestSpec& spec,
                      ContractManifestLifecycle lifecycle,
                      const std::optional<ContractReview>& review) {
    return json{{"storage_schema_version", ContractManifest::STORAGE_SCHEMA_VERSION},
                {"lifecycle", std::string(to_string(lifecycle))},
                {"spec", spec_to_json(spec)},
                {"review", review_to_json(review)}};
}

std::string manifest_hash(const ContractManifestSpec& spec,
                          ContractManifestLifecycle lifecycle,
                          const std::optional<ContractReview>& review) {
    return sha256_identity("neograph-program-contract-manifest",
                           canonical_json_bytes(manifest_payload(spec, lifecycle, review)));
}

json manifest_to_json(const ContractManifestSpec& spec,
                      ContractManifestLifecycle lifecycle,
                      const std::optional<ContractReview>& review,
                      std::string_view hash) {
    auto result = manifest_payload(spec, lifecycle, review);
    result["content_hash"] = std::string(hash);
    return result;
}

ContractEvidence evidence_from_json(const json& value) {
    detail::reject_unknown_fields(
        value,
        "contract.evidence",
        {"evidence_id",
         "acceptance_id",
         "kind",
         "manifest_hash",
         "program_version_id",
         "run_id",
         "workspace_revision",
         "command",
         "toolchain",
         "artifact_hash",
         "executed",
         "passed",
         "diagnostics",
         "details"});
    return ContractEvidence{string_value(value, "evidence_id"),
                            string_value(value, "acceptance_id"),
                            contract_evidence_kind_from_string(string_value(value, "kind")),
                            string_value(value, "manifest_hash"),
                            string_value(value, "program_version_id"),
                            string_value(value, "run_id"),
                            string_value(value, "workspace_revision"),
                            string_value(value, "command"),
                            string_value(value, "toolchain"),
                            string_value(value, "artifact_hash"),
                            bool_value(value, "executed"),
                            bool_value(value, "passed"),
                            strings_from(required_array(value, "diagnostics"), "diagnostics"),
                            value.contains("details") ? value["details"] : json::object()};
}

json evidence_to_json(const ContractEvidence& value) {
    return json{{"evidence_id", value.evidence_id},
                {"acceptance_id", value.acceptance_id},
                {"kind", std::string(to_string(value.kind))},
                {"manifest_hash", value.manifest_hash},
                {"program_version_id", value.program_version_id},
                {"run_id", value.run_id},
                {"workspace_revision", value.workspace_revision},
                {"command", value.command},
                {"toolchain", value.toolchain},
                {"artifact_hash", value.artifact_hash},
                {"executed", value.executed},
                {"passed", value.passed},
                {"diagnostics", strings_to(value.diagnostics)},
                {"details", value.details}};
}
void validate_loaded_evidence(const ContractEvidence& evidence,
                              const ContractManifest& manifest,
                              std::set<std::string>& seen_ids) {
    validate_identifier(evidence.evidence_id, "evidence_id");
    if (!seen_ids.insert(evidence.evidence_id).second) {
        throw std::invalid_argument("Contract run contains duplicate evidence_id");
    }
    if (evidence.manifest_hash != manifest.content_hash()) {
        throw std::invalid_argument("Contract run evidence hash mismatch");
    }
    if (!evidence.executed) {
        throw std::invalid_argument("Persisted contract evidence must be marked executed");
    }
    for (const auto& diagnostic : evidence.diagnostics) validate_utf8(diagnostic);
    (void)canonical_json_bytes(evidence.details);

    if (evidence.kind == ContractEvidenceKind::WorkerReport) {
        return;
    }
    if (evidence.program_version_id.empty() || evidence.run_id.empty() ||
        evidence.workspace_revision.empty() || evidence.command.empty() ||
        evidence.toolchain.empty() || evidence.artifact_hash.empty()) {
        throw std::invalid_argument(
            "Persisted contract evidence must identify version, run, workspace, command, "
            "toolchain, and artifact");
    }
    validate_utf8(evidence.program_version_id);
    validate_utf8(evidence.run_id);
    validate_utf8(evidence.workspace_revision);
    validate_utf8(evidence.command);
    validate_utf8(evidence.toolchain);
    validate_utf8(evidence.artifact_hash);

    if (evidence.kind == ContractEvidenceKind::IndependentOracle) {
        if (!evidence.acceptance_id.empty()) {
            throw std::invalid_argument(
                "Persisted independent oracle evidence cannot target an acceptance gate");
        }
        if (!evidence.details.is_object() || !evidence.details.contains("oracle_id") ||
            !evidence.details["oracle_id"].is_string()) {
            throw std::invalid_argument(
                "Persisted independent oracle evidence must identify its oracle");
        }
        const auto oracle_id = evidence.details["oracle_id"].get<std::string>();
        if (std::find(manifest.spec().independent_oracles.begin(),
                      manifest.spec().independent_oracles.end(),
                      oracle_id) == manifest.spec().independent_oracles.end()) {
            throw std::invalid_argument(
                "Persisted contract evidence names an unknown independent oracle");
        }
        return;
    }

    if (evidence.acceptance_id.empty()) {
        throw std::invalid_argument(
            "Persisted deterministic evidence must target an acceptance gate");
    }
    const auto acceptance = std::find_if(
        manifest.spec().acceptance.begin(), manifest.spec().acceptance.end(),
        [&](const auto& item) { return item.id == evidence.acceptance_id; });
    if (acceptance == manifest.spec().acceptance.end()) {
        throw std::invalid_argument(
            "Persisted contract evidence names an unknown acceptance gate");
    }
}


void validate_loaded_diagnostic(const ContractDiagnostic& diagnostic) {
    validate_identifier(diagnostic.code, "diagnostic.code");
    if (diagnostic.message.empty()) {
        throw std::invalid_argument("Persisted contract diagnostic message must not be empty");
    }
    validate_utf8(diagnostic.message);
}

ContractDiagnostic diagnostic_from_json(const json& value) {
    detail::reject_unknown_fields(value, "contract.diagnostic", {"code", "message", "blocking"});
    return ContractDiagnostic{string_value(value, "code"),
                              string_value(value, "message"),
                              bool_value(value, "blocking")};
}

json diagnostic_to_json(const ContractDiagnostic& value) {
    return json{{"code", value.code}, {"message", value.message}, {"blocking", value.blocking}};
}

json verification_to_json(const std::optional<ContractVerification>& verification) {
    if (!verification) {
        return nullptr;
    }
    return json{{"publishable", verification->publishable},
                {"status", std::string(to_string(verification->status))},
                {"missing_acceptance_ids", strings_to(verification->missing_acceptance_ids)},
                {"blocking_diagnostics", strings_to(verification->blocking_diagnostics)},
                {"failed_evidence_ids", strings_to(verification->failed_evidence_ids)}};
}

ContractVerification verification_from_json(const json& value) {
    detail::reject_unknown_fields(value,
                                  "contract.verification",
                                  {"publishable",
                                   "status",
                                   "missing_acceptance_ids",
                                   "blocking_diagnostics",
                                   "failed_evidence_ids"});
    return ContractVerification{bool_value(value, "publishable"),
                                contract_run_status_from_string(string_value(value, "status")),
                                strings_from(required_array(value, "missing_acceptance_ids"),
                                             "missing_acceptance_ids"),
                                strings_from(required_array(value, "blocking_diagnostics"),
                                             "blocking_diagnostics"),
                                strings_from(required_array(value, "failed_evidence_ids"),
                                             "failed_evidence_ids")};
}

}  // namespace

std::string_view to_string(ContractManifestLifecycle state) noexcept {
    switch (state) {
        case ContractManifestLifecycle::Proposed:
            return "proposed";
        case ContractManifestLifecycle::Reviewed:
            return "reviewed";
        case ContractManifestLifecycle::Frozen:
            return "frozen";
    }
    return "unknown";
}

ContractManifestLifecycle contract_manifest_lifecycle_from_string(std::string_view value) {
    if (value == "proposed") {
        return ContractManifestLifecycle::Proposed;
    }
    if (value == "reviewed") {
        return ContractManifestLifecycle::Reviewed;
    }
    if (value == "frozen") {
        return ContractManifestLifecycle::Frozen;
    }
    throw std::invalid_argument("Unknown contract manifest lifecycle");
}

struct ContractManifest::Impl {
    ContractManifestSpec spec;
    ContractManifestLifecycle lifecycle = ContractManifestLifecycle::Proposed;
    std::optional<ContractReview> review;
    std::string content_hash;
};

ContractManifest::ContractManifest(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

ContractManifest ContractManifest::propose(ContractManifestSpec spec) {
    validate_spec(spec);
    auto impl = std::make_shared<Impl>();
    impl->spec = std::move(spec);
    impl->content_hash = manifest_hash(impl->spec, impl->lifecycle, impl->review);
    return ContractManifest(std::move(impl));
}

ContractManifest ContractManifest::parse(std::string_view stored_bytes) {
    const auto value = parse_json_strict(stored_bytes);
    detail::reject_unknown_fields(value,
                                  "contract.manifest",
                                  {"storage_schema_version", "lifecycle", "spec", "review", "content_hash"});
    if (unsigned_value(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Unsupported contract manifest storage schema version");
    }
    const auto lifecycle =
        contract_manifest_lifecycle_from_string(string_value(value, "lifecycle"));
    const auto spec = spec_from_json(required_object(value, "spec"));
    const auto review = value.contains("review") ? review_from_json(value["review"]) : std::nullopt;
    validate_spec(spec);
    if (lifecycle == ContractManifestLifecycle::Proposed && review) {
        throw std::invalid_argument("Proposed contract manifest cannot contain a review");
    }
    if (lifecycle != ContractManifestLifecycle::Proposed && !review) {
        throw std::invalid_argument("Reviewed/frozen contract manifest requires a review");
    }
    if (lifecycle == ContractManifestLifecycle::Frozen && !review->approved) {
        throw std::invalid_argument("Frozen contract manifest requires an approved review");
    }
    const auto expected_hash = manifest_hash(spec, lifecycle, review);
    if (string_value(value, "content_hash") != expected_hash) {
        throw std::invalid_argument("Contract manifest content hash mismatch");
    }
    auto impl = std::make_shared<Impl>();
    impl->spec = spec;
    impl->lifecycle = lifecycle;
    impl->review = review;
    impl->content_hash = expected_hash;
    return ContractManifest(std::move(impl));
}

ContractManifest ContractManifest::review(ContractReview review_record) const {
    if (lifecycle() != ContractManifestLifecycle::Proposed) {
        throw std::logic_error("Contract manifest may only be reviewed once");
    }
    validate_identifier(review_record.reviewer_id, "reviewer_id");
    validate_utf8(review_record.notes);
    if (!review_record.approved) {
        throw std::invalid_argument("Contract review must be approved before freezing");
    }
    auto impl = std::make_shared<Impl>(*impl_);
    impl->lifecycle = ContractManifestLifecycle::Reviewed;
    impl->review = std::move(review_record);
    impl->content_hash = manifest_hash(impl->spec, impl->lifecycle, impl->review);
    return ContractManifest(std::move(impl));
}

ContractManifest ContractManifest::freeze() const {
    if (lifecycle() != ContractManifestLifecycle::Reviewed || !review_record() ||
        !review_record()->approved) {
        throw std::logic_error("Only an approved reviewed contract manifest may be frozen");
    }
    auto impl = std::make_shared<Impl>(*impl_);
    impl->lifecycle = ContractManifestLifecycle::Frozen;
    impl->content_hash = manifest_hash(impl->spec, impl->lifecycle, impl->review);
    return ContractManifest(std::move(impl));
}

ContractManifestLifecycle ContractManifest::lifecycle() const noexcept {
    return impl_->lifecycle;
}

const ContractManifestSpec& ContractManifest::spec() const noexcept {
    return impl_->spec;
}

const std::optional<ContractReview>& ContractManifest::review_record() const noexcept {
    return impl_->review;
}

const std::string& ContractManifest::content_hash() const noexcept {
    return impl_->content_hash;
}

std::string ContractManifest::serialize_canonical() const {
    return canonical_json_bytes(
        manifest_to_json(impl_->spec, impl_->lifecycle, impl_->review, impl_->content_hash));
}

std::string_view to_string(ContractEvidenceKind kind) noexcept {
    switch (kind) {
        case ContractEvidenceKind::WorkerReport:
            return "worker_report";
        case ContractEvidenceKind::DeterministicRun:
            return "deterministic_run";
        case ContractEvidenceKind::IndependentOracle:
            return "independent_oracle";
    }
    return "unknown";
}

ContractEvidenceKind contract_evidence_kind_from_string(std::string_view value) {
    if (value == "worker_report") {
        return ContractEvidenceKind::WorkerReport;
    }
    if (value == "deterministic_run") {
        return ContractEvidenceKind::DeterministicRun;
    }
    if (value == "independent_oracle") {
        return ContractEvidenceKind::IndependentOracle;
    }
    throw std::invalid_argument("Unknown contract evidence kind");
}

std::string_view to_string(ContractRunStatus status) noexcept {
    switch (status) {
        case ContractRunStatus::Frozen:
            return "frozen";
        case ContractRunStatus::Running:
            return "running";
        case ContractRunStatus::Verified:
            return "verified";
        case ContractRunStatus::Published:
            return "published";
        case ContractRunStatus::Blocked:
            return "blocked";
        case ContractRunStatus::Failed:
            return "failed";
    }
    return "unknown";
}

ContractRunStatus contract_run_status_from_string(std::string_view value) {
    if (value == "frozen") {
        return ContractRunStatus::Frozen;
    }
    if (value == "running") {
        return ContractRunStatus::Running;
    }
    if (value == "verified") {
        return ContractRunStatus::Verified;
    }
    if (value == "published") {
        return ContractRunStatus::Published;
    }
    if (value == "blocked") {
        return ContractRunStatus::Blocked;
    }
    if (value == "failed") {
        return ContractRunStatus::Failed;
    }
    throw std::invalid_argument("Unknown contract run status");
}

struct ContractRun::Impl {
    ContractManifest manifest;
    ContractRunStatus status = ContractRunStatus::Frozen;
    std::uint32_t attempt = 0;
    std::vector<ContractEvidence> evidence;
    std::vector<ContractDiagnostic> diagnostics;
    std::optional<ContractVerification> verification;

    explicit Impl(ContractManifest value) : manifest(std::move(value)) {}
};

ContractRun::ContractRun(ContractManifest frozen_manifest)
    : impl_(std::make_shared<Impl>(std::move(frozen_manifest))) {
    if (impl_->manifest.lifecycle() != ContractManifestLifecycle::Frozen) {
        throw std::invalid_argument("ContractRun requires a frozen manifest");
    }
}

ContractRun::ContractRun(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

const ContractManifest& ContractRun::manifest() const noexcept {
    return impl_->manifest;
}

ContractRunStatus ContractRun::status() const noexcept {
    return impl_->status;
}

std::uint32_t ContractRun::attempt() const noexcept {
    return impl_->attempt;
}

const std::vector<ContractEvidence>& ContractRun::evidence() const noexcept {
    return impl_->evidence;
}

const std::vector<ContractDiagnostic>& ContractRun::diagnostics() const noexcept {
    return impl_->diagnostics;
}

const std::optional<ContractVerification>& ContractRun::verification() const noexcept {
    return impl_->verification;
}

void ContractRun::begin_attempt() {
    if (impl_->status == ContractRunStatus::Published || impl_->status == ContractRunStatus::Verified) {
        throw std::logic_error("Published/verified contract run cannot start another attempt");
    }
    if (impl_->attempt >= impl_->manifest.spec().retry_policy.max_attempts) {
        impl_->status = ContractRunStatus::Blocked;
        throw std::runtime_error("Contract retry policy exhausted");
    }
    impl_->attempt += 1;
    impl_->status = ContractRunStatus::Running;
    impl_->verification.reset();
}

void ContractRun::record_worker_report(std::string report, bool claims_success) {
    if (impl_->status != ContractRunStatus::Running && impl_->status != ContractRunStatus::Blocked &&
        impl_->status != ContractRunStatus::Failed) {
        throw std::logic_error("Contract run cannot record a worker report in status " +
                               std::string(to_string(impl_->status)));
    }
    impl_->status = ContractRunStatus::Running;
    impl_->verification.reset();
    validate_utf8(report);
    ContractEvidence evidence;
    evidence.evidence_id = "worker-report-" + std::to_string(impl_->evidence.size() + 1);
    evidence.manifest_hash = impl_->manifest.content_hash();
    evidence.kind = ContractEvidenceKind::WorkerReport;
    evidence.executed = true;
    evidence.passed = claims_success;
    evidence.details = json{{"report", std::move(report)}, {"claims_success", claims_success}};
    impl_->evidence.push_back(std::move(evidence));
}

void ContractRun::record_evidence(ContractEvidence evidence) {
    if (impl_->status != ContractRunStatus::Running && impl_->status != ContractRunStatus::Blocked &&
        impl_->status != ContractRunStatus::Failed) {
        throw std::logic_error("Contract run cannot record evidence in status " +
                               std::string(to_string(impl_->status)));
    }
    impl_->status = ContractRunStatus::Running;
    impl_->verification.reset();
    validate_identifier(evidence.evidence_id, "evidence_id");
    if (evidence.manifest_hash != impl_->manifest.content_hash()) {
        throw std::invalid_argument("Contract evidence is bound to another manifest");
    }
    if (evidence.kind == ContractEvidenceKind::WorkerReport) {
        throw std::invalid_argument("Use record_worker_report for worker self-reports");
    }
    if (evidence.program_version_id.empty() || evidence.run_id.empty() ||
        evidence.workspace_revision.empty() || evidence.command.empty() ||
        evidence.toolchain.empty() || evidence.artifact_hash.empty()) {
        throw std::invalid_argument(
            "Contract evidence must identify version, run, workspace, command, toolchain, and "
            "artifact");
    }
    if (!evidence.executed) {
        throw std::invalid_argument("Contract evidence must be marked executed");
    }
    validate_utf8(evidence.program_version_id);
    validate_utf8(evidence.run_id);
    validate_utf8(evidence.workspace_revision);
    validate_utf8(evidence.command);
    validate_utf8(evidence.toolchain);
    validate_utf8(evidence.artifact_hash);
    for (const auto& diagnostic : evidence.diagnostics) {
        validate_utf8(diagnostic);
    }
    if (evidence.kind == ContractEvidenceKind::IndependentOracle) {
        if (!evidence.acceptance_id.empty()) {
            throw std::invalid_argument("Independent oracle evidence cannot target an acceptance gate");
        }
        if (!evidence.details.is_object() || !evidence.details.contains("oracle_id") ||
            !evidence.details["oracle_id"].is_string()) {
            throw std::invalid_argument("Independent oracle evidence must identify its oracle");
        }
        const auto oracle_id = evidence.details["oracle_id"].get<std::string>();
        if (std::find(impl_->manifest.spec().independent_oracles.begin(),
                      impl_->manifest.spec().independent_oracles.end(),
                      oracle_id) == impl_->manifest.spec().independent_oracles.end()) {
            throw std::invalid_argument("Contract evidence names an unknown independent oracle");
        }
    } else {
        if (evidence.acceptance_id.empty()) {
            throw std::invalid_argument("Deterministic evidence must target an acceptance gate");
        }
        const auto acceptance = std::find_if(
            impl_->manifest.spec().acceptance.begin(), impl_->manifest.spec().acceptance.end(),
            [&](const auto& item) { return item.id == evidence.acceptance_id; });
        if (acceptance == impl_->manifest.spec().acceptance.end()) {
            throw std::invalid_argument("Contract evidence names an unknown acceptance gate");
        }
    }
    impl_->evidence.push_back(std::move(evidence));
}

void ContractRun::record_diagnostic(ContractDiagnostic diagnostic) {
    if (impl_->status != ContractRunStatus::Running && impl_->status != ContractRunStatus::Blocked &&
        impl_->status != ContractRunStatus::Failed) {
        throw std::logic_error("Contract run cannot record a diagnostic in status " +
                               std::string(to_string(impl_->status)));
    }
    impl_->status = ContractRunStatus::Running;
    impl_->verification.reset();
    validate_identifier(diagnostic.code, "diagnostic.code");
    if (diagnostic.message.empty()) {
        throw std::invalid_argument("Contract diagnostic message must not be empty");
    }
    validate_utf8(diagnostic.message);
    impl_->diagnostics.push_back(std::move(diagnostic));
}

ContractVerification ContractRun::verify(std::string_view program_version_id,
                                         std::string_view run_id,
                                         std::string_view workspace_revision) {
    if (impl_->status == ContractRunStatus::Published && impl_->verification) {
        return *impl_->verification;
    }
    if (impl_->status != ContractRunStatus::Running && impl_->status != ContractRunStatus::Blocked &&
        impl_->status != ContractRunStatus::Failed) {
        throw std::logic_error("Contract run must be running before verification");
    }
    if (program_version_id.empty() || run_id.empty() || workspace_revision.empty()) {
        throw std::invalid_argument(
            "Verification requires program version, run identity, and workspace revision");
    }

    ContractVerification result;
    result.status = ContractRunStatus::Blocked;
    const auto& spec = impl_->manifest.spec();
    for (const auto& acceptance : spec.acceptance) {
        if (!acceptance.required) {
            continue;
        }
        bool passed = false;
        bool failed = false;
        for (const auto& evidence : impl_->evidence) {
            if (evidence.kind == ContractEvidenceKind::WorkerReport ||
                evidence.acceptance_id != acceptance.id ||
                evidence.manifest_hash != impl_->manifest.content_hash() ||
                evidence.program_version_id != program_version_id ||
                evidence.run_id != run_id ||
                evidence.workspace_revision != workspace_revision) {
                continue;
            }
            if (evidence.passed) {
                passed = true;
                continue;
            }
            failed = true;
            result.failed_evidence_ids.push_back(evidence.evidence_id);
        }
        // A later positive claim cannot erase an independently observed
        // failure for the same gate. Publication is deliberately fail-closed.
        if (!passed || failed) {
            result.missing_acceptance_ids.push_back(acceptance.id);
        }
    }

    for (const auto& oracle : spec.independent_oracles) {
        bool passed = false;
        bool failed = false;
        for (const auto& evidence : impl_->evidence) {
            if (evidence.kind != ContractEvidenceKind::IndependentOracle ||
                evidence.manifest_hash != impl_->manifest.content_hash() ||
                evidence.program_version_id != program_version_id ||
                evidence.run_id != run_id ||
                evidence.workspace_revision != workspace_revision ||
                !evidence.details.is_object() ||
                !evidence.details.contains("oracle_id") ||
                !evidence.details["oracle_id"].is_string() ||
                evidence.details["oracle_id"].get<std::string>() != oracle) {
                continue;
            }
            if (evidence.passed) {
                passed = true;
                continue;
            }
            failed = true;
            result.failed_evidence_ids.push_back(evidence.evidence_id);
        }
        if (!passed || failed) {
            result.blocking_diagnostics.push_back("missing-independent-oracle:" + oracle);
        }
    }

    for (const auto& diagnostic : impl_->diagnostics) {
        if (diagnostic.blocking) {
            result.blocking_diagnostics.push_back(diagnostic.code + ":" + diagnostic.message);
        }
    }

    const bool has_failure = !result.failed_evidence_ids.empty();
    const bool blocked = !result.missing_acceptance_ids.empty() ||
                         !result.blocking_diagnostics.empty();
    result.publishable = !has_failure && !blocked;
    result.status = result.publishable ? ContractRunStatus::Verified
                                       : (has_failure ? ContractRunStatus::Failed
                                                      : ContractRunStatus::Blocked);
    impl_->verification = result;
    impl_->status = result.status;
    return result;
}

void ContractRun::publish() {
    if (impl_->status != ContractRunStatus::Verified || !impl_->verification ||
        !impl_->verification->publishable) {
        throw std::logic_error("Contract run is not independently verified");
    }
    impl_->status = ContractRunStatus::Published;
    impl_->verification->status = ContractRunStatus::Published;
}

std::string ContractRun::serialize_canonical() const {
    json evidence = json::array();
    for (const auto& item : impl_->evidence) {
        evidence.push_back(evidence_to_json(item));
    }
    json diagnostics = json::array();
    for (const auto& item : impl_->diagnostics) {
        diagnostics.push_back(diagnostic_to_json(item));
    }
    json result{{"storage_schema_version", STORAGE_SCHEMA_VERSION},
                {"manifest", parse_json_strict(impl_->manifest.serialize_canonical())},
                {"status", std::string(to_string(impl_->status))},
                {"attempt", impl_->attempt},
                {"evidence", evidence},
                {"diagnostics", diagnostics},
                {"verification", verification_to_json(impl_->verification)}};
    return canonical_json_bytes(result);
}

ContractRun ContractRun::parse(std::string_view stored_bytes) {
    const auto value = parse_json_strict(stored_bytes);
    detail::reject_unknown_fields(value,
                                  "contract.run",
                                  {"storage_schema_version",
                                   "manifest",
                                   "status",
                                   "attempt",
                                   "evidence",
                                   "diagnostics",
                                   "verification"});
    if (unsigned_value(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Unsupported contract run storage schema version");
    }
    auto impl = std::make_shared<Impl>(ContractManifest::parse(
        canonical_json_bytes(required_object(value, "manifest"))));
    impl->status = contract_run_status_from_string(string_value(value, "status"));
    impl->attempt = static_cast<std::uint32_t>(unsigned_value(value, "attempt"));
    std::set<std::string> seen_evidence_ids;
    for (const auto& item : required_array(value, "evidence")) {
        const auto evidence = evidence_from_json(item);
        validate_loaded_evidence(evidence, impl->manifest, seen_evidence_ids);
        impl->evidence.push_back(evidence);
    }
    for (const auto& item : required_array(value, "diagnostics")) {
        const auto diagnostic = diagnostic_from_json(item);
        validate_loaded_diagnostic(diagnostic);
        impl->diagnostics.push_back(diagnostic);
    }
    if (value.contains("verification") && !value["verification"].is_null()) {
        impl->verification = verification_from_json(value["verification"]);
    }
    if (impl->status == ContractRunStatus::Published &&
        (!impl->verification || !impl->verification->publishable)) {
        throw std::invalid_argument("Published contract run lacks publishable verification");
    }
    if (impl->status == ContractRunStatus::Running && impl->attempt == 0) {
        throw std::invalid_argument("Running contract run must have an attempt");
    }
    return ContractRun(std::move(impl));
}

}  // namespace neograph::program
