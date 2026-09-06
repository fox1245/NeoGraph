#include <neograph/program/child_synthesis.h>
#include <neograph/program/transition_store.h>

#include "canonical_json.h"

#include <algorithm>
#include <array>

namespace neograph::program {
namespace {
using State                                      = ProgramChildSynthesisState;
constexpr std::array<std::string_view, 12> names = {
    "reserved", "compiling", "compiled",    "validating", "validated", "admitting",
    "admitted", "bound",     "dispatching", "spawned",    "failed",    "reconciliation_required"};
json nested(std::string_view bytes) {
    return detail::parse_json_strict(bytes);
}
std::string encoded(const json& value) {
    return detail::canonical_json_bytes(value);
}
void require(bool condition) {
    if (!condition) throw std::invalid_argument("Invalid durable child synthesis record");
}
json body(const ProgramChildSynthesisRecordData& d) {
    const auto index = static_cast<std::size_t>(d.state);
    require(index < names.size());
    return {{"format", "neograph-program-child-synthesis-record"},
            {"storage_schema_version", 1},
            {"proposal", nested(d.proposal.serialize_canonical())},
            {"grant", nested(d.grant.serialize_canonical())},
            {"parent_version", nested(d.parent.version.serialize_canonical())},
            {"parent_run", nested(d.parent.run.serialize_canonical())},
            {"parent_lineage", nested(d.parent.lineage.serialize_canonical())},
            {"parent_generation", nested(d.parent.generation.serialize_canonical())},
            {"reservation", nested(d.reservation.serialize_canonical())},
            {"binding_name", d.binding_name},
            {"revision", d.revision},
            {"previous_id", d.previous_id},
            {"state", names[index]},
            {"artifacts", d.artifacts},
            {"error_code", d.error_code}};
}
void validate(const ProgramChildSynthesisRecordData& d) {
    auto authorization = authorize_program_child_synthesis(d.proposal, d.grant, d.parent);
    validate_program_child_synthesis_reservation(authorization, d.reservation);
    detail::validate_token(d.binding_name, "Child synthesis binding");
    require(d.binding_name.size() <= 256 && d.revision > 0 && d.revision <= 16);
    require(d.revision == 1 ? d.previous_id.empty() : detail::is_sha256_identity(d.previous_id));
    require((d.revision == 1) == (d.state == State::Reserved));
    require(d.artifacts.is_object());
    detail::reject_unknown_fields(d.artifacts, "Child synthesis artifacts",
                                  {"bundle", "validation", "evidence", "admission", "version",
                                   "receipt", "module", "link", "child_run_id", "child_input"});
    const bool failure = d.state == State::Failed || d.state == State::ReconciliationRequired;
    require(failure == !d.error_code.empty());
    if (failure) detail::validate_token(d.error_code, "Child synthesis error");
    const auto level = static_cast<int>(d.state);
    const auto has   = [&](const char* field, State first) {
        const bool present = d.artifacts.contains(field);
        if (!failure) require(present == (level >= static_cast<int>(first)));
        return present;
    };
    std::optional<ProgramBundle> bundle;
    if (has("bundle", State::Compiled)) {
        bundle = ProgramBundle::parse(encoded(d.artifacts.at("bundle")));
        require(bundle->source_hash() == d.proposal.source().source_hash());
        require(bundle->registry_snapshot_fingerprint() ==
                authorization.data().registry_fingerprint);
        require(execution_guarantee_rank(bundle->execution_guarantee()) >=
                execution_guarantee_rank(authorization.data().minimum_execution_guarantee));
        const auto& closure = bundle->capability_effect_closure();
        const auto& request = d.proposal.data();
        require(std::includes(request.requested_capabilities.begin(),
                              request.requested_capabilities.end(), closure.capabilities.begin(),
                              closure.capabilities.end()));
        require(std::includes(request.requested_effects.begin(), request.requested_effects.end(),
                              closure.effects.begin(), closure.effects.end()));
    }
    if (has("validation", State::Validated)) {
        require(bundle.has_value() && d.artifacts.contains("evidence"));
        const auto receipt =
            ProgramSynthesisValidationReceipt::parse(encoded(d.artifacts.at("validation")));
        const auto& r = receipt.data();
        require(r.proposal_id == d.proposal.id() && r.reservation_id == d.reservation.id() &&
                r.bundle_id == bundle->id() &&
                (r.accepted ||
                 (d.state == State::Failed && d.error_code == "P_CHILD_SYNTHESIS_REJECTED")) &&
                r.validator_identity == d.grant.data().semantic_validator_identity &&
                r.contract_identity == d.grant.data().semantic_contract_identity);
        validate_program_synthesis_validation_evidence(receipt, d.artifacts.at("evidence"));
    }
    require(has("evidence", State::Validated) == d.artifacts.contains("validation"));
    if (has("admission", State::Admitting)) {
        require(d.artifacts.contains("validation"));
        const auto& a = d.artifacts.at("admission");
        detail::reject_unknown_fields(a, "Frozen synthesis admission",
                                      {"owner_scope", "profile", "policy", "dependency_receipts"});
        require(a.at("owner_scope") == d.proposal.data().owner_scope);
        (void)AdmissionProfile::parse(encoded(a.at("profile")));
        auto policy = PolicySnapshot::parse(encoded(a.at("policy")));
        require(policy.registry_fingerprint() == authorization.data().registry_fingerprint);
        auto                     capabilities = policy.allowed_capabilities();
        auto                     effects      = policy.allowed_effects();
        auto                     modules      = policy.allowed_module_digests();
        std::vector<std::string> source_modules;
        for (const auto& import : d.proposal.source().imports())
            source_modules.push_back(import.content_identity);
        std::sort(capabilities.begin(), capabilities.end());
        std::sort(effects.begin(), effects.end());
        std::sort(modules.begin(), modules.end());
        std::sort(source_modules.begin(), source_modules.end());
        const auto& request = d.proposal.data();
        require(std::includes(request.requested_capabilities.begin(),
                              request.requested_capabilities.end(), capabilities.begin(),
                              capabilities.end()));
        require(std::includes(request.requested_effects.begin(), request.requested_effects.end(),
                              effects.begin(), effects.end()));
        require(std::includes(source_modules.begin(), source_modules.end(), modules.begin(),
                              modules.end()));
        require(a.at("dependency_receipts").is_array());
        for (const auto& r : a.at("dependency_receipts")) {
            detail::reject_unknown_fields(r, "Synthesis dependency",
                                          {"dependency_id", "content_identity"});
            detail::validate_token(r.at("dependency_id").get<std::string>(),
                                   "Synthesis dependency");
            require(detail::is_sha256_identity(r.at("content_identity").get<std::string>()));
        }
    }
    if (has("version", State::Admitted)) {
        require(bundle.has_value() && d.artifacts.contains("admission") &&
                d.artifacts.contains("receipt"));
        const auto version = ProgramVersion::parse(encoded(d.artifacts.at("version")));
        const auto receipt = ProgramSynthesisReceipt::parse(encoded(d.artifacts.at("receipt")));
        require(version.ownership_scope() == d.proposal.data().owner_scope &&
                version.bundle_id() == bundle->id());
        require(receipt.data().proposal_id == d.proposal.id() &&
                receipt.data().reservation_id == d.reservation.id() &&
                receipt.data().bundle_id == bundle->id() &&
                receipt.data().program_version_id == version.id() &&
                receipt.data().policy_snapshot_fingerprint ==
                    version.policy_snapshot().fingerprint());
        require(encoded(d.artifacts.at("admission").at("policy")) ==
                version.policy_snapshot().serialize_canonical());
        require(encoded(d.artifacts.at("admission").at("profile")) ==
                version.admission_profile().serialize_canonical());
        const auto& dependencies = d.artifacts.at("admission").at("dependency_receipts");
        require(dependencies.size() == version.dependency_receipts().size());
        for (const auto& dependency : version.dependency_receipts()) {
            bool found = false;
            for (const auto& stored : dependencies)
                if (stored.at("dependency_id") == dependency.dependency_id &&
                    stored.at("content_identity") == dependency.content_identity)
                    found = true;
            require(found);
        }
    }
    require(has("receipt", State::Admitted) == d.artifacts.contains("version"));
    if (has("module", State::Bound)) {
        require(bundle.has_value() && d.artifacts.contains("version") &&
                d.artifacts.contains("link"));
        const auto module  = ProgramModule::parse(encoded(d.artifacts.at("module")));
        const auto version = ProgramVersion::parse(encoded(d.artifacts.at("version")));
        const auto link    = ModuleLinkReceipt::parse(encoded(d.artifacts.at("link")));
        require(module.attestation_id() == authorization.id());
        const auto& b = d.proposal.data().requested_budget;
        require(b.max_dynamic_compiles <= UINT32_MAX);
        require(link.budget() == BudgetLimits{b.wall_time_ms, b.model_tokens, b.monetary_microunits,
                                              b.max_concurrency, b.max_program_operations,
                                              b.max_core_steps,
                                              static_cast<std::uint32_t>(b.max_dynamic_compiles),
                                              b.max_child_depth, b.max_total_children});
        ModuleResolution resolution{module.coordinate(), {module}, {}};
        require(link_module_child(resolution, module, d.binding_name, *bundle, version).id() ==
                link.id());
    }
    require(has("link", State::Bound) == d.artifacts.contains("module"));
    if (has("child_run_id", State::Dispatching)) {
        require(d.artifacts.contains("link") && d.artifacts.contains("child_input"));
        detail::validate_token(d.artifacts.at("child_run_id").get<std::string>(),
                               "Synthesized child run");
    }
    require(has("child_input", State::Dispatching) == d.artifacts.contains("child_run_id"));
}
}  // namespace
struct ProgramChildSynthesisRecord::Impl {
    ProgramChildSynthesisRecordData data;
    std::string                     id, canonical;
};
ProgramChildSynthesisRecord::ProgramChildSynthesisRecord(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}
ProgramChildSynthesisRecord ProgramChildSynthesisRecord::create(
    ProgramChildSynthesisRecordData data) {
    data.artifacts = nested(encoded(data.artifacts));
    validate(data);
    auto value  = body(data);
    auto id     = detail::sha256_identity("program-child-synthesis-record/v1", encoded(value));
    value["id"] = id;
    auto bytes  = encoded(value);
    require(bytes.size() <= MAX_RECORD_BYTES);
    return ProgramChildSynthesisRecord(
        std::make_shared<const Impl>(Impl{std::move(data), std::move(id), std::move(bytes)}));
}
ProgramChildSynthesisRecord ProgramChildSynthesisRecord::parse(std::string_view bytes) {
    require(bytes.size() <= MAX_RECORD_BYTES);
    auto       v          = nested(bytes);
    const auto state_name = v.at("state").get<std::string>();
    const auto state      = std::find(names.begin(), names.end(), state_name);
    require(state != names.end());
    require(v.at("revision").is_number_unsigned() ||
            (v.at("revision").is_number_integer() && v.at("revision").get<std::int64_t>() > 0));
    auto result = create({ProgramSynthesisProposal::parse(encoded(v.at("proposal"))),
                          ProgramChildSynthesisGrant::parse(encoded(v.at("grant"))),
                          {ProgramVersion::parse(encoded(v.at("parent_version"))),
                           ProgramRunRecord::parse(encoded(v.at("parent_run"))),
                           ProgramRunLineage::parse(encoded(v.at("parent_lineage"))),
                           ProgramRunGeneration::parse(encoded(v.at("parent_generation")))},
                          ProgramSynthesisReservation::parse(encoded(v.at("reservation"))),
                          v.at("binding_name").get<std::string>(),
                          v.at("revision").get<std::uint64_t>(),
                          v.at("previous_id").get<std::string>(),
                          static_cast<State>(state - names.begin()),
                          v.at("artifacts"),
                          v.at("error_code").get<std::string>()});
    // Exact canonical comparison also rejects unknown fields and numeric normalization.
    require(result.serialize_canonical() == encoded(v));
    return result;
}
const ProgramChildSynthesisRecordData& ProgramChildSynthesisRecord::data() const noexcept {
    return impl_->data;
}
const std::string& ProgramChildSynthesisRecord::id() const noexcept {
    return impl_->id;
}
std::string ProgramChildSynthesisRecord::serialize_canonical() const {
    return impl_->canonical;
}

bool is_valid_program_child_synthesis_append(const std::vector<ProgramChildSynthesisRecord>& heads,
                                             const ProgramChildSynthesisRecord& next) noexcept {
    try {
        const auto&                        n   = next.data();
        const ProgramChildSynthesisRecord* old = nullptr;
        for (const auto& head : heads) {
            if (head.data().proposal.id() == n.proposal.id())
                old = &head;
            else if (head.data().binding_name == n.binding_name)
                return false;
        }
        if (!old) return n.state == State::Reserved && n.revision == 1;
        const auto& p = old->data();
        if (p.state >= State::Spawned || n.previous_id != old->id() || n.revision != p.revision + 1)
            return false;
        auto previous = body(p), current = body(n);
        for (const auto* key : {"revision", "previous_id", "state", "artifacts", "error_code"}) {
            previous[key] = nullptr;
            current[key]  = nullptr;
        }
        if (previous != current) return false;
        for (auto it = p.artifacts.begin(); it != p.artifacts.end(); ++it)
            if (!n.artifacts.contains(it.key()) ||
                encoded(n.artifacts.at(it.key())) != encoded(it.value()))
                return false;
        if (n.state == State::Failed || n.state == State::ReconciliationRequired) {
            if (p.artifacts == n.artifacts) return true;
            return n.state == State::Failed && p.state == State::Validating &&
                   n.error_code == "P_CHILD_SYNTHESIS_REJECTED" &&
                   n.artifacts.size() == p.artifacts.size() + 2 &&
                   n.artifacts.contains("validation") && n.artifacts.contains("evidence");
        }
        return static_cast<int>(n.state) == static_cast<int>(p.state) + 1;
    } catch (...) {
        return false;
    }
}

bool is_valid_program_child_synthesis_publication(
    const std::vector<ProgramChildSynthesisRecord>& heads,
    const ProgramRunRecord*                         previous_run,
    const ProgramTransitionPublication&             publication) noexcept {
    try {
        if (publication.child_synthesis_records.empty()) return true;
        if (!previous_run || publication.child_synthesis_records.size() != 1 ||
            !publication.run_lineage || publication.run_generation ||
            publication.fork_source_lineage || publication.migration_plan)
            return false;
        const auto& record = publication.child_synthesis_records.front();
        const auto& d      = record.data();
        const auto& run    = publication.run_record;
        if (!is_valid_program_child_synthesis_append(heads, record) ||
            run.owner_scope() != d.proposal.data().owner_scope ||
            run.run_id() != d.parent.run.run_id() ||
            run.program_version_id() != d.parent.version.id() ||
            publication.run_lineage->active_generation_id() != d.parent.generation.id() ||
            run.continuation().state != ContinuationState::Running ||
            previous_run->continuation().state != ContinuationState::Running)
            return false;
        if (d.state == State::Reserved) {
            return previous_run->id() == d.parent.run.id() &&
                   d.reservation.data().reserved_lineage_head_id == publication.run_lineage->id() &&
                   d.reservation.data().remaining_after_reservation == run.remaining_budget();
        }
        // Stage completion never mints a fresh compile allowance.
        return run.remaining_budget().max_dynamic_compiles ==
               previous_run->remaining_budget().max_dynamic_compiles;
    } catch (...) {
        return false;
    }
}
}  // namespace neograph::program
