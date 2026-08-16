#include <neograph/program/replay.h>

#include "canonical_json.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::string_view EVIDENCE_FORMAT = "neograph-recorded-capability-evidence";

std::string require_string(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_string()) {
        throw std::invalid_argument("Recorded evidence field '" + owned_key +
                                    "' must be a string");
    }
    return value[owned_key].get<std::string>();
}

std::uint64_t require_uint64(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_number_unsigned()) {
        throw std::invalid_argument("Recorded evidence field '" + owned_key +
                                    "' must be unsigned");
    }
    return value[owned_key].get<std::uint64_t>();
}

std::uint32_t require_uint32(const json& value, std::string_view key) {
    const auto number = require_uint64(value, key);
    if (number > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Recorded evidence integer exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(number);
}

bool require_bool(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_boolean()) {
        throw std::invalid_argument("Recorded evidence field '" + owned_key +
                                    "' must be boolean");
    }
    return value[owned_key].get<bool>();
}

json require_value(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key)) {
        throw std::invalid_argument("Recorded evidence requires field '" + owned_key + "'");
    }
    return value[owned_key];
}

void require_token(std::string_view value, std::string_view name) {
    detail::validate_token(value, name);
}

void require_sha256(std::string_view value, std::string_view name) {
    if (!detail::is_sha256_identity(value)) {
        throw std::invalid_argument(std::string(name) + " must be a sha256 identity");
    }
}

json encode_executable(const ExecutableIdentity& executable) {
    return json{{"kind", std::string(to_string(executable.kind))},
                {"name", executable.name},
                {"semantic_version", executable.semantic_version},
                {"implementation_digest", executable.implementation_digest}};
}

ExecutableIdentity parse_executable(const json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("Recorded executable identity must be an object");
    }
    detail::reject_unknown_fields(value, "Recorded executable identity",
                                  {"kind", "name", "semantic_version",
                                   "implementation_digest"});
    ExecutableIdentity result;
    result.kind = executable_kind_from_string(require_string(value, "kind"));
    result.name = require_string(value, "name");
    result.semantic_version = require_string(value, "semantic_version");
    result.implementation_digest = require_string(value, "implementation_digest");
    require_token(result.name, "Recorded executable name");
    require_token(result.semantic_version, "Recorded executable semantic version");
    require_sha256(result.implementation_digest, "Recorded executable implementation digest");
    return result;
}

json encode_binding(const CapabilityBindingReceipt& binding) {
    return json{{"executable", encode_executable(binding.executable)},
                {"binding_identity", binding.binding_identity}};
}

CapabilityBindingReceipt parse_binding(const json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("Recorded binding receipt must be an object");
    }
    detail::reject_unknown_fields(value, "Recorded binding receipt",
                                  {"executable", "binding_identity"});
    CapabilityBindingReceipt result;
    result.executable = parse_executable(require_value(value, "executable"));
    result.binding_identity = require_string(value, "binding_identity");
    require_sha256(result.binding_identity, "Recorded binding identity");
    return result;
}

auto binding_key(const CapabilityBindingReceipt& binding) {
    return std::tuple{static_cast<int>(binding.executable.kind), binding.executable.name,
                      binding.executable.semantic_version,
                      binding.executable.implementation_digest, binding.binding_identity};
}

void normalize_bindings(std::vector<CapabilityBindingReceipt>& bindings,
                        std::string_view context) {
    for (const auto& binding : bindings) {
        require_token(binding.executable.name, "Recorded binding executable name");
        require_token(binding.executable.semantic_version,
                      "Recorded binding executable semantic version");
        require_sha256(binding.executable.implementation_digest,
                       "Recorded binding executable digest");
        require_sha256(binding.binding_identity, "Recorded binding identity");
    }
    std::sort(bindings.begin(), bindings.end(),
              [](const auto& lhs, const auto& rhs) { return binding_key(lhs) < binding_key(rhs); });
    if (std::adjacent_find(bindings.begin(), bindings.end()) != bindings.end()) {
        throw std::invalid_argument(std::string(context) + " contains duplicate binding receipts");
    }
}

json encode_reference(const RecordedCapabilityCallReference& reference) {
    json value{{"sequence", reference.sequence},
               {"binding", encode_binding(reference.binding)},
               {"operation_id", reference.operation_id},
               {"call_id", reference.call_id}};
    value["effect_id"] = reference.effect_id ? json(*reference.effect_id) : json(nullptr);
    return value;
}

RecordedCapabilityCallReference parse_reference(const json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("Recorded call reference must be an object");
    }
    detail::reject_unknown_fields(
        value, "Recorded call reference",
        {"sequence", "binding", "operation_id", "call_id", "effect_id"});
    RecordedCapabilityCallReference reference;
    reference.sequence = require_uint64(value, "sequence");
    reference.binding = parse_binding(require_value(value, "binding"));
    reference.operation_id = require_string(value, "operation_id");
    reference.call_id = require_string(value, "call_id");
    require_token(reference.operation_id, "Recorded capability operation id");
    require_token(reference.call_id, "Recorded capability call id");
    const auto& effect = require_value(value, "effect_id");
    if (!effect.is_null()) {
        if (!effect.is_string()) {
            throw std::invalid_argument("Recorded effect_id must be a string or null");
        }
        reference.effect_id = effect.get<std::string>();
        require_token(*reference.effect_id, "Recorded capability effect id");
    }
    return reference;
}

json encode_failure(const RecordedCapabilityFailure& failure) {
    return json{{"code", failure.code},
                {"classification", failure.classification},
                {"message", failure.message},
                {"witness", failure.witness}};
}

RecordedCapabilityFailure parse_failure(const json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("Recorded capability failure must be an object");
    }
    detail::reject_unknown_fields(value, "Recorded capability failure",
                                  {"code", "classification", "message", "witness"});
    RecordedCapabilityFailure failure;
    failure.code = require_string(value, "code");
    failure.classification = require_string(value, "classification");
    failure.message = require_string(value, "message");
    failure.witness = require_value(value, "witness");
    require_token(failure.code, "Recorded capability failure code");
    require_token(failure.classification, "Recorded capability failure classification");
    return failure;
}

void validate_evidence_data(const RecordedCapabilityEvidenceData& data) {
    if (data.reference.sequence == 0) {
        throw std::invalid_argument("Recorded capability sequence must start at one");
    }
    require_token(data.reference.operation_id, "Recorded capability operation id");
    require_token(data.reference.call_id, "Recorded capability call id");
    if (data.reference.effect_id) {
        require_token(*data.reference.effect_id, "Recorded capability effect id");
    }
    std::vector<CapabilityBindingReceipt> one{data.reference.binding};
    normalize_bindings(one, "Recorded capability evidence");
    const unsigned outcome_count = static_cast<unsigned>(data.input_outcome.has_value()) +
                                   static_cast<unsigned>(data.effect_outcome.has_value()) +
                                   static_cast<unsigned>(data.failure.has_value());
    if (outcome_count > 1) {
        throw std::invalid_argument(
            "Recorded capability evidence cannot contain multiple outcomes");
    }
    if (data.input_outcome) {
        if (data.reference.effect_id ||
            data.input_outcome->operation_id() != data.reference.operation_id ||
            data.input_outcome->call_id() != data.reference.call_id ||
            data.input_outcome->kind() != ProgramPendingInputKind::CapabilityResult ||
            data.input_outcome->state() != ProgramPendingState::Consumed ||
            !data.input_outcome->consumed_result()) {
            throw std::invalid_argument(
                "Recorded capability input outcome is not an exact consumed result");
        }
    }
    if (data.effect_outcome) {
        if (!data.reference.effect_id ||
            data.effect_outcome->operation_id() != data.reference.operation_id ||
            data.effect_outcome->call_id() != data.reference.call_id ||
            data.effect_outcome->effect_id() != *data.reference.effect_id ||
            data.effect_outcome->state() != ProgramPendingState::Consumed ||
            (data.effect_outcome->reconciliation() !=
                 ProgramEffectReconciliation::Completed &&
             data.effect_outcome->reconciliation() != ProgramEffectReconciliation::Failed)) {
            throw std::invalid_argument(
                "Recorded capability effect outcome is not an exact reconciled result/failure");
        }
        if ((data.effect_outcome->reconciliation() ==
             ProgramEffectReconciliation::Completed) !=
            data.effect_outcome->reconciled_result().has_value()) {
            throw std::invalid_argument(
                "Recorded completed/failed effect result shape is inconsistent");
        }
    }
    if (data.failure) {
        require_token(data.failure->code, "Recorded capability failure code");
        require_token(data.failure->classification,
                      "Recorded capability failure classification");
    }
    if (data.coverage == RecordedEvidenceCoverage::Full && !data.redacted &&
        outcome_count != 1) {
        throw std::invalid_argument(
            "FULL non-redacted recorded evidence requires exactly one outcome");
    }
}

json stored_value(std::string bytes) {
    return detail::parse_json_strict(bytes);
}

json evidence_body(const RecordedCapabilityEvidenceData& data) {
    json value{{"reference", encode_reference(data.reference)},
               {"coverage", std::string(to_string(data.coverage))},
               {"redacted", data.redacted}};
    value["input_outcome"] =
        data.input_outcome ? stored_value(data.input_outcome->serialize_canonical())
                           : json(nullptr);
    value["effect_outcome"] =
        data.effect_outcome ? stored_value(data.effect_outcome->serialize_canonical())
                            : json(nullptr);
    value["failure"] = data.failure ? encode_failure(*data.failure) : json(nullptr);
    return value;
}

json evidence_envelope(const RecordedCapabilityEvidenceData& data) {
    auto value = evidence_body(data);
    value["format"] = std::string(EVIDENCE_FORMAT);
    value["storage_schema_version"] = RecordedCapabilityEvidence::STORAGE_SCHEMA_VERSION;
    return value;
}

RecordedCapabilityEvidenceData parse_evidence_body(const json& value) {
    RecordedCapabilityEvidenceData data;
    data.reference = parse_reference(require_value(value, "reference"));
    data.coverage = recorded_evidence_coverage_from_string(require_string(value, "coverage"));
    data.redacted = require_bool(value, "redacted");
    const auto& input = require_value(value, "input_outcome");
    if (!input.is_null()) {
        data.input_outcome =
            ProgramPendingInput::parse(detail::canonical_json_bytes(input));
    }
    const auto& effect = require_value(value, "effect_outcome");
    if (!effect.is_null()) {
        data.effect_outcome =
            ProgramPendingEffect::parse(detail::canonical_json_bytes(effect));
    }
    const auto& failure = require_value(value, "failure");
    if (!failure.is_null()) data.failure = parse_failure(failure);
    validate_evidence_data(data);
    return data;
}

bool contains_binding(const std::vector<CapabilityBindingReceipt>& bindings,
                      const CapabilityBindingReceipt& binding) {
    return std::binary_search(bindings.begin(), bindings.end(), binding,
                              [](const auto& lhs, const auto& rhs) {
                                  return binding_key(lhs) < binding_key(rhs);
                              });
}

}  // namespace

std::string_view to_string(RecordedEvidenceCoverage coverage) noexcept {
    switch (coverage) {
        case RecordedEvidenceCoverage::Full: return "full";
        case RecordedEvidenceCoverage::Partial: return "partial";
        case RecordedEvidenceCoverage::Redacted: return "redacted";
    }
    return "partial";
}

RecordedEvidenceCoverage recorded_evidence_coverage_from_string(std::string_view value) {
    if (value == "full") return RecordedEvidenceCoverage::Full;
    if (value == "partial") return RecordedEvidenceCoverage::Partial;
    if (value == "redacted") return RecordedEvidenceCoverage::Redacted;
    throw std::invalid_argument("Unknown recorded evidence coverage");
}

struct RecordedCapabilityEvidence::Impl {
    explicit Impl(RecordedCapabilityEvidenceData value) : data(std::move(value)) {}
    RecordedCapabilityEvidenceData data;
    std::string                    id;
};

RecordedCapabilityEvidence::RecordedCapabilityEvidence(RecordedCapabilityEvidenceData data) {
    validate_evidence_data(data);
    auto impl = std::make_shared<Impl>(std::move(data));
    impl->id = detail::sha256_identity(
        "recorded-capability-evidence",
        detail::canonical_json_bytes(evidence_envelope(impl->data)));
    impl_ = std::move(impl);
}

RecordedCapabilityEvidence::RecordedCapabilityEvidence(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

RecordedCapabilityEvidence RecordedCapabilityEvidence::parse(std::string_view stored_bytes) {
    json value;
    try {
        value = detail::parse_json_strict(stored_bytes);
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string("Invalid stored recorded evidence JSON: ") +
                                    error.what());
    }
    if (!value.is_object() || require_string(value, "format") != EVIDENCE_FORMAT) {
        throw std::invalid_argument("Stored recorded evidence has unknown format");
    }
    detail::reject_unknown_fields(
        value, "Stored recorded capability evidence",
        {"format", "storage_schema_version", "id", "reference", "coverage", "redacted",
         "input_outcome", "effect_outcome", "failure"});
    if (require_uint32(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored recorded evidence schema version is unsupported");
    }
    RecordedCapabilityEvidence parsed(parse_evidence_body(value));
    const auto stored_id = require_string(value, "id");
    if (!detail::is_sha256_identity(stored_id) || stored_id != parsed.id()) {
        throw std::invalid_argument("Stored recorded evidence id does not match its content");
    }
    return parsed;
}

const std::string& RecordedCapabilityEvidence::id() const noexcept { return impl_->id; }
const RecordedCapabilityCallReference& RecordedCapabilityEvidence::reference() const noexcept {
    return impl_->data.reference;
}
RecordedEvidenceCoverage RecordedCapabilityEvidence::coverage() const noexcept {
    return impl_->data.coverage;
}
bool RecordedCapabilityEvidence::redacted() const noexcept { return impl_->data.redacted; }
std::optional<ProgramPendingInput> RecordedCapabilityEvidence::input_outcome() const {
    return impl_->data.input_outcome;
}
std::optional<ProgramPendingEffect> RecordedCapabilityEvidence::effect_outcome() const {
    return impl_->data.effect_outcome;
}
std::optional<RecordedCapabilityFailure> RecordedCapabilityEvidence::failure() const {
    return impl_->data.failure;
}
std::string RecordedCapabilityEvidence::serialize_canonical() const {
    auto value = evidence_envelope(impl_->data);
    value["id"] = impl_->id;
    return detail::canonical_json_bytes(value);
}

struct RecordedBindingSet::Impl {
    std::vector<CapabilityBindingReceipt>        exact_bindings;
    std::vector<RecordedCapabilityCallReference> expected_calls;
    CatalogCapabilityBinding                     binding;
    std::vector<RecordedCapabilityEvidence>      evidence;
    std::string                                  fingerprint;
    bool                                         released = false;
};

RecordedBindingSet::RecordedBindingSet(
    std::vector<CapabilityBindingReceipt> exact_bindings,
    std::vector<RecordedCapabilityCallReference> expected_calls,
    CatalogCapabilityBinding owned_binding,
    std::vector<RecordedCapabilityEvidence> evidence)
    : impl_(std::make_unique<Impl>()) {
    normalize_bindings(exact_bindings, "Recorded binding set");
    auto owned_receipts = owned_binding.receipts;
    normalize_bindings(owned_receipts, "Recorded owned binding");
    if (owned_receipts != exact_bindings) {
        throw std::invalid_argument(
            "Recorded owned binding receipts do not exactly match pinned receipts");
    }
    if (expected_calls.size() != evidence.size()) {
        throw std::invalid_argument("Recorded replay evidence is missing or has extra calls");
    }

    std::set<std::pair<std::string, std::string>> call_ids;
    std::set<std::tuple<std::string, std::string, std::string>> effect_ids;
    for (std::size_t index = 0; index < expected_calls.size(); ++index) {
        const auto& expected = expected_calls[index];
        const auto& actual = evidence[index];
        if (expected.sequence != index + 1) {
            throw std::invalid_argument(
                "Recorded replay expected call sequence is not contiguous and ordered");
        }
        if (!call_ids.insert({expected.operation_id, expected.call_id}).second) {
            throw std::invalid_argument("Recorded replay contains duplicate call ids");
        }
        if (expected.effect_id &&
            !effect_ids
                 .insert({expected.operation_id, expected.call_id, *expected.effect_id})
                 .second) {
            throw std::invalid_argument("Recorded replay contains duplicate effect ids");
        }
        if (!contains_binding(exact_bindings, expected.binding)) {
            throw std::invalid_argument("Recorded replay expected call has a wrong binding receipt");
        }
        if (actual.reference() != expected) {
            throw std::invalid_argument(
                "Recorded replay evidence is unordered or bound to a wrong call/effect/receipt");
        }
        if (actual.coverage() != RecordedEvidenceCoverage::Full || actual.redacted()) {
            throw std::invalid_argument("Recorded replay requires FULL non-redacted evidence");
        }
        const unsigned outcome_count =
            static_cast<unsigned>(actual.input_outcome().has_value()) +
            static_cast<unsigned>(actual.effect_outcome().has_value()) +
            static_cast<unsigned>(actual.failure().has_value());
        if (outcome_count != 1) {
            throw std::invalid_argument(
                "Recorded replay evidence requires exactly one result or failure");
        }
    }

    json fingerprint_body = json::object();
    fingerprint_body["bindings"] = json::array();
    for (const auto& binding : exact_bindings) {
        fingerprint_body["bindings"].push_back(encode_binding(binding));
    }
    fingerprint_body["calls"] = json::array();
    for (const auto& expected : expected_calls) {
        fingerprint_body["calls"].push_back(encode_reference(expected));
    }
    fingerprint_body["evidence"] = json::array();
    for (const auto& item : evidence) {
        fingerprint_body["evidence"].push_back(item.id());
    }

    impl_->exact_bindings = std::move(exact_bindings);
    impl_->expected_calls = std::move(expected_calls);
    impl_->binding = std::move(owned_binding);
    impl_->evidence = std::move(evidence);
    impl_->fingerprint = detail::sha256_identity(
        "recorded-binding-set", detail::canonical_json_bytes(fingerprint_body));
}

RecordedBindingSet::RecordedBindingSet(RecordedBindingSet&&) noexcept = default;
RecordedBindingSet& RecordedBindingSet::operator=(RecordedBindingSet&&) noexcept = default;
RecordedBindingSet::~RecordedBindingSet() = default;

const std::string& RecordedBindingSet::fingerprint() const noexcept {
    return impl_->fingerprint;
}
const std::vector<CapabilityBindingReceipt>& RecordedBindingSet::exact_bindings() const noexcept {
    return impl_->exact_bindings;
}
const std::vector<RecordedCapabilityCallReference>&
RecordedBindingSet::expected_calls() const noexcept {
    return impl_->expected_calls;
}
const std::vector<RecordedCapabilityEvidence>& RecordedBindingSet::evidence() const noexcept {
    return impl_->evidence;
}

void RecordedBindingSet::validate_target(const ProgramVersion& target) const {
    auto expected = target.core_materialization_receipt().capability_bindings;
    normalize_bindings(expected, "Target ProgramVersion");
    if (expected != impl_->exact_bindings) {
        throw std::invalid_argument(
            "Recorded binding set does not exactly bind the target ProgramVersion");
    }
}

CatalogCapabilityBinding RecordedBindingSet::release_owned_binding() && {
    if (impl_->released) {
        throw std::logic_error("Recorded binding set was already released");
    }
    impl_->released = true;
    return std::move(impl_->binding);
}

struct ProgramHistoricalReplacementStep::Impl {
    ProgramRunGeneration                     source_generation;
    ProgramRunLineage                        source_lineage;
    ProgramRunRecord                         source_run;
    ProgramJavaScriptCommandJournalEntry      checkpoint;
    ProgramRunGeneration                     target_generation;
    ProgramRunLineage                        target_initial_lineage;
    ProgramTransitionPublication             target_initial_publication;
};

ProgramHistoricalReplacementStep::ProgramHistoricalReplacementStep(
    std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

const ProgramRunGeneration&
ProgramHistoricalReplacementStep::source_generation() const noexcept {
    return impl_->source_generation;
}
const ProgramRunLineage& ProgramHistoricalReplacementStep::source_lineage() const noexcept {
    return impl_->source_lineage;
}
const ProgramRunRecord& ProgramHistoricalReplacementStep::source_run() const noexcept {
    return impl_->source_run;
}
const ProgramJavaScriptCommandJournalEntry&
ProgramHistoricalReplacementStep::checkpoint() const noexcept {
    return impl_->checkpoint;
}
const ProgramRunGeneration&
ProgramHistoricalReplacementStep::target_generation() const noexcept {
    return impl_->target_generation;
}
const ProgramRunLineage&
ProgramHistoricalReplacementStep::target_initial_lineage() const noexcept {
    return impl_->target_initial_lineage;
}
const ProgramTransitionPublication&
ProgramHistoricalReplacementStep::target_initial_publication() const noexcept {
    return impl_->target_initial_publication;
}

struct ProgramHistoricalReplacementChain::Impl {
    ProgramRunLineage                            anchor;
    std::vector<ProgramRunGeneration>            generations;
    std::vector<ProgramHistoricalReplacementStep> replacements;
};

ProgramHistoricalReplacementChain::ProgramHistoricalReplacementChain(
    std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

const ProgramRunLineage& ProgramHistoricalReplacementChain::anchor() const noexcept {
    return impl_->anchor;
}
const std::vector<ProgramRunGeneration>&
ProgramHistoricalReplacementChain::generations() const noexcept {
    return impl_->generations;
}
const std::vector<ProgramHistoricalReplacementStep>&
ProgramHistoricalReplacementChain::replacements() const noexcept {
    return impl_->replacements;
}

ProgramHistoricalReplacementChain inspect_program_replacement_chain(
    const ProgramTransitionStore& store,
    std::string_view owner_scope,
    std::string_view any_run_id) {
    if (owner_scope.empty() || any_run_id.empty()) {
        throw std::invalid_argument(
            "Program replacement history owner scope and run id must not be empty");
    }
    const auto fail = []() -> void {
        throw std::invalid_argument(
            "Program historical replacement evidence is absent or invalid");
    };

    const auto anchor = store.load_run_lineage(owner_scope, any_run_id);
    if (!anchor || anchor->owner_scope() != owner_scope || anchor->active_generation() == 0) fail();

    std::vector<ProgramRunGeneration>        generations;
    std::vector<ProgramTransitionPublication> initial_publications;
    for (std::uint64_t ordinal = 1; ordinal <= anchor->active_generation(); ++ordinal) {
        const auto generation = store.load_generation(owner_scope, anchor->lineage_id(), ordinal);
        const auto publication = store.load_generation_initial_publication(
            owner_scope, anchor->lineage_id(), ordinal);
        if (!generation || !publication || !publication->run_generation ||
            !publication->run_lineage || publication->run_generation->id() != generation->id() ||
            generation->owner_scope() != owner_scope ||
            generation->lineage_id() != anchor->lineage_id() ||
            generation->generation() != ordinal ||
            generation->initial_run_record_id() != publication->run_record.id() ||
            generation->initial_journal_head() != publication->journal_record.id ||
            publication->run_record.journal_head() != publication->journal_record.id ||
            publication->run_lineage->active_run_record_id() != publication->run_record.id() ||
            publication->run_lineage->active_journal_head() != publication->journal_record.id ||
            !does_program_run_generation_bind(*generation, *publication->run_lineage,
                                               publication->run_record)) {
            fail();
        }
        if (ordinal == 1) {
            if (generation->replacement_receipt() ||
                !is_valid_program_run_lineage_initial(*publication->run_lineage, *generation)) {
                fail();
            }
        } else {
            if (!generation->replacement_receipt() ||
                !generation->predecessor_generation_id() ||
                *generation->predecessor_generation_id() != generations.back().id()) {
                fail();
            }
        }
        generations.push_back(*generation);
        initial_publications.push_back(*publication);
        if (ordinal == std::numeric_limits<std::uint64_t>::max()) fail();
    }
    if (std::none_of(generations.begin(), generations.end(), [&](const auto& generation) {
            return generation.run_id() == any_run_id;
        })) {
        fail();
    }

    std::set<std::string, std::less<>> lineage_path;
    auto                               current = *anchor;
    while (true) {
        if (!lineage_path.insert(current.id()).second) fail();
        if (!current.predecessor_head_id()) break;
        const auto previous = store.load_lineage_head(
            owner_scope, anchor->lineage_id(), *current.predecessor_head_id());
        if (!previous) fail();
        std::optional<ProgramRunGeneration> successor;
        if (current.active_generation() != previous->active_generation()) {
            if (previous->active_generation() == std::numeric_limits<std::uint64_t>::max() ||
                current.active_generation() != previous->active_generation() + 1 ||
                current.active_generation() > generations.size()) {
                fail();
            }
            successor = generations[static_cast<std::size_t>(current.active_generation() - 1)];
        }
        if (!is_valid_program_run_lineage_transition(*previous, current, successor)) fail();
        current = *previous;
    }
    if (!is_valid_program_run_lineage_initial(current, generations.front())) fail();
    for (const auto& publication : initial_publications) {
        if (!lineage_path.contains(publication.run_lineage->id())) fail();
    }

    std::vector<ProgramHistoricalReplacementStep> replacements;
    replacements.reserve(generations.size() - 1);
    for (std::size_t index = 1; index < generations.size(); ++index) {
        const auto& source_generation = generations[index - 1];
        const auto& target_generation = generations[index];
        const auto  receipt = target_generation.replacement_receipt();
        if (!receipt) fail();
        const auto source_lineage = store.load_lineage_head(
            owner_scope, anchor->lineage_id(), receipt->source_lineage_head_id());
        const auto source_run = store.load(owner_scope, receipt->source_run_id());
        const auto source_journal = store.latest(owner_scope, receipt->source_run_id());
        if (!source_lineage || !source_run || !source_journal ||
            source_generation.id() != receipt->source_generation_id() ||
            source_run->id() != receipt->source_run_record_id() ||
            source_journal->id != receipt->source_journal_head() ||
            source_run->journal_head() != source_journal->id ||
            !lineage_path.contains(source_lineage->id())) {
            fail();
        }

        const auto commands =
            store.load_javascript_commands(owner_scope, source_run->run_id());
        if (commands.empty() || commands.back().id() != receipt->checkpoint_entry_id()) fail();
        const auto& checkpoint = commands.back();
        const auto& target_publication = initial_publications[index];
        if (checkpoint.coordinate_id() != receipt->checkpoint_coordinate_id() ||
            target_publication.run_generation->id() != target_generation.id() ||
            !target_publication.run_lineage->predecessor_head_id() ||
            *target_publication.run_lineage->predecessor_head_id() != source_lineage->id() ||
            !is_valid_program_run_lineage_transition(
                *source_lineage, *target_publication.run_lineage, target_generation) ||
            !is_valid_program_replacement_transition(
                source_generation, *source_lineage, *source_run, checkpoint,
                target_generation, *target_publication.run_lineage,
                target_publication.run_record)) {
            fail();
        }
        replacements.push_back(ProgramHistoricalReplacementStep(
            std::make_shared<const ProgramHistoricalReplacementStep::Impl>(
                ProgramHistoricalReplacementStep::Impl{
                    source_generation, *source_lineage, *source_run, checkpoint,
                    target_generation, *target_publication.run_lineage, target_publication})));
    }

    return ProgramHistoricalReplacementChain(
        std::make_shared<const ProgramHistoricalReplacementChain::Impl>(
            ProgramHistoricalReplacementChain::Impl{
                *anchor, std::move(generations), std::move(replacements)}));
}

}  // namespace neograph::program
