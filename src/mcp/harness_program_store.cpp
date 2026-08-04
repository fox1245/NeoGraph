#include <neograph/mcp/harness.h>
#include <neograph/mcp/harness_program_store.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace neograph::mcp {
namespace {

constexpr std::string_view kArtifactFormat = "neograph-harness-program-adapter-artifact";
constexpr std::string_view kRunFormat      = "neograph-harness-program-adapter-run";

void reject_unknown_fields(const json&                             value,
                           std::string_view                        context,
                           std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) {
        throw std::invalid_argument(std::string(context) + " must be an object");
    }
    for (const auto& [key, ignored] : value.items()) {
        (void)ignored;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            throw std::invalid_argument(std::string(context) + " contains unknown field '" + key +
                                        "'");
        }
    }
}

std::string require_string(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_string()) {
        throw std::invalid_argument("Harness Program adapter field '" + owned_key +
                                    "' must be a string");
    }
    auto result = value[owned_key].get<std::string>();
    if (result.empty()) {
        throw std::invalid_argument("Harness Program adapter field '" + owned_key +
                                    "' must not be empty");
    }
    return result;
}

void require_schema_version(const json& value) {
    if (!value.contains("storage_schema_version") ||
        !value["storage_schema_version"].is_number_unsigned()) {
        throw std::invalid_argument(
            "Harness Program adapter storage_schema_version must be unsigned");
    }
    const auto encoded = value["storage_schema_version"].get<std::uint64_t>();
    if (encoded != HarnessProgramArtifactRecord::STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Harness Program adapter schema version is unsupported");
    }
}

bool is_program_identity(std::string_view value) noexcept {
    if (value.size() != 71 || !value.starts_with("sha256:")) return false;
    return std::all_of(value.begin() + 7, value.end(), [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

json owned_json(const json& value) {
    return json::parse(value.dump());
}
bool budget_within_template(const program::RunBudget& requested,
                            const program::RunBudget& ceiling) noexcept {
    return requested.wall_time_ms <= ceiling.wall_time_ms &&
           requested.model_tokens <= ceiling.model_tokens &&
           requested.monetary_microunits <= ceiling.monetary_microunits &&
           requested.max_concurrency <= ceiling.max_concurrency &&
           requested.max_program_operations <= ceiling.max_program_operations &&
           requested.max_core_steps <= ceiling.max_core_steps &&
           requested.max_dynamic_compiles <= ceiling.max_dynamic_compiles &&
           requested.max_child_depth <= ceiling.max_child_depth &&
           requested.max_total_children <= ceiling.max_total_children;
}

json materialization_json(const program::CoreMaterializationReceipt& receipt) {
    json plans = json::array();
    for (const auto& plan : receipt.plans) {
        plans.push_back(
            json{{"name", plan.name}, {"compiled_plan_identity", plan.compiled_plan_identity}});
    }
    json bindings = json::array();
    for (const auto& binding : receipt.capability_bindings) {
        bindings.push_back(
            json{{"executable",
                  json{{"kind", std::string(program::to_string(binding.executable.kind))},
                       {"name", binding.executable.name},
                       {"semantic_version", binding.executable.semantic_version},
                       {"implementation_digest", binding.executable.implementation_digest}}},
                 {"binding_identity", binding.binding_identity}});
    }
    return json{
        {"compiler_build_id", receipt.compiler_build_id},
        {"registry_snapshot_fingerprint", receipt.registry_snapshot_fingerprint},
        {"plans", std::move(plans)},
        {"capability_bindings", std::move(bindings)},
        {"capability_binding_receipt_root",
         program::capability_binding_receipt_root(receipt.capability_bindings)},
    };
}

void validate_exact_bindings(const std::string&             owner_scope,
                             const program::ProgramBundle&  bundle,
                             const program::ProgramVersion& version) {
    const auto  profile      = version.admission_profile();
    const auto  policy       = version.policy_snapshot();
    const auto& materialized = version.core_materialization_receipt();
    if (version.bundle_id() != bundle.id()) {
        throw std::invalid_argument("Harness Program adapter bundle/version binding mismatch");
    }
    if (version.ownership_scope() != owner_scope || policy.owner_scope() != owner_scope) {
        throw std::invalid_argument("Harness Program adapter owner binding mismatch");
    }
    if (profile.fingerprint() != policy.admission_profile_fingerprint()) {
        throw std::invalid_argument("Harness Program adapter admission binding mismatch");
    }
    if (bundle.registry_snapshot_fingerprint() != profile.registry_fingerprint() ||
        bundle.registry_snapshot_fingerprint() != policy.registry_fingerprint() ||
        bundle.registry_snapshot_fingerprint() != materialized.registry_snapshot_fingerprint) {
        throw std::invalid_argument("Harness Program adapter registry binding mismatch");
    }
    if (bundle.compiler_build_id() != materialized.compiler_build_id) {
        throw std::invalid_argument("Harness Program adapter compiler binding mismatch");
    }
    if (bundle.core_plan_identities() != materialized.plans) {
        throw std::invalid_argument("Harness Program adapter materialization binding mismatch");
    }
}

}  // namespace

struct HarnessProgramArtifactRecord::Impl {
    Impl(std::string               artifact,
         std::string               owner,
         program::ProgramBundle    program_bundle,
         program::ProgramVersion   program_version,
         HarnessInvocationTemplate request_template,
         json                      harness_projection)
        : artifact_id(std::move(artifact)),
          owner_scope(std::move(owner)),
          bundle(std::move(program_bundle)),
          version(std::move(program_version)),
          invocation_template(std::move(request_template)),
          projection(std::move(harness_projection)) {}

    std::string               artifact_id;
    std::string               owner_scope;
    program::ProgramBundle    bundle;
    program::ProgramVersion   version;
    HarnessInvocationTemplate invocation_template;
    json                      projection;
};

HarnessProgramArtifactRecord::HarnessProgramArtifactRecord(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

HarnessProgramArtifactRecord HarnessProgramArtifactRecord::create(
    std::string               artifact_id,
    std::string               owner_scope,
    program::ProgramBundle    bundle,
    program::ProgramVersion   version,
    HarnessInvocationTemplate invocation_template,
    json                      projection) {
    if (artifact_id.empty()) {
        throw std::invalid_argument("Harness Program adapter artifact_id must not be empty");
    }
    if (owner_scope.empty()) {
        throw std::invalid_argument("Harness Program adapter owner_scope must not be empty");
    }
    if (!projection.is_object()) {
        throw std::invalid_argument("Harness Program adapter projection must be an object");
    }
    (void)invocation_template.serialize_canonical();
    validate_exact_bindings(owner_scope, bundle, version);
    return HarnessProgramArtifactRecord(std::make_shared<const Impl>(
        std::move(artifact_id), std::move(owner_scope), std::move(bundle), std::move(version),
        std::move(invocation_template), owned_json(projection)));
}

HarnessProgramArtifactRecord HarnessProgramArtifactRecord::parse(const json& stored) {
    reject_unknown_fields(
        stored, "Stored Harness Program artifact",
        {"format", "storage_schema_version", "artifact_id", "owner_scope", "bundle", "bundle_id",
         "version", "version_id", "admission_profile_fingerprint", "policy_fingerprint",
         "registry_fingerprint", "compiler_build_id", "materialization_receipt",
         "invocation_template", "projection"});
    if (require_string(stored, "format") != kArtifactFormat) {
        throw std::invalid_argument("Stored Harness Program artifact format is unsupported");
    }
    require_schema_version(stored);
    const auto artifact_id   = require_string(stored, "artifact_id");
    const auto owner_scope   = require_string(stored, "owner_scope");
    const auto bundle_bytes  = require_string(stored, "bundle");
    const auto version_bytes = require_string(stored, "version");
    const auto bundle        = program::ProgramBundle::parse(bundle_bytes);
    const auto version       = program::ProgramVersion::parse(version_bytes);

    if (require_string(stored, "bundle_id") != bundle.id() ||
        require_string(stored, "version_id") != version.id()) {
        throw std::invalid_argument("Stored Harness Program artifact content identity mismatch");
    }
    const auto profile = version.admission_profile();
    const auto policy  = version.policy_snapshot();
    if (require_string(stored, "admission_profile_fingerprint") != profile.fingerprint() ||
        require_string(stored, "policy_fingerprint") != policy.fingerprint() ||
        require_string(stored, "registry_fingerprint") != bundle.registry_snapshot_fingerprint() ||
        require_string(stored, "compiler_build_id") != bundle.compiler_build_id()) {
        throw std::invalid_argument("Stored Harness Program artifact receipt mismatch");
    }
    if (!stored.contains("materialization_receipt") ||
        stored["materialization_receipt"] !=
            materialization_json(version.core_materialization_receipt())) {
        throw std::invalid_argument("Stored Harness Program materialization receipt mismatch");
    }
    if (!stored.contains("projection") || !stored["projection"].is_object()) {
        throw std::invalid_argument("Stored Harness Program projection must be an explicit object");
    }
    const auto invocation_template =
        HarnessInvocationTemplate::parse(require_string(stored, "invocation_template"));
    return create(artifact_id, owner_scope, bundle, version, invocation_template,
                  stored["projection"]);
}

const std::string& HarnessProgramArtifactRecord::artifact_id() const noexcept {
    return impl_->artifact_id;
}
const std::string& HarnessProgramArtifactRecord::owner_scope() const noexcept {
    return impl_->owner_scope;
}
const program::ProgramBundle& HarnessProgramArtifactRecord::bundle() const noexcept {
    return impl_->bundle;
}
const program::ProgramVersion& HarnessProgramArtifactRecord::version() const noexcept {
    return impl_->version;
}
const HarnessInvocationTemplate& HarnessProgramArtifactRecord::invocation_template() const noexcept {
    return impl_->invocation_template;
}
json HarnessProgramArtifactRecord::projection() const {
    return owned_json(impl_->projection);
}

json HarnessProgramArtifactRecord::serialize() const {
    validate_exact_bindings(impl_->owner_scope, impl_->bundle, impl_->version);
    const auto profile = impl_->version.admission_profile();
    const auto policy  = impl_->version.policy_snapshot();
    return json{{"format", std::string(kArtifactFormat)},
                {"storage_schema_version", STORAGE_SCHEMA_VERSION},
                {"artifact_id", impl_->artifact_id},
                {"owner_scope", impl_->owner_scope},
                {"bundle", impl_->bundle.serialize_canonical()},
                {"bundle_id", impl_->bundle.id()},
                {"version", impl_->version.serialize_canonical()},
                {"version_id", impl_->version.id()},
                {"admission_profile_fingerprint", profile.fingerprint()},
                {"policy_fingerprint", policy.fingerprint()},
                {"registry_fingerprint", impl_->bundle.registry_snapshot_fingerprint()},
                {"compiler_build_id", impl_->bundle.compiler_build_id()},
                {"materialization_receipt",
                 materialization_json(impl_->version.core_materialization_receipt())},
                {"invocation_template", impl_->invocation_template.serialize_canonical()},
                {"projection", owned_json(impl_->projection)}};
}

struct HarnessProgramRunRecord::Impl {
    Impl(program::ProgramRunRecord record, program::RunInvocation request)
        : run_record(std::move(record)), invocation(std::move(request)) {}
    std::string               artifact_id;
    std::string               owner_scope;
    program::ProgramRunRecord run_record;
    program::RunInvocation    invocation;
    std::string               admission_profile_fingerprint;
    std::string               policy_fingerprint;
    std::string               registry_fingerprint;
    std::string               compiler_build_id;
    json                      materialization_receipt;
    json                      projection;
};

HarnessProgramRunRecord::HarnessProgramRunRecord(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

HarnessProgramRunRecord HarnessProgramRunRecord::create(
    const HarnessProgramArtifactRecord& artifact,
    program::ProgramRunRecord           run_record,
    json                                projection) {
    if (!projection.is_object()) {
        throw std::invalid_argument("Harness Program run projection must be an object");
    }
    const auto invocation = run_record.invocation();
    invocation.validate();
    if (run_record.owner_scope() != artifact.owner_scope() ||
        run_record.bundle_id() != artifact.bundle().id() ||
        run_record.program_version_id() != artifact.version().id()) {
        throw std::invalid_argument("Harness Program run/artifact binding mismatch");
    }
    const auto profile = artifact.version().admission_profile();
    const auto policy  = artifact.version().policy_snapshot();
    auto impl          = std::make_shared<Impl>(std::move(run_record), invocation);
    impl->artifact_id  = artifact.artifact_id();
    impl->owner_scope  = artifact.owner_scope();
    impl->admission_profile_fingerprint = profile.fingerprint();
    impl->policy_fingerprint            = policy.fingerprint();
    impl->registry_fingerprint          = artifact.bundle().registry_snapshot_fingerprint();
    impl->compiler_build_id             = artifact.bundle().compiler_build_id();
    impl->materialization_receipt =
        materialization_json(artifact.version().core_materialization_receipt());
    impl->projection = owned_json(projection);
    HarnessProgramRunRecord record(std::move(impl));
    record.validate_artifact(artifact);
    return record;
}

HarnessProgramRunRecord HarnessProgramRunRecord::parse(const json& stored) {
    reject_unknown_fields(
        stored, "Stored Harness Program run",
        {"format", "storage_schema_version", "artifact_id", "owner_scope", "program_run",
         "program_run_id", "run_id", "bundle_id", "version_id", "binding_fingerprint",
         "admission_profile_fingerprint", "policy_fingerprint", "registry_fingerprint",
         "compiler_build_id", "materialization_receipt", "invocation", "projection"});
    if (require_string(stored, "format") != kRunFormat) {
        throw std::invalid_argument("Stored Harness Program run format is unsupported");
    }
    require_schema_version(stored);
    const auto run_record = program::ProgramRunRecord::parse(require_string(stored, "program_run"));
    const auto invocation = program::RunInvocation::parse(require_string(stored, "invocation"));
    if (require_string(stored, "program_run_id") != run_record.id() ||
        require_string(stored, "owner_scope") != run_record.owner_scope() ||
        require_string(stored, "run_id") != run_record.run_id() ||
        require_string(stored, "bundle_id") != run_record.bundle_id() ||
        require_string(stored, "version_id") != run_record.program_version_id() ||
        require_string(stored, "binding_fingerprint") != run_record.binding_fingerprint() ||
        run_record.invocation() != invocation) {
        throw std::invalid_argument("Stored Harness Program run content identity mismatch");
    }
    if (!stored.contains("materialization_receipt") ||
        !stored["materialization_receipt"].is_object() || !stored.contains("projection") ||
        !stored["projection"].is_object()) {
        throw std::invalid_argument(
            "Stored Harness Program run requires explicit receipt and projection");
    }
    auto impl                           = std::make_shared<Impl>(run_record, invocation);
    impl->artifact_id                   = require_string(stored, "artifact_id");
    impl->owner_scope                   = run_record.owner_scope();
    impl->admission_profile_fingerprint = require_string(stored, "admission_profile_fingerprint");
    impl->policy_fingerprint            = require_string(stored, "policy_fingerprint");
    impl->registry_fingerprint          = require_string(stored, "registry_fingerprint");
    impl->compiler_build_id             = require_string(stored, "compiler_build_id");
    impl->materialization_receipt       = owned_json(stored["materialization_receipt"]);
    impl->projection                    = owned_json(stored["projection"]);
    return HarnessProgramRunRecord(std::move(impl));
}

const std::string& HarnessProgramRunRecord::artifact_id() const noexcept {
    return impl_->artifact_id;
}
const std::string& HarnessProgramRunRecord::owner_scope() const noexcept {
    return impl_->owner_scope;
}
const program::ProgramRunRecord& HarnessProgramRunRecord::run_record() const noexcept {
    return impl_->run_record;
}
const program::RunInvocation& HarnessProgramRunRecord::invocation() const noexcept {
    return impl_->invocation;
}
json HarnessProgramRunRecord::projection() const {
    return owned_json(impl_->projection);
}

void HarnessProgramRunRecord::validate_artifact(
    const HarnessProgramArtifactRecord& artifact) const {
    const auto profile = artifact.version().admission_profile();
    const auto policy  = artifact.version().policy_snapshot();
    auto expected = bind_harness_invocation(
        artifact.invocation_template(), artifact.owner_scope(), artifact.version().id(),
        impl_->run_record.run_id(), impl_->invocation.correlation_id);
    if (impl_->run_record.fork_receipt()) {
        if (!budget_within_template(impl_->invocation.budget, expected.budget)) {
            throw std::invalid_argument(
                "Harness Program fork invocation exceeds its artifact template budget");
        }
        // ProgramRuntime derives an exact fork continuation budget from the
        // source remainder before publishing its immutable migration receipt.
        // The artifact template remains the upper authority bound.
        expected.budget = impl_->invocation.budget;
    }
    if (impl_->artifact_id != artifact.artifact_id() ||
        impl_->owner_scope != artifact.owner_scope() ||
        impl_->run_record.bundle_id() != artifact.bundle().id() ||
        impl_->run_record.program_version_id() != artifact.version().id() ||
        impl_->admission_profile_fingerprint != profile.fingerprint() ||
        impl_->policy_fingerprint != policy.fingerprint() ||
        impl_->registry_fingerprint != artifact.bundle().registry_snapshot_fingerprint() ||
        impl_->compiler_build_id != artifact.bundle().compiler_build_id() ||
        impl_->materialization_receipt !=
            materialization_json(artifact.version().core_materialization_receipt()) ||
        impl_->invocation != impl_->run_record.invocation() ||
        impl_->invocation != expected) {
        throw std::invalid_argument("Harness Program run retained artifact binding mismatch");
    }
}

json HarnessProgramRunRecord::serialize() const {
    return json{
        {"format", std::string(kRunFormat)},
        {"storage_schema_version", STORAGE_SCHEMA_VERSION},
        {"artifact_id", impl_->artifact_id},
        {"owner_scope", impl_->owner_scope},
        {"program_run", impl_->run_record.serialize_canonical()},
        {"program_run_id", impl_->run_record.id()},
        {"run_id", impl_->run_record.run_id()},
        {"bundle_id", impl_->run_record.bundle_id()},
        {"version_id", impl_->run_record.program_version_id()},
        {"binding_fingerprint", impl_->run_record.binding_fingerprint()},
        {"admission_profile_fingerprint", impl_->admission_profile_fingerprint},
        {"policy_fingerprint", impl_->policy_fingerprint},
        {"registry_fingerprint", impl_->registry_fingerprint},
        {"compiler_build_id", impl_->compiler_build_id},
        {"materialization_receipt", owned_json(impl_->materialization_receipt)},
        {"invocation", impl_->invocation.serialize_canonical()},
        {"projection", owned_json(impl_->projection)},
    };
}

std::shared_ptr<HarnessProgramAdapterStore> require_harness_program_adapter_store(
    const std::shared_ptr<HarnessRecordStore>& records) {
    if (!records) {
        throw std::invalid_argument("Harness Program durable adapter requires a record store");
    }
    auto capability = std::dynamic_pointer_cast<HarnessProgramAdapterStore>(records);
    if (!capability) {
        throw std::invalid_argument(
            "Harness Program durable reconnect and host resume require atomic transition storage");
    }
    return capability;
}

struct HarnessBoundedProgramStore::Impl {
    Impl(std::shared_ptr<HarnessRecordStore> store,
         std::string                         artifact,
         std::string                         owner,
         HarnessInvocationTemplate           request_template,
         json                                harness_projection)
        : records(std::move(store)),
          artifact_id(std::move(artifact)),
          owner_scope(std::move(owner)),
          invocation_template(std::move(request_template)),
          projection(owned_json(harness_projection)) {}

    std::shared_ptr<HarnessRecordStore> records;
    std::string                         artifact_id;
    std::string                         owner_scope;
    HarnessInvocationTemplate           invocation_template;
    json                                projection;
};

HarnessBoundedProgramStore::HarnessBoundedProgramStore(
    std::shared_ptr<HarnessRecordStore> records,
    std::string                         artifact_id,
    std::string                         owner_scope,
    HarnessInvocationTemplate           invocation_template,
    json                                projection) {
    if (!records) throw std::invalid_argument("Harness Program adapter requires a record store");
    if (artifact_id.empty() || owner_scope.empty()) {
        throw std::invalid_argument("Harness Program adapter requires artifact_id and owner_scope");
    }
    if (!projection.is_object()) {
        throw std::invalid_argument("Harness Program adapter projection must be an object");
    }
    (void)invocation_template.serialize_canonical();
    impl_ = std::make_unique<Impl>(std::move(records), std::move(artifact_id),
                                   std::move(owner_scope), std::move(invocation_template),
                                   std::move(projection));
}

HarnessBoundedProgramStore::~HarnessBoundedProgramStore() = default;
HarnessBoundedProgramStore::HarnessBoundedProgramStore(HarnessBoundedProgramStore&&) noexcept =
    default;
HarnessBoundedProgramStore& HarnessBoundedProgramStore::operator=(
    HarnessBoundedProgramStore&&) noexcept = default;

void HarnessBoundedProgramStore::publish_admitted(const program::ProgramBundle&  bundle,
                                                  const program::ProgramVersion& version) {
    const auto record = HarnessProgramArtifactRecord::create(
        impl_->artifact_id, impl_->owner_scope, bundle, version, impl_->invocation_template,
        impl_->projection);
    impl_->records->save_artifact(impl_->artifact_id, record.serialize());
}

std::optional<HarnessProgramArtifactRecord> HarnessBoundedProgramStore::load_artifact() const {
    const auto stored = impl_->records->load_artifact(impl_->artifact_id);
    if (!stored) return std::nullopt;
    if (stored->is_object() && stored->contains("owner_scope") &&
        (*stored)["owner_scope"].is_string() &&
        (*stored)["owner_scope"].get<std::string>() != impl_->owner_scope) {
        return std::nullopt;
    }
    auto record = HarnessProgramArtifactRecord::parse(*stored);
    if (record.owner_scope() != impl_->owner_scope) return std::nullopt;
    return record;
}

std::optional<program::ProgramBundle> HarnessBoundedProgramStore::get_bundle(
    std::string_view id) const {
    const auto record = load_artifact();
    if (!record || record->bundle().id() != id) return std::nullopt;
    return record->bundle();
}

std::optional<program::ProgramVersion> HarnessBoundedProgramStore::get_version(
    std::string_view id) const {
    const auto record = load_artifact();
    if (!record || record->version().id() != id) return std::nullopt;
    return record->version();
}

std::optional<program::ProgramBundle> HarnessBoundedProgramStore::get_bundle(
    std::string_view owner_scope, std::string_view id) const {
    if (owner_scope != impl_->owner_scope) return std::nullopt;
    return get_bundle(id);
}

std::optional<program::ProgramVersion> HarnessBoundedProgramStore::get_version(
    std::string_view owner_scope, std::string_view id) const {
    if (owner_scope != impl_->owner_scope) return std::nullopt;
    return get_version(id);
}

struct HarnessBoundedProgramJournal::Impl {
    Impl(std::shared_ptr<program::ProgramTransitionStore> store, std::string owner)
        : transitions(std::move(store)), owner_scope(std::move(owner)) {}
    std::shared_ptr<program::ProgramTransitionStore> transitions;
    std::string                                      owner_scope;
};

HarnessBoundedProgramJournal::HarnessBoundedProgramJournal(
    std::shared_ptr<program::ProgramTransitionStore> transitions, std::string owner_scope) {
    if (!transitions || owner_scope.empty()) {
        throw std::invalid_argument(
            "Harness bounded ProgramJournal requires transitions and owner scope");
    }
    impl_ = std::make_unique<Impl>(std::move(transitions), std::move(owner_scope));
}

HarnessBoundedProgramJournal::~HarnessBoundedProgramJournal() = default;

std::optional<program::ProgramJournalRecord> HarnessBoundedProgramJournal::latest(
    std::string_view run_id) const {
    return impl_->transitions->latest(impl_->owner_scope, run_id);
}

program::JournalAppendResult HarnessBoundedProgramJournal::compare_append(
    std::string_view expected_previous_id, program::ProgramJournalRecord record) {
    std::string canonical_bytes;
    try {
        if (!expected_previous_id.empty() && !is_program_identity(expected_previous_id)) {
            return program::JournalAppendResult::Conflict;
        }
        canonical_bytes = record.serialize_canonical();
    } catch (const std::invalid_argument&) {
        return program::JournalAppendResult::Conflict;
    }
    const auto existing = impl_->transitions->latest(impl_->owner_scope, record.run_id);
    if (existing && existing->id == record.id) {
        return expected_previous_id == record.previous_id &&
                       existing->serialize_canonical() == canonical_bytes
                   ? program::JournalAppendResult::AlreadyPresent
                   : program::JournalAppendResult::Conflict;
    }
    const auto current = impl_->transitions->load(impl_->owner_scope, record.run_id);
    if (!current) return program::JournalAppendResult::Conflict;

    program::ProgramRunRecordData next_data;
    next_data.owner_scope         = current->owner_scope();
    next_data.run_id              = current->run_id();
    next_data.program_version_id  = current->program_version_id();
    next_data.bundle_id           = current->bundle_id();
    next_data.binding_fingerprint = current->binding_fingerprint();
    next_data.invocation          = current->invocation();
    next_data.continuation        = record.continuation;
    next_data.remaining_budget    = record.remaining_budget;
    next_data.exact_checkpoint    = record.core_checkpoint;
    next_data.pending_input       = current->pending_input();
    next_data.pending_effect      = current->pending_effect();
    next_data.terminal_result     = current->terminal_result();
    next_data.fork_receipt        = current->fork_receipt();
    next_data.journal_head        = record.id;
    next_data.event_sequence      = current->event_sequence();
    next_data.effect_sequence     = current->effect_sequence();
    next_data.created_at_ms       = current->created_at_ms();
    next_data.updated_at_ms       = record.timestamp_ms;

    program::ProgramTransitionPublication publication{
        program::ProgramRunRecord::create(std::move(next_data)),
        std::move(record),
        {},
        {},
    };
    switch (impl_->transitions->compare_publish(impl_->owner_scope, expected_previous_id,
                                                std::move(publication))) {
        case program::ProgramTransitionPublishResult::Published:
            return program::JournalAppendResult::Appended;
        case program::ProgramTransitionPublishResult::AlreadyPresent:
            return program::JournalAppendResult::AlreadyPresent;
        case program::ProgramTransitionPublishResult::Conflict:
            return program::JournalAppendResult::Conflict;
    }
    return program::JournalAppendResult::Conflict;
}
}  // namespace neograph::mcp
