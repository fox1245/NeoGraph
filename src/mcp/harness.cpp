#include <neograph/mcp/harness.h>
#include <neograph/mcp/harness_program_store.h>
#include <neograph/mcp/server.h>
#include <neograph/program/diagnostic.h>
#include <neograph/program/event.h>
#include <neograph/program/result.h>
#include "../program/canonical_json.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <variant>

namespace neograph::mcp {
namespace {
std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

json budget_json(const program::RunBudget& v) {
    return {{"wall_time_ms", v.wall_time_ms},
            {"model_tokens", v.model_tokens},
            {"monetary_microunits", v.monetary_microunits},
            {"max_concurrency", v.max_concurrency},
            {"max_program_operations", v.max_program_operations},
            {"max_core_steps", v.max_core_steps},
            {"max_dynamic_compiles", v.max_dynamic_compiles},
            {"max_child_depth", v.max_child_depth},
            {"max_total_children", v.max_total_children}};
}
program::RunBudget parse_budget(const json& v) {
    return {v.at("wall_time_ms").get<std::uint64_t>(),
            v.at("model_tokens").get<std::uint64_t>(),
            v.at("monetary_microunits").get<std::uint64_t>(),
            v.at("max_concurrency").get<std::uint32_t>(),
            v.at("max_program_operations").get<std::uint64_t>(),
            v.at("max_core_steps").get<std::uint64_t>(),
            v.at("max_dynamic_compiles").get<std::uint64_t>(),
            v.at("max_child_depth").get<std::uint32_t>(),
            v.at("max_total_children").get<std::uint64_t>()};
}
program::RunBudget bounded_budget(const program::RunBudget& requested,
                                  const program::RunBudget& ceiling) {
    return {std::min(requested.wall_time_ms, ceiling.wall_time_ms),
            std::min(requested.model_tokens, ceiling.model_tokens),
            std::min(requested.monetary_microunits, ceiling.monetary_microunits),
            std::min(requested.max_concurrency, ceiling.max_concurrency),
            std::min(requested.max_program_operations, ceiling.max_program_operations),
            std::min(requested.max_core_steps, ceiling.max_core_steps),
            std::min(requested.max_dynamic_compiles, ceiling.max_dynamic_compiles),
            std::min(requested.max_child_depth, ceiling.max_child_depth),
            std::min(requested.max_total_children, ceiling.max_total_children)};
}
json invocation_json(const program::ProgramInvocation& v) {
    return {{"input", v.input}, {"budget", budget_json(v.budget)}, {"trace_id", v.trace_id}};
}
program::ProgramInvocation parse_invocation(const json& v) {
    program::ProgramInvocation r;
    r.input    = v.at("input");
    r.budget   = parse_budget(v.at("budget"));
    r.trace_id = v.value("trace_id", "");
    return r;
}
json executable_json(const program::ExecutableIdentity& v) {
    return {{"kind", std::string(program::to_string(v.kind))},
            {"name", v.name},
            {"semantic_version", v.semantic_version},
            {"implementation_digest", v.implementation_digest}};
}
program::ExecutableIdentity parse_executable(const json& v) {
    return {program::executable_kind_from_string(v.at("kind").get<std::string>()),
            v.at("name").get<std::string>(), v.at("semantic_version").get<std::string>(),
            v.at("implementation_digest").get<std::string>()};
}
json bindings_json(const HarnessCapabilityBindingRequest& v) {
    json tools = json::array();
    for (const auto& t : v.tools)
        tools.push_back({{"executable", executable_json(t.executable)},
                         {"host_configuration", t.host_configuration}});
    json r = {{"tools", std::move(tools)}, {"policy_restrictions", v.policy_restrictions}};
    if (v.provider) r["provider"] = executable_json(*v.provider);
    return r;
}
HarnessCapabilityBindingRequest parse_bindings(const json& v) {
    HarnessCapabilityBindingRequest r;
    if (v.contains("provider")) r.provider = parse_executable(v.at("provider"));
    for (const auto& t : v.value("tools", json::array()))
        r.tools.push_back({parse_executable(t.at("executable")), t.at("host_configuration")});
    r.policy_restrictions = v.value("policy_restrictions", json::object());
    return r;
}
program::ProgramEffectReconciliation parse_reconciliation(std::string_view value) {
    if (value == "completed") return program::ProgramEffectReconciliation::Completed;
    if (value == "failed") return program::ProgramEffectReconciliation::Failed;
    if (value == "unknown") return program::ProgramEffectReconciliation::Unknown;
    throw std::invalid_argument("unsupported Harness effect reconciliation");
}
std::string alias(const program::ProgramBundle&                    bundle,
                  std::string                                      policy,
                  const HarnessCapabilityBindingRequest&           bindings,
                  std::string_view                                 artifact_binding_identity,
                  std::string_view                                 contract_hash = {}) {
    auto bundle_id = bundle.id();
    json binding_payload = {
        {"requested_bindings", bindings_json(bindings)},
        {"host_binding_identity", artifact_binding_identity},
    };
    if (!contract_hash.empty()) binding_payload["contract_hash"] = contract_hash;
    auto binding_id = program::detail::sha256_identity(
        "harness-artifact-binding/v1",
        program::detail::canonical_json_bytes(binding_payload));
    std::replace(bundle_id.begin(), bundle_id.end(), ':', '-');
    std::replace(policy.begin(), policy.end(), ':', '-');
    std::replace(binding_id.begin(), binding_id.end(), ':', '-');
    return "artifact-" + policy + "-" + bundle_id + "-" + binding_id;
}
std::string run_id(std::string_view policy) {
    std::random_device          random;
    std::array<unsigned int, 4> words{random(), random(), random(), random()};
    auto                        scope = policy.substr(policy.find(':') + 1, 12);
    std::ostringstream          out;
    out << "run-" << scope << '-' << std::hex << std::setfill('0');
    for (auto word : words)
        out << std::setw(8) << word;
    return out.str();
}
std::string status(program::ProgramTerminalStatus v) {
    switch (v) {
        case program::ProgramTerminalStatus::Completed:
            return "completed";
        case program::ProgramTerminalStatus::Interrupted:
            return "input_required";
        case program::ProgramTerminalStatus::Cancelled:
            return "cancelled";
        case program::ProgramTerminalStatus::BudgetExhausted:
            return "max_steps_exhausted";
        case program::ProgramTerminalStatus::TimedOut:
            return "timeout";
        case program::ProgramTerminalStatus::AmbiguousEffect:
            return "ambiguous_effect";
        case program::ProgramTerminalStatus::CheckpointIncompatible:
        case program::ProgramTerminalStatus::Failed:
            return "failed";
    }
    return "failed";
}
json checkpoint(const program::CoreCheckpointIdentity& v) {
    return {{"core_name", v.core_name},
            {"core_generation_id", v.core_generation_id},
            {"core_thread_id", v.core_thread_id},
            {"checkpoint_id", v.checkpoint_id},
            {"checkpoint_schema_version", v.checkpoint_schema_version}};
}
json project(const program::ProgramResult& v) {
    json r = {{"run_id", v.run_id()},
              {"status", status(v.status())},
              {"program_version_id", v.program_version_id()},
              {"bundle_id", v.bundle_id()},
              {"operation_id", v.operation_id()},
              {"attempt", v.attempt()},
              {"remaining_budget", budget_json(v.remaining_budget())}};
    if (v.status() == program::ProgramTerminalStatus::Completed) {
        const auto& output = v.output();
        if (output.is_object() && output.contains("channels") &&
            output.at("channels").is_object() && output.at("channels").contains("final_result") &&
            output.at("channels").at("final_result").is_object() &&
            output.at("channels").at("final_result").contains("value")) {
            r["result"] = output.at("channels").at("final_result").at("value");
        } else {
            r["result"] = output;
        }
    }
    if (auto c = v.checkpoint()) r["checkpoint"] = checkpoint(*c);
    if (auto i = v.interrupt()) {
        json pending;
        if (i->pending_input)
            pending = json::parse(i->pending_input->serialize_canonical());
        else if (i->pending_effect)
            pending = json::parse(i->pending_effect->serialize_canonical());
        else
            pending = i->value;
        pending["core_node"]            = i->core_node;
        pending["core_interrupt_value"] = i->value;
        r["pending"]                    = std::move(pending);
        if ((i->pending_effect &&
             i->pending_effect->state() == program::ProgramPendingState::Awaiting) ||
            (i->pending_input &&
             i->pending_input->state() == program::ProgramPendingState::Awaiting &&
             i->pending_input->kind() ==
                 program::ProgramPendingInputKind::CapabilityResult)) {
            r["status"] = "awaiting_tool_results";
        }
    }
    if (auto f = v.failure()) {
        r["error"]   = f->message;
        r["failure"] = {{"code", f->code},
                        {"message", f->message},
                        {"operation_id", f->operation_id},
                        {"core_node", f->core_node},
                        {"attempts", f->attempts},
                        {"witness", f->witness}};
    }
    return r;
}

json contract_projection(const program::ContractRun& run) {
    json result = {
        {"manifest_hash", run.manifest().content_hash()},
        {"manifest_lifecycle", std::string(program::to_string(run.manifest().lifecycle()))},
        {"status", std::string(program::to_string(run.status()))},
        {"attempt", run.attempt()},
        {"evidence", json::array()},
        {"diagnostics", json::array()},
        {"verification", nullptr},
    };
    for (const auto& evidence : run.evidence()) {
        result["evidence"].push_back({
            {"evidence_id", evidence.evidence_id},
            {"acceptance_id", evidence.acceptance_id},
            {"kind", std::string(program::to_string(evidence.kind))},
            {"manifest_hash", evidence.manifest_hash},
            {"program_version_id", evidence.program_version_id},
            {"workspace_revision", evidence.workspace_revision},
            {"executed", evidence.executed},
            {"passed", evidence.passed},
            {"diagnostics", evidence.diagnostics},
            {"details", evidence.details},
        });
    }
    for (const auto& diagnostic : run.diagnostics()) {
        result["diagnostics"].push_back({{"code", diagnostic.code},
                                          {"message", diagnostic.message},
                                          {"blocking", diagnostic.blocking}});
    }
    if (run.verification()) {
        const auto& verification = *run.verification();
        result["verification"] = {
            {"publishable", verification.publishable},
            {"status", std::string(program::to_string(verification.status))},
            {"missing_acceptance_ids", verification.missing_acceptance_ids},
            {"blocking_diagnostics", verification.blocking_diagnostics},
            {"failed_evidence_ids", verification.failed_evidence_ids},
        };
    }
    return result;
}

json runtime_final_value(const program::ProgramResult& result) {
    const auto& output = result.output();
    if (output.is_object() && output.contains("channels") &&
        output.at("channels").is_object() &&
        output.at("channels").contains("final_result") &&
        output.at("channels").at("final_result").is_object() &&
        output.at("channels").at("final_result").contains("value")) {
        return output.at("channels").at("final_result").at("value");
    }
    return output;
}

bool runtime_matches_expected(const json& actual, const json& expected) {
    // An empty expected object is an explicit presence/runtime-success gate;
    // non-empty expected values are exact runtime observations.
    return expected.is_object() && expected.empty() ? !actual.is_null() : actual == expected;
}

json project(const program::ProgramEvent& v) {
    json r = {{"sequence", v.sequence},
              {"timestamp_ms", v.timestamp_ms},
              {"run_id", v.run_id},
              {"program_version_id", v.program_version_id},
              {"bundle_id", v.bundle_id},
              {"operation_id", v.operation_id},
              {"core_generation_id", v.core_generation_id},
              {"core_run_id", v.core_run_id},
              {"trace_id", v.trace_id},
              {"attempt", v.attempt}};
    switch (v.kind) {
        case program::ProgramEventKind::Started:
            r["type"] = "run.started";
            break;
        case program::ProgramEventKind::Core:
            r["type"] = "core.event";
            break;
        case program::ProgramEventKind::CheckpointPublished:
            r["type"] = "checkpoint.published";
            r["checkpoint"] =
                checkpoint(std::get<program::ProgramCheckpointEvent>(v.payload).checkpoint);
            break;
        case program::ProgramEventKind::Terminal:
            r["type"]   = "run.terminal";
            r["status"] = status(std::get<program::ProgramTerminalEvent>(v.payload).status);
            break;
    }
    return r;
}
class EventProjection final : public program::ProgramEventSink {
public:
    explicit EventProjection(std::shared_ptr<HarnessJournal> j) : j_(std::move(j)) {}
    void on_event(const program::ProgramEvent& e) override {
        if (j_) j_->append_event(project(e));
    }

private:
    std::shared_ptr<HarnessJournal> j_;
};
class MemoryRecords final : public HarnessRecordStore, public HarnessProgramAdapterStore {
public:
    void save_artifact(const std::string& i, const json& v) override {
        std::lock_guard l(m_);
        auto [x, n] = a_.emplace(i, v);
        if (!n && x->second != v) throw std::runtime_error("immutable Harness artifact conflict");
    }
    std::optional<json> load_artifact(const std::string& i) override {
        std::lock_guard l(m_);
        auto            x = a_.find(i);
        return x == a_.end() ? std::nullopt : std::optional<json>(x->second);
    }
    void save_run(const std::string& i, const json& v) override {
        std::lock_guard l(m_);
        r_[i] = v;
    }
    std::optional<json> load_run(const std::string& i) override {
        std::lock_guard l(m_);
        auto            x = r_.find(i);
        return x == r_.end() ? std::nullopt : std::optional<json>(x->second);
    }
    std::shared_ptr<program::ProgramTransitionStore> bind_program_transitions(
        HarnessProgramArtifactRecord) override {
        std::lock_guard l(m_);
        if (!transitions_)
            transitions_ = std::make_shared<program::InMemoryProgramTransitionStore>();
        return transitions_;
    }
    std::optional<HarnessProgramRunRecord> resolve_program_run(std::string_view,
                                                               std::string_view) const override {
        return std::nullopt;
    }

private:
    mutable std::mutex                               m_;
    std::map<std::string, json>                      a_, r_;
    std::shared_ptr<program::ProgramTransitionStore> transitions_;
};
class ForkProgramStore final : public program::ProgramStore {
public:
    ForkProgramStore(std::shared_ptr<program::ProgramStore> target,
                     program::ProgramBundle                 source_bundle,
                     program::ProgramVersion                source_version)
        : target_(std::move(target)),
          source_bundle_(std::move(source_bundle)),
          source_version_(std::move(source_version)) {}
    void publish_admitted(const program::ProgramBundle&  bundle,
                          const program::ProgramVersion& version) override {
        target_->publish_admitted(bundle, version);
    }
    std::optional<program::ProgramBundle> get_bundle(std::string_view id) const override {
        if (id == source_bundle_.id()) return source_bundle_;
        return target_->get_bundle(id);
    }
    std::optional<program::ProgramVersion> get_version(std::string_view id) const override {
        if (id == source_version_.id()) return source_version_;
        return target_->get_version(id);
    }

private:
    std::shared_ptr<program::ProgramStore> target_;
    program::ProgramBundle                 source_bundle_;
    program::ProgramVersion                source_version_;
};
class ForkProgramTransitionStore final : public program::ProgramTransitionStore {
public:
    ForkProgramTransitionStore(
        std::string source_run_id,
        std::shared_ptr<program::ProgramTransitionStore> source,
        std::shared_ptr<program::ProgramTransitionStore> target)
        : source_run_id_(std::move(source_run_id)),
          source_(std::move(source)),
          target_(std::move(target)) {
        if (source_run_id_.empty() || !source_ || !target_) {
            throw std::invalid_argument("incomplete Program fork transition stores");
        }
    }

    std::optional<program::ProgramRunRecord> load(
        std::string_view owner_scope, std::string_view run_id) const override {
        return select(run_id).load(owner_scope, run_id);
    }
    std::optional<program::ProgramJournalRecord> latest(
        std::string_view owner_scope, std::string_view run_id) const override {
        return select(run_id).latest(owner_scope, run_id);
    }
    std::vector<program::ProgramEvent> load_events(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence) const override {
        return select(run_id).load_events(owner_scope, run_id, after_sequence);
    }
    std::vector<program::ProgramEffectOutboxEntry> load_effects(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence) const override {
        return select(run_id).load_effects(owner_scope, run_id, after_sequence);
    }
    std::optional<program::MigrationPlan> load_migration_plan(
        std::string_view owner_scope, std::string_view run_id) const override {
        return select(run_id).load_migration_plan(owner_scope, run_id);
    }
    program::ProgramTransitionPublishResult compare_publish(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        program::ProgramTransitionPublication publication) override {
        return target_->compare_publish(owner_scope, expected_journal_head,
                                        std::move(publication));
    }

private:
    program::ProgramTransitionStore& select(std::string_view run_id) const {
        return run_id == source_run_id_ ? *source_ : *target_;
    }

    std::string source_run_id_;
    std::shared_ptr<program::ProgramTransitionStore> source_;
    std::shared_ptr<program::ProgramTransitionStore> target_;
};

json diagnostics(const std::vector<program::Diagnostic>& values) {
    json result = json::array();
    for (const auto& diagnostic : values) {
        json encoded;
        program::to_json(encoded, diagnostic);
        result.push_back(std::move(encoded));
    }
    return result;
}
json source_map_json(const std::vector<program::SourceMapEntry>& entries) {
    json result = json::array();
    for (const auto& entry : entries) {
        json encoded;
        program::to_json(encoded, entry);
        result.push_back(std::move(encoded));
    }
    return result;
}
ToolDefinition tool_definition(std::string name,
                               std::string title,
                               std::string description,
                               json        input_schema,
                               json        output_schema,
                               bool        read_only = false) {
    ToolDefinition definition;
    definition.name          = std::move(name);
    definition.title         = std::move(title);
    definition.description   = std::move(description);
    definition.input_schema  = std::move(input_schema);
    definition.output_schema = std::move(output_schema);
    definition.annotations   = {{"readOnlyHint", read_only}};
    return definition;
}
CallToolResult mcp_result(json value, std::string message) {
    CallToolResult result;
    result.content            = json::array({{{"type", "text"}, {"text", std::move(message)}}});
    result.structured_content = std::move(value);
    return result;
}
json harness_preset_contracts() {
    return {
        {"fanout_judge",
         {{"mode", "preset"}, {"core_name", "harness_fanout_judge"},
          {"description", "Run bounded workers and aggregate their findings."}}},
        {"pr_review_panel",
         {{"mode", "preset"}, {"core_name", "harness_pr_review_panel"},
          {"description", "Run a read-only review panel with evidence requirements."}}},
        {"bug_triage",
         {{"mode", "preset"}, {"core_name", "harness_bug_triage"},
          {"description", "Run bounded workers for reproducible bug triage."}}},
        {"research_synthesis",
         {{"mode", "preset"}, {"core_name", "harness_research_synthesis"},
          {"description", "Run bounded research workers and synthesize evidence."}}},
    };
}

json harness_preset_names() {
    return json::array(
        {"fanout_judge", "pr_review_panel", "bug_triage", "research_synthesis"});
}

json harness_admission_schema(const HarnessProgramSnapshots& snapshots) {
    return snapshots.admission_profile.manifest();
}

json compile_schema() {
    return json::parse(
        R"JSON({"type":"object","required":["ok","diagnostics","artifacts"],"properties":{"ok":{"type":"boolean"},"artifact_id":{"type":"string"},"diagnostics":{"type":"array"},"artifacts":{"type":"object"}},"additionalProperties":true})JSON");
}
json run_schema() {
    return json::parse(
        R"JSON({"type":"object","required":["run_id","status"],"properties":{"run_id":{"type":"string"},"artifact_id":{"type":"string"},"status":{"type":"string"},"result":{"type":"object"},"pending":{"type":"object"}},"additionalProperties":true})JSON");
}
}  // namespace

HarnessWorkerResponse HarnessWorkerResponse::success(json v) {
    return {HarnessWorkerResponseKind::VALUE, std::move(v), {}};
}
HarnessWorkerResponse HarnessWorkerResponse::empty(std::string m) {
    return {HarnessWorkerResponseKind::EMPTY, nullptr, std::move(m)};
}
HarnessWorkerResponse HarnessWorkerResponse::parse_error(std::string m) {
    return {HarnessWorkerResponseKind::PARSE_ERROR, nullptr, std::move(m)};
}
HarnessWorkerResponse HarnessWorkerResponse::tool_error(std::string m) {
    return {HarnessWorkerResponseKind::TOOL_ERROR, nullptr, std::move(m)};
}
HarnessWorkerResponse HarnessWorkerResponse::timeout(std::string m) {
    return {HarnessWorkerResponseKind::TIMEOUT, nullptr, std::move(m)};
}
HarnessWorkerResponse HarnessWorkerResponse::cancelled(std::string m) {
    return {HarnessWorkerResponseKind::CANCELLED, nullptr, std::move(m)};
}
HarnessWorkerResponse HarnessWorkerResponse::awaiting_tool_results(json v) {
    return {HarnessWorkerResponseKind::AWAITING_TOOL_RESULTS, std::move(v), {}};
}
HarnessWorkerResponse HarnessWorkerResponse::input_required(json v) {
    return {HarnessWorkerResponseKind::INPUT_REQUIRED, std::move(v), {}};
}
struct HarnessService::Impl : std::enable_shared_from_this<HarnessService::Impl> {
    struct Artifact {
        std::string                              id;
        HarnessProgramArtifactRecord             record;
        HarnessCapabilityBindingRequest          bindings;
        std::optional<program::ContractManifest> contract;
        std::shared_ptr<program::ProgramStore>   store;
        std::shared_ptr<program::ProgramCatalog> catalog;
        std::shared_ptr<program::ProgramRuntime> runtime;
    };
    struct Run {
        std::string                              artifact_id;
        std::string                              mode;
        program::ProgramHandle                   handle;
        std::shared_ptr<program::ProgramRuntime> runtime;
        std::optional<program::ContractRun>       contract;
        std::string                              workspace_revision;
        bool                                     contract_finalized = false;
        std::int64_t                             created;
        std::shared_ptr<std::mutex>              guard;
    };
    Impl(HarnessServiceConfig c, std::shared_ptr<HarnessJournal> j, HarnessServiceResources r)
        : config(std::move(c)), journal(std::move(j)), resources(std::move(r)) {
        if (!resources.compiler || !resources.make_program_catalog ||
            !resources.make_program_runtime || !resources.make_recorded_binding ||
            resources.owner_scope.empty())
            throw std::invalid_argument(
                "HarnessService requires owned Program resources and scope");
        if (!config.record_store) config.record_store = std::make_shared<MemoryRecords>();
        events = std::make_shared<EventProjection>(journal);
    }
    std::shared_ptr<program::ProgramTransitionStore> transitions(
        const HarnessProgramArtifactRecord& record) {
        if (auto adapter =
                std::dynamic_pointer_cast<HarnessProgramAdapterStore>(config.record_store))
            return adapter->bind_program_transitions(record);
        return std::make_shared<program::InMemoryProgramTransitionStore>();
    }
    std::shared_ptr<Artifact> artifact(const std::string& id) {
        {
            std::lock_guard l(mutex);
            auto            x = artifacts.find(id);
            if (x != artifacts.end()) return x->second;
        }
        auto store = std::make_shared<HarnessBoundedProgramStore>(
            config.record_store, id, resources.owner_scope, json::object(), json::object());
        auto rec = store->load_artifact();
        if (!rec) return {};
        auto p = rec->legacy_projection();
        if (!p.contains("program_bindings"))
            throw std::invalid_argument("legacy pre-Program Harness artifact is blocked");
        std::optional<program::ContractManifest> contract;
        if (p.contains("contract_manifest")) {
            if (!p.at("contract_manifest").is_object())
                throw std::invalid_argument("Harness contract manifest must be an object");
            contract = program::ContractManifest::parse(
                program::detail::canonical_json_bytes(p.at("contract_manifest")));
            if (contract->lifecycle() != program::ContractManifestLifecycle::Frozen)
                throw std::invalid_argument("Harness artifact contract is not frozen");
            if (!p.contains("contract_manifest_hash") ||
                p.at("contract_manifest_hash") != contract->content_hash())
                throw std::invalid_argument("Harness artifact contract hash mismatch");
            if (contract->spec().owner_scope != resources.owner_scope)
                throw std::invalid_argument("Harness artifact contract owner scope mismatch");
        }
        auto b       = parse_bindings(p.at("program_bindings"));
        auto catalog = resources.make_program_catalog(store, b);
        if (!catalog) throw std::runtime_error("incomplete Program catalog");
        auto v = catalog->resolve_version(resources.owner_scope, rec->version().id());
        if (!v || v->id() != rec->version().id() || v->bundle_id() != rec->bundle().id())
            throw std::invalid_argument("stored Program tuple failed exact resolution");
        auto runtime = resources.make_program_runtime(catalog, transitions(*rec));
        if (!runtime) throw std::runtime_error("incomplete Program runtime");
        auto a = std::make_shared<Artifact>(
            Artifact{id, *rec, std::move(b), std::move(contract), store, std::move(catalog),
                     std::move(runtime)});
        std::lock_guard l(mutex);
        return artifacts.emplace(id, a).first->second;
    }
    json compile(const json& request) {
        try {
            auto t  = HarnessRequestTranslator::translate(request, resources.snapshots.registry,
                                                          config.translation_defaults);
            if (t.contract && t.contract->spec().owner_scope != resources.owner_scope)
                throw HarnessTranslationError(
                    "H_CONTRACT_SCOPE", "/contract",
                    "Harness contract owner_scope must match the service owner scope");
            auto b  = resources.compiler->compile(t.source);
            auto id =
                alias(b, resources.snapshots.policy.fingerprint(), t.bindings,
                      resources.artifact_binding_identity,
                      t.contract ? t.contract->content_hash() : std::string_view{});
            auto p  = t.wire.legacy_projection;
            auto source_map        = source_map_json(t.source.source_map());
            p["program_bindings"]  = bindings_json(t.bindings);
            p["source_id"]         = t.wire.source_id;
            p["core"]              = b.serialize_canonical();
            p["sourcemap"]         = source_map;
            p["diagnostics"]       = json::array();
            p["admission-profile"] = resources.snapshots.admission_profile.manifest();
            if (t.contract) {
                p["contract_manifest"] =
                    json::parse(t.contract->serialize_canonical());
                p["contract_manifest_hash"] = t.contract->content_hash();
            }
            auto store             = std::make_shared<HarnessBoundedProgramStore>(
                config.record_store, id, resources.owner_scope, invocation_json(t.invocation), p);
            auto catalog = resources.make_program_catalog(store, t.bindings);
            if (!catalog) throw std::runtime_error("incomplete Program catalog");
            auto v   = catalog->admit(b, {resources.owner_scope,
                                          resources.snapshots.admission_profile,
                                          resources.snapshots.policy,
                                          {}});
            auto rec = store->load_artifact();
            if (!rec) throw std::runtime_error("Catalog did not publish Program tuple");
            auto runtime = resources.make_program_runtime(catalog, transitions(*rec));
            if (!runtime) throw std::runtime_error("incomplete Program runtime");
            const bool        has_contract  = t.contract.has_value();
            const std::string contract_hash =
                t.contract ? t.contract->content_hash() : std::string{};
            const json contract_manifest =
                t.contract ? json::parse(t.contract->serialize_canonical()) : json(nullptr);
            {
                std::lock_guard l(mutex);
                artifacts[id] =
                    std::make_shared<Artifact>(Artifact{id, *rec, std::move(t.bindings),
                                                        std::move(t.contract), store,
                                                        std::move(catalog), std::move(runtime)});
            }
            auto base = "neograph://artifacts/" + id;
            json artifacts = {
                {"core_lockfile",
                 {{"uri", base + "/core"}, {"content", b.serialize_canonical()}}},
                {"source_map",
                 {{"uri", base + "/sourcemap"}, {"content", source_map}}},
                {"diagnostics",
                 {{"uri", base + "/diagnostics"}, {"content", json::array()}}},
                {"admission_profile",
                 {{"uri", base + "/admission-profile"},
                  {"content", resources.snapshots.admission_profile.manifest()}}},
            };
            if (has_contract)
                artifacts["contract"] = {
                    {"uri", base + "/contract"},
                    {"content", contract_manifest},
                    {"manifest_hash", contract_hash}};
            return {{"ok", true},
                    {"artifact_id", id},
                    {"diagnostics", json::array()},
                    {"bundle_id", b.id()},
                    {"program_version_id", v.id()},
                    {"revision_digest", v.id()},
                    {"artifacts", std::move(artifacts)}};
        } catch (const HarnessTranslationError& e) {
            return {{"ok", false},
                    {"diagnostics", json::array({{{"phase", "translation"},
                                                  {"code", e.code()},
                                                  {"severity", "error"},
                                                  {"path", e.pointer()},
                                                  {"message", e.what()}}})},
                    {"artifacts", json::object()}};
        } catch (const program::ProgramCompileError& e) {
            return {{"ok", false},
                    {"diagnostics", diagnostics(e.diagnostics())},
                    {"artifacts", json::object()}};
        } catch (const program::ProgramAdmissionError& e) {
            return {{"ok", false},
                    {"diagnostics", diagnostics(e.diagnostics())},
                    {"artifacts", json::object()}};
        }
    }
    json read_artifact(const std::string& id, const std::string& view) {
        auto x = artifact(id);
        if (!x) throw std::invalid_argument("unknown Harness artifact: " + id);
        const auto& p = x->record.legacy_projection();
        if (view != "core" && view != "sourcemap" && view != "diagnostics" &&
            view != "admission-profile" && view != "contract")
            throw std::invalid_argument("unsupported Harness artifact view: " + view);
        if (view == "contract") {
            if (!p.contains("contract_manifest"))
                throw std::invalid_argument("Harness artifact has no frozen contract");
            return {{"manifest", p.at("contract_manifest")},
                    {"manifest_hash", p.at("contract_manifest_hash")}};
        }
        return p.at(view);
    }
    static json uris(const std::string& id) {
        auto b = "neograph://runs/" + id + "/";
        return {{"status", b + "status"},
                {"details", b + "details"},
                {"trace", b + "trace"},
                {"attempts", b + "attempts"},
                {"checkpoints", b + "checkpoints"},
                {"diff", b + "diff"},
                {"artifacts", b + "artifacts"}};
    }
    json start(const json& a) {
        if (!a.is_object()) throw std::invalid_argument("start arguments must be object");
        if (std::dynamic_pointer_cast<FileHarnessRecordStore>(config.record_store))
            throw std::invalid_argument(
                "FileHarnessRecordStore cannot start Program runs; use SQLite");
        std::string                                             artifact_id;
        std::string                                             mode = "live";
        std::optional<program::ExactProgramCheckpointReference> fork_source;
        std::shared_ptr<Artifact>                               fork_artifact;
        std::shared_ptr<Run>                                    replay_source;
        program::ProgramResume                                  fork_resume;
        std::shared_ptr<program::ProgramTransitionStore>         fork_source_transitions;
        if (a.contains("request")) {
            if (a.size() != 1) throw std::invalid_argument("inline start accepts only request");
            auto c = compile(a.at("request"));
            if (!c.value("ok", false))
                return {{"started", false},
                        {"status", "compile_failed"},
                        {"diagnostics", c.at("diagnostics")},
                        {"artifacts", c.at("artifacts")}};
            artifact_id = c.at("artifact_id").get<std::string>();
        } else if (a.contains("artifact_id") && a.size() == 1 && a.at("artifact_id").is_string()) {
            artifact_id = a.at("artifact_id").get<std::string>();
        } else if (a.contains("replay") && a.size() == 1 && a.at("replay").is_object()) {
            const auto& replay = a.at("replay");
            if (!replay.contains("source_run_id") || !replay.at("source_run_id").is_string() ||
                !replay.contains("mode") || !replay.at("mode").is_string())
                throw std::invalid_argument("replay requires source_run_id and mode");
            const auto requested_mode = replay.at("mode").get<std::string>();
            if (requested_mode != "live" && requested_mode != "recorded")
                throw std::invalid_argument("replay mode must be live or recorded");
            replay_source = run(replay.at("source_run_id").get<std::string>());
            if (!replay_source) return {{"started", false}, {"status", "not_found"}};
            mode        = requested_mode == "live" ? "live_replay" : "recorded_replay";
            artifact_id = replay_source->artifact_id;
        } else if (a.contains("fork") && a.size() == 1 && a.at("fork").is_object()) {
            const auto& fork = a.at("fork");
            if (!fork.contains("source_run_id") || !fork.at("source_run_id").is_string() ||
                !fork.contains("checkpoint_id") || !fork.at("checkpoint_id").is_string() ||
                !fork.contains("artifact_id") || !fork.at("artifact_id").is_string()) {
                throw std::invalid_argument(
                    "fork requires source_run_id, checkpoint_id, and artifact_id");
            }
            const bool has_call_id = fork.contains("call_id");
            const bool has_result  = fork.contains("result");
            if (has_call_id != has_result ||
                (has_call_id && !fork.at("call_id").is_string())) {
                throw std::invalid_argument(
                    "fork call_id and result must be supplied together");
            }
            if (has_call_id) {
                fork_resume.pending_id = fork.at("call_id").get<std::string>();
                fork_resume.value      = fork.at("result");
            }
            auto source = run(fork.at("source_run_id").get<std::string>());
            if (!source) return {{"started", false}, {"status", "not_found"}};
            fork_artifact = artifact(source->artifact_id);
            if (!fork_artifact) return {{"started", false}, {"status", "not_found"}};
            fork_source_transitions = transitions(fork_artifact->record);
            artifact_id = fork.at("artifact_id").get<std::string>();
            fork_source = program::ExactProgramCheckpointReference{
                fork.at("source_run_id").get<std::string>(),
                fork.at("checkpoint_id").get<std::string>()};
            mode = "compatible_fork";
        } else
            throw std::invalid_argument(
                "start requires exactly request, artifact_id, replay, or fork");
        auto x = artifact(artifact_id);
        if (!x) return {{"started", false}, {"status", "not_found"}};
        auto runtime = x->runtime;
        if (fork_artifact && fork_artifact->id != x->id) {
            auto store = std::make_shared<ForkProgramStore>(
                x->store, fork_artifact->record.bundle(), fork_artifact->record.version());
            auto catalog = resources.make_program_catalog(std::move(store), x->bindings);
            if (!catalog) throw std::runtime_error("incomplete Program fork catalog");
            auto source_transitions = fork_source_transitions;
            auto target_transitions = transitions(x->record);
            auto fork_transitions   = std::make_shared<ForkProgramTransitionStore>(
                fork_source->source_run_id, std::move(source_transitions),
                std::move(target_transitions));
            runtime = resources.make_program_runtime(std::move(catalog),
                                                     std::move(fork_transitions));
            if (!runtime) throw std::runtime_error("incomplete Program fork runtime");
        }
        auto invocation             = parse_invocation(x->record.legacy_invocation());
        if (fork_source) {
            const auto source_record =
                fork_source_transitions->load(resources.owner_scope,
                                              fork_source->source_run_id);
            if (!source_record) return {{"started", false}, {"status", "not_found"}};
            invocation.budget =
                bounded_budget(invocation.budget, source_record->remaining_budget());
        }
        auto id                     = run_id(resources.snapshots.policy.fingerprint());
        invocation.requested_run_id = id;
        invocation.events           = events;
        std::optional<program::ContractRun> contract_run;
        std::string workspace_revision = x->record.version().id();
        const auto projection = x->record.legacy_projection();
        if (projection.contains("workspace_revision")) {
            if (!projection.at("workspace_revision").is_string() ||
                projection.at("workspace_revision").get<std::string>().empty())
                throw std::invalid_argument("Harness workspace_revision must be a non-empty string");
            workspace_revision = projection.at("workspace_revision").get<std::string>();
        }
        if (x->contract) {
            contract_run.emplace(*x->contract);
            contract_run->begin_attempt();
        }
        std::optional<program::ProgramHandle> handle;
        try {
            if (fork_source) {
                auto resume     = std::move(fork_resume);
                resume.trace_id = "harness-fork:" + id;
                resume.events   = events;
                handle.emplace(runtime->fork(resources.owner_scope, *fork_source,
                                             x->record.version(), std::move(invocation),
                                             std::move(resume)));
            } else if (mode == "recorded_replay") {
                auto source_handle = retained_handle(replay_source);
                if (!source_handle.try_result())
                    return {{"started", false}, {"status", "source_not_terminal"}};
                auto recorded = resources.make_recorded_binding(
                    x->record.version(), x->bindings, source_handle.events_after(0));
                handle.emplace(runtime->start_recorded(
                    resources.owner_scope, x->record.version(), std::move(invocation),
                    std::move(recorded)));
            } else {
                handle.emplace(runtime->start(resources.owner_scope, x->record.version(),
                                              std::move(invocation)));
            }
        } catch (const program::ProgramForkCompatibilityError& e) {
            return {{"started", false},
                    {"status", "incompatible_fork"},
                    {"receipt", json::parse(e.receipt().serialize_canonical())}};
        }
        if (handle->run_id() != id)
            throw std::runtime_error("ProgramRuntime changed requested run_id");
        {
            std::lock_guard l(mutex);
            runs[id] = std::make_shared<Run>(
                Run{artifact_id, mode, *handle, runtime, std::move(contract_run),
                    std::move(workspace_revision), false, now_ms(), std::make_shared<std::mutex>()});
        }
        json response = {{"started", true},
                         {"run_id", id},
                         {"artifact_id", artifact_id},
                         {"execution_mode", mode},
                         {"status", "queued"},
                         {"program_version_id", handle->program_version_id()},
                         {"bundle_id", x->record.bundle().id()},
                         {"revision_digest", handle->program_version_id()},
                         {"artifacts", uris(id)}};
        if (x->contract) {
            response["contract"] = {
                {"manifest_hash", x->contract->content_hash()},
                {"status", "frozen"},
                {"attempt", 1},
            };
        }
        if (fork_source) {
            response["source_run_id"]        = fork_source->source_run_id;
            response["source_checkpoint_id"] = fork_source->source_checkpoint_id;
        }
        return response;
    }
    std::shared_ptr<Run> run(const std::string& id) {
        {
            std::lock_guard l(mutex);
            auto            x = runs.find(id);
            if (x != runs.end()) return x->second;
        }
        auto adapter = std::dynamic_pointer_cast<HarnessProgramAdapterStore>(config.record_store);
        if (!adapter) {
            if (config.record_store->load_run(id))
                throw std::invalid_argument(
                    "legacy pre-Program Harness run is blocked after reconnect");
            return {};
        }
        auto retained = adapter->resolve_program_run(resources.owner_scope, id);
        if (!retained) return {};
        auto a = artifact(retained->artifact_id());
        if (!a) return {};
        retained->validate_artifact(a->record);
        auto h     = a->runtime->reconnect(resources.owner_scope, id);
        auto mode  = retained->run_record().fork_receipt() ? std::string("compatible_fork")
                                                           : std::string("live");
        std::optional<program::ContractRun> contract_run;
        std::string workspace_revision = a->record.version().id();
        const auto projection = a->record.legacy_projection();
        if (projection.contains("workspace_revision") &&
            projection.at("workspace_revision").is_string() &&
            !projection.at("workspace_revision").get<std::string>().empty())
            workspace_revision = projection.at("workspace_revision").get<std::string>();
        if (a->contract) {
            contract_run.emplace(*a->contract);
            contract_run->begin_attempt();
        }
        auto value = std::make_shared<Run>(Run{a->id, std::move(mode), h, a->runtime,
                                               std::move(contract_run),
                                               std::move(workspace_revision), false,
                                               retained->run_record().created_at_ms(),
                                               std::make_shared<std::mutex>()});
        std::lock_guard l(mutex);
        return runs.emplace(id, value).first->second;
    }
    program::ProgramHandle retained_handle(const std::shared_ptr<Run>& x) const {
        std::lock_guard lock(*x->guard);
        return x->handle;
    }
    void finalize_contract(Run& x, const program::ProgramResult& terminal) {
        if (!x.contract || x.contract_finalized) return;
        auto& contract = *x.contract;
        const auto program_version = terminal.program_version_id();
        const auto artifact_hash   = terminal.bundle_id();
        const auto actual           = runtime_final_value(terminal);
        const bool completed = terminal.status() == program::ProgramTerminalStatus::Completed;
        try {
            // Worker output is retained only as a self-report. It is never
            // used as the acceptance decision below.
            const auto output = terminal.output();
            const auto channels = output.is_object() && output.contains("channels")
                                      ? output.at("channels")
                                      : json::object();
            const auto worker_results = channels.is_object()
                                            ? channels.value("worker_results", json::array())
                                            : json::array();
            if (worker_results.is_array() && !worker_results.empty()) {
                for (const auto& worker : worker_results)
                    contract.record_worker_report(worker.dump(),
                                                  worker.value("status", "failed") == "completed");
            } else {
                contract.record_worker_report(actual.dump(), completed);
            }

            for (const auto& acceptance : contract.manifest().spec().acceptance) {
                if (!acceptance.required) continue;
                program::ContractEvidence evidence;
                evidence.evidence_id = "runtime-" + acceptance.id + "-" +
                                       std::to_string(contract.attempt());
                evidence.acceptance_id      = acceptance.id;
                evidence.kind               = program::ContractEvidenceKind::DeterministicRun;
                evidence.manifest_hash     = contract.manifest().content_hash();
                evidence.program_version_id = program_version;
                evidence.workspace_revision = x.workspace_revision;
                evidence.command            = "ProgramRuntime";
                evidence.toolchain          = "neograph-program-runtime";
                evidence.artifact_hash      = artifact_hash;
                evidence.executed           = true;
                evidence.passed = completed && runtime_matches_expected(actual, acceptance.expected);
                evidence.details = {{"actual", actual},
                                    {"expected", acceptance.expected},
                                    {"program_status", std::string(program::to_string(
                                         terminal.status()))}};
                if (!completed && terminal.failure())
                    evidence.details["failure"] = terminal.failure()->message;
                contract.record_evidence(std::move(evidence));
            }
            // The runtime observation is the independent oracle boundary for
            // Harness. Worker-provided fields are deliberately not consulted.
            for (const auto& oracle : contract.manifest().spec().independent_oracles) {
                program::ContractEvidence evidence;
                evidence.evidence_id = "oracle-" + oracle + "-" +
                                       std::to_string(contract.attempt());
                evidence.kind               = program::ContractEvidenceKind::IndependentOracle;
                evidence.manifest_hash     = contract.manifest().content_hash();
                evidence.program_version_id = program_version;
                evidence.workspace_revision = x.workspace_revision;
                evidence.command            = "ProgramRuntime outcome oracle";
                evidence.toolchain          = "neograph-program-runtime";
                evidence.artifact_hash      = artifact_hash;
                evidence.executed           = true;
                evidence.passed             = completed && !actual.is_null();
                evidence.details            = {{"oracle_id", oracle},
                                    {"observed", actual},
                                    {"program_status", std::string(program::to_string(
                                         terminal.status()))}};
                contract.record_evidence(std::move(evidence));
            }
        } catch (const std::exception& error) {
            contract.record_diagnostic(
                {"harness-contract-finalization", error.what(), true});
        }
        const auto verification = contract.verify(program_version, x.workspace_revision);
        if (verification.publishable) contract.publish();
        x.contract_finalized = true;
    }
    json snapshot(const std::shared_ptr<Run>& x) {
        auto handle           = retained_handle(x);
        auto record           = handle.snapshot();
        auto terminal         = handle.try_result();
        if (terminal && x->contract) {
            std::lock_guard lock(*x->guard);
            finalize_contract(*x, *terminal);
        }
        json r                = terminal ? project(*terminal)
                                         : json{{"run_id", record.run_id()},
                                                {"status", "running"},
                                                {"program_version_id", record.program_version_id()},
                                                {"bundle_id", record.bundle_id()},
                                                {"operation_id", record.continuation().operation_id},
                                                {"attempt", record.continuation().attempt},
                                                {"remaining_budget", budget_json(record.remaining_budget())}};
        r["artifact_id"]      = x->artifact_id;
        r["revision_digest"]  = record.program_version_id();
        r["protocol_version"] = MCP_PROTOCOL_VERSION;
        r["profile"]          = "harness-m4";
        r["execution_mode"]   = x->mode;
        r["created_at"]       = record.created_at_ms();
        r["updated_at"]       = record.updated_at_ms();
        r["expires_at"]       = record.created_at_ms() + config.run_ttl.count();
        r["poll_after_ms"]    = config.poll_interval.count();
        r["artifacts"]        = uris(record.run_id());
        if (x->contract) {
            const auto contract = contract_projection(*x->contract);
            r["contract"] = contract;
            if (terminal && x->contract->status() != program::ContractRunStatus::Published) {
                r["program_status"] = r.at("status");
                r["status"] = x->contract->status() == program::ContractRunStatus::Failed
                                   ? "failed"
                                   : "blocked";
                if (r.contains("result")) {
                    const auto candidate = r.at("result");
                    json       projected  = json::object();
                    for (const auto& [key, value] : r.items()) {
                        if (key != "result") projected[key] = value;
                    }
                    projected["candidate_result"] = candidate;
                    r = std::move(projected);
                }
            }
        }
        return r;
    }
    json get(const std::string& id, const std::string& view, std::size_t after, std::size_t limit) {
        auto x = run(id);
        if (!x) throw std::invalid_argument("unknown Harness run: " + id);
        auto s = snapshot(x);
        if (view == "status" || view == "details") return s;
        if (view == "artifacts")
            return {{"run_id", id}, {"status", s.at("status")}, {"artifacts", s.at("artifacts")}};
        auto handle = retained_handle(x);
        if (view == "trace" || view == "attempts") {
            json e = json::array();
            for (const auto& v : handle.events_after(after)) {
                if (e.size() >= limit) break;
                e.push_back(project(v));
            }
            return {{"run_id", id},
                    {"status", s.at("status")},
                    {"after_sequence", after},
                    {"events", std::move(e)}};
        }
        if (view == "checkpoints") {
            json c = json::array();
            if (auto v = handle.latest_checkpoint()) c.push_back(checkpoint(*v));
            return {{"run_id", id}, {"status", s.at("status")}, {"checkpoints", std::move(c)}};
        }
        if (view == "diff")
            return {{"run_id", id},
                    {"status", s.at("status")},
                    {"available", false},
                    {"reason", "Program checkpoint diff unavailable"}};
        throw std::invalid_argument("unsupported Harness view: " + view);
    }
    json resume(const json& a) {
        if (!a.is_object() || !a.contains("run_id") || !a.contains("call_id") ||
            !a.at("run_id").is_string() || !a.at("call_id").is_string())
            throw std::invalid_argument("resume requires run_id and call_id");
        auto id   = a.at("run_id").get<std::string>();
        auto call = a.at("call_id").get<std::string>();
        auto x    = run(id);
        if (!x) throw std::invalid_argument("unknown Harness run: " + id);
        std::unique_lock run_lock(*x->guard);
        auto            handle = x->handle;
        const auto      before = handle.snapshot();
        bool       duplicate = false;
        const auto now = static_cast<std::uint64_t>(now_ms());
        if (a.contains("resolution")) {
            if (const auto pending = before.pending_effect()) {
                const auto resolution =
                    parse_reconciliation(a.at("resolution").get<std::string>());
                const auto update = [&] {
                    if (before.continuation().state == program::ContinuationState::Interrupted &&
                        resolution == program::ProgramEffectReconciliation::Unknown &&
                        !a.contains("result")) {
                        if (pending->call_id() != call)
                            return pending->reconcile(call, pending->effect_id(), resolution,
                                                       std::nullopt, now);
                        return pending->mark_outcome_unknown(now);
                    }
                    return pending->reconcile(
                        call, pending->effect_id(), resolution,
                        a.contains("result") ? std::optional<json>{a.at("result")} : std::nullopt,
                        now);
                }();
                duplicate = update.disposition == program::ProgramPendingDisposition::Duplicate;
            }
        } else if (a.contains("result")) {
            if (const auto pending = before.pending_input()) {
                duplicate = pending->submit(call, a.at("result"), now).disposition ==
                            program::ProgramPendingDisposition::Duplicate;
            } else if (const auto pending = before.pending_effect()) {
                duplicate = pending->submit(call, pending->effect_id(), a.at("result"), now)
                                .disposition == program::ProgramPendingDisposition::Duplicate;
            }
        }
        if (duplicate) {
            run_lock.unlock();
            return {{"accepted", true},
                    {"duplicate", true},
                    {"run_id", id},
                    {"call_id", call},
                    {"status", snapshot(x).at("status")}};
        }
        const auto before_journal = before.journal_head();
        const auto before_events  = before.event_sequence();
        const auto before_effects = before.effect_sequence();

        auto next = [&]() -> program::ProgramHandle {
            if (a.contains("resolution")) {
                program::ProgramEffectResolution value;
                value.pending_id = call;
                value.resolution = parse_reconciliation(a.at("resolution").get<std::string>());
                if (a.contains("result")) value.result = a.at("result");
                value.trace_id = "harness-reconcile:" + call;
                value.events   = events;
                return handle.reconcile(*x->runtime, std::move(value));
            }
            program::ProgramResume value;
            value.value      = a.at("result");
            value.trace_id   = "harness-resume:" + call;
            value.events     = events;
            value.pending_id = call;
            return handle.resume(*x->runtime, std::move(value));
        }();
        const auto after = next.snapshot();
        duplicate = duplicate || (before_journal == after.journal_head() &&
                                  before_events == after.event_sequence() &&
                                  before_effects == after.effect_sequence());
        if (!duplicate) x->handle = std::move(next);
        run_lock.unlock();
        return {{"accepted", true},
                {"duplicate", duplicate},
                {"run_id", id},
                {"call_id", call},
                {"status", snapshot(x).at("status")}};
    }
    bool cancel(const std::string& id) {
        auto x = run(id);
        return x && retained_handle(x).cancel();
    }
    HarnessServiceConfig                                       config;
    std::shared_ptr<HarnessJournal>                            journal;
    HarnessServiceResources                                    resources;
    std::shared_ptr<EventProjection>                           events;
    mutable std::mutex                                         mutex;
    std::unordered_map<std::string, std::shared_ptr<Artifact>> artifacts;
    std::unordered_map<std::string, std::shared_ptr<Run>>      runs;
};

HarnessService::HarnessService(HarnessServiceConfig            c,
                               std::shared_ptr<HarnessJournal> j,
                               HarnessServiceResources         r)
    : impl_(std::make_shared<Impl>(std::move(c), std::move(j), std::move(r))) {}
HarnessService::~HarnessService() = default;
json HarnessService::schema() const {
    return {{"service", "neograph-harness-m4"},
            {"protocol_version", MCP_PROTOCOL_VERSION},
            {"profile", "harness-m4"},
            {"request_schema", harness_program_request_schema()},
            {"output_schema", harness_program_output_schema()},
            {"node_palette", impl_->resources.snapshots.registry.manifest()},
            {"admission_profile", harness_admission_schema(impl_->resources.snapshots)},
            {"presets", harness_preset_names()},
            {"preset_contracts", harness_preset_contracts()},
            {"capabilities",
             {{"compile", true},
              {"run", true},
              {"resume", true},
              {"cancel", true},
              {"program_api", true}}}};
}
json HarnessService::compile(const json& v) {
    return impl_->compile(v);
}
json HarnessService::start(const json& v) {
    return impl_->start(v);
}
json HarnessService::resume(const json& v) {
    return impl_->resume(v);
}
json HarnessService::get(const std::string& i, const std::string& v) const {
    return impl_->get(i, v, 0, 100);
}
json HarnessService::get(const std::string& i,
                         const std::string& v,
                         std::size_t        a,
                         std::size_t        l) const {
    if (!l || l > 1000) throw std::invalid_argument("limit must be in [1,1000]");
    return impl_->get(i, v, a, l);
}
bool HarnessService::cancel(const std::string& i) {
    return impl_->cancel(i);
}
json HarnessService::read(const std::string& u) const {
    constexpr std::string_view runs = "neograph://runs/", artifacts = "neograph://artifacts/";
    if (u.rfind(runs.data(), 0) == 0) {
        auto       path  = u.substr(runs.size());
        const auto slash = path.find('/');
        if (slash == std::string::npos) throw std::invalid_argument("Harness URI requires view");
        const auto  query = path.find('?', slash + 1);
        auto        view  = path.substr(slash + 1,
                                query == std::string::npos ? std::string::npos : query - slash - 1);
        std::size_t after = 0, limit = 100;
        if (query != std::string::npos) {
            auto        parameters = path.substr(query + 1);
            std::size_t position   = 0;
            while (position <= parameters.size()) {
                const auto end  = parameters.find('&', position);
                const auto pair = parameters.substr(
                    position, end == std::string::npos ? std::string::npos : end - position);
                const auto equal = pair.find('=');
                if (equal == std::string::npos)
                    throw std::invalid_argument("Harness URI query requires key=value");
                const auto key   = pair.substr(0, equal);
                const auto value = pair.substr(equal + 1);
                if (key == "after_sequence")
                    after = std::stoull(value);
                else if (key == "limit")
                    limit = std::stoull(value);
                else
                    throw std::invalid_argument("unsupported Harness URI query parameter");
                if (end == std::string::npos) break;
                position = end + 1;
            }
        }
        return get(path.substr(0, slash), view, after, limit);
    }
    if (u.rfind(artifacts.data(), 0) == 0) {
        auto       path  = u.substr(artifacts.size());
        const auto slash = path.find('/');
        if (slash == std::string::npos)
            throw std::invalid_argument("Harness artifact URI requires view");
        return impl_->read_artifact(path.substr(0, slash), path.substr(slash + 1));
    }
    throw std::invalid_argument("unsupported Harness URI");
}

void HarnessService::register_tools(MCPServer& server) {
    auto a = impl_;
    server.register_tool(
        tool_definition(
            "neograph_schema", "NeoGraph Harness schema",
            "Return Harness schemas and Program registry palette.",
            json::parse(
                R"JSON({"type":"object","properties":{},"additionalProperties":false})JSON"),
            json::parse(
                R"JSON({"type":"object","required":["service","request_schema","node_palette"],"properties":{"service":{"type":"string"},"request_schema":{"type":"object"},"node_palette":{"type":"object"}},"additionalProperties":true})JSON"),
            true),
        [a](const json&, const auto&) {
            return mcp_result({{"service", "neograph-harness-m4"},
                               {"request_schema", harness_program_request_schema()},
                               {"output_schema", harness_program_output_schema()},
                               {"node_palette", a->resources.snapshots.registry.manifest()},
                               {"admission_profile",
                                harness_admission_schema(a->resources.snapshots)},
                               {"presets", harness_preset_names()},
                               {"preset_contracts", harness_preset_contracts()}},
                              "NeoGraph Harness Program schema");
        });
    server.register_tool(tool_definition("neograph_compile", "Compile Harness",
                                         "Compile through the public Program API.",
                                         harness_program_request_schema(), compile_schema()),
                         [a](const json& v, const auto&) {
                             return mcp_result(a->compile(v),
                                               "Harness Program compilation completed");
                         });
    auto sd = tool_definition(
        "neograph_start", "Start Harness", "Start an exact retained Program artifact.",
        json::parse(
            R"JSON({"type":"object","properties":{"artifact_id":{"type":"string"},"request":{"type":"object"},"replay":{"type":"object","required":["source_run_id","mode"],"properties":{"source_run_id":{"type":"string"},"mode":{"enum":["live","recorded"]}},"additionalProperties":false},"fork":{"type":"object","required":["source_run_id","checkpoint_id","artifact_id"],"properties":{"source_run_id":{"type":"string"},"checkpoint_id":{"type":"string"},"artifact_id":{"type":"string"},"call_id":{"type":"string"},"result":{}},"additionalProperties":false}},"additionalProperties":false})JSON"),
        json::parse(
            R"JSON({"type":"object","required":["started","status"],"properties":{"started":{"type":"boolean"},"run_id":{"type":"string"},"artifact_id":{"type":"string"},"execution_mode":{"type":"string"},"status":{"type":"string"},"diagnostics":{"type":"array"},"artifacts":{"type":"object"}},"additionalProperties":true})JSON"));
    server.register_tool(std::move(sd), [a](const json& v, const auto&) {
        return mcp_result(a->start(v), "Harness Program start completed");
    });
    server.register_tool(
        tool_definition(
            "neograph_get", "Get Harness run", "Read Program-backed run state.",
            json::parse(
                R"JSON({"type":"object","required":["run_id"],"properties":{"run_id":{"type":"string"},"view":{"enum":["status","details","trace","attempts","checkpoints","diff","artifacts"]},"uri":{"type":"string"},"after_sequence":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":1,"maximum":1000}},"additionalProperties":false})JSON"),
            run_schema(), true),
        [a](const json& v, const auto&) {
            auto id = v.at("run_id").get<std::string>();
            return mcp_result(
                a->get(id, v.value("view", "status"), v.value("after_sequence", std::uint64_t{0}),
                       v.value("limit", std::uint64_t{100})),
                "Harness Program run status");
        });
    server.register_tool(
        tool_definition(
            "neograph_resume", "Resume Harness run", "Submit exact pending Program input.",
            json::parse(
                R"JSON({"type":"object","required":["run_id","call_id"],"properties":{"run_id":{"type":"string"},"call_id":{"type":"string"},"result":{},"resolution":{"enum":["completed","failed","unknown"]}},"anyOf":[{"required":["result"]},{"required":["resolution"]}],"additionalProperties":false})JSON"),
            json::parse(
                R"JSON({"type":"object","required":["accepted","duplicate","run_id","call_id","status"],"properties":{"accepted":{"type":"boolean"},"duplicate":{"type":"boolean"},"run_id":{"type":"string"},"call_id":{"type":"string"},"status":{"type":"string"}},"additionalProperties":false})JSON")),
        [a](const json& v, const auto&) {
            return mcp_result(a->resume(v), "Harness Program resume processed");
        });
    server.register_tool(
        tool_definition(
            "neograph_cancel", "Cancel Harness run", "Cancel a Program run.",
            json::parse(
                R"JSON({"type":"object","required":["run_id"],"properties":{"run_id":{"type":"string"}},"additionalProperties":false})JSON"),
            json::parse(
                R"JSON({"type":"object","required":["run_id","cancelled"],"properties":{"run_id":{"type":"string"},"cancelled":{"type":"boolean"}},"additionalProperties":false})JSON")),
        [a](const json& v, const auto&) {
            auto id = v.at("run_id").get<std::string>();
            return mcp_result({{"run_id", id}, {"cancelled", a->cancel(id)}},
                              "Harness Program cancellation processed");
        });
}

}  // namespace neograph::mcp
