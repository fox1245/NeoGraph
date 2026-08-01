#include <neograph/graph/node.h>
#include <neograph/mcp/harness.h>
#include <neograph/mcp/json_schema.h>
#include <neograph/program/transition_store.h>
#include <neograph/program/replay.h>
#include <neograph/provider.h>
#include <neograph/tool.h>
#include "harness_journal_internal.h"
#include "../program/canonical_json.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>

namespace neograph::mcp {
namespace {

bool is_secret_host_key(std::string_view key) {
    return key == "api_key" || key == "authorization" || key == "auth" ||
           key == "bearer_token" || key == "credential" || key == "cookie" ||
           key == "password" || key == "private_key" || key == "refresh_token" ||
           key == "secret" || key == "session" || key == "session_id" ||
           key == "token";
}

json non_secret_host_configuration(const json& value) {
    if (value.is_array()) {
        json result = json::array();
        for (const auto& child : value) result.push_back(non_secret_host_configuration(child));
        return result;
    }
    if (!value.is_object()) return value;
    json result = json::object();
    for (const auto& [key, child] : value.items()) {
        if (!is_secret_host_key(key))
            result[key] = non_secret_host_configuration(child);
    }
    return result;
}

std::string configured_binding_identity(std::string_view route_identity,
                                        const json&      host_configuration) {
    return program::detail::sha256_identity(
        "harness-capability-binding/v2",
        program::detail::canonical_json_bytes(
            {{"route_identity", route_identity},
             {"host_configuration", non_secret_host_configuration(host_configuration)}}));
}

class HarnessBoundProvider final : public Provider {
public:
    HarnessBoundProvider(HarnessWorkerExecutor executor, std::map<std::string, json> tools)
        : executor_(std::move(executor)), tools_(std::move(tools)) {}
    std::string                  get_name() const override { return "harness-owned-provider"; }
    const HarnessWorkerExecutor& executor() const noexcept { return executor_; }
    const std::map<std::string, json>& tools() const noexcept { return tools_; }

private:
    HarnessWorkerExecutor       executor_;
    std::map<std::string, json> tools_;
};

class HarnessBoundTool final : public Tool {
public:
    HarnessBoundTool(std::string name, json definition, HarnessCapabilityExecutor execute)
        : name_(std::move(name)),
          definition_(std::move(definition)),
          execute_(std::move(execute)) {}
    ChatTool get_definition() const override {
        return {name_, definition_.value("description", name_),
                definition_.value("input_schema", json{{"type", "object"}})};
    }
    std::string execute(const json& arguments) override {
        auto value = execute_(definition_, arguments, std::make_shared<graph::CancelToken>());
        return value.is_string() ? value.get<std::string>() : value.dump();
    }
    std::string get_name() const override { return name_; }

private:
    std::string               name_;
    json                      definition_;
    HarnessCapabilityExecutor execute_;
};

constexpr std::string_view RECORDED_CALLS_FIELD = "__recorded_capability_calls";
constexpr std::string_view PROGRAM_PENDING_KIND_FIELD = "__neograph_program_pending_kind";

std::string_view response_kind(HarnessWorkerResponseKind kind) {
    switch (kind) {
        case HarnessWorkerResponseKind::VALUE:
            return "value";
        case HarnessWorkerResponseKind::EMPTY:
            return "empty";
        case HarnessWorkerResponseKind::PARSE_ERROR:
            return "parse_error";
        case HarnessWorkerResponseKind::TOOL_ERROR:
            return "tool_error";
        case HarnessWorkerResponseKind::TIMEOUT:
            return "timeout";
        case HarnessWorkerResponseKind::CANCELLED:
            return "cancelled";
        case HarnessWorkerResponseKind::AWAITING_TOOL_RESULTS:
            return "awaiting_tool_results";
        case HarnessWorkerResponseKind::INPUT_REQUIRED:
            return "input_required";
    }
    throw std::invalid_argument("unknown Harness worker response kind");
}

HarnessWorkerResponseKind response_kind(std::string_view value) {
    if (value == "value") return HarnessWorkerResponseKind::VALUE;
    if (value == "empty") return HarnessWorkerResponseKind::EMPTY;
    if (value == "parse_error") return HarnessWorkerResponseKind::PARSE_ERROR;
    if (value == "tool_error") return HarnessWorkerResponseKind::TOOL_ERROR;
    if (value == "timeout") return HarnessWorkerResponseKind::TIMEOUT;
    if (value == "cancelled") return HarnessWorkerResponseKind::CANCELLED;
    if (value == "awaiting_tool_results")
        return HarnessWorkerResponseKind::AWAITING_TOOL_RESULTS;
    if (value == "input_required") return HarnessWorkerResponseKind::INPUT_REQUIRED;
    throw std::invalid_argument("invalid recorded Harness worker response kind");
}

json recorded_call(const HarnessWorkerCall& call, const HarnessWorkerResponse& response) {
    return {{"worker_id", call.worker.at("worker_id")},
            {"attempt", call.attempt},
            {"kind", std::string(response_kind(response.kind))},
            {"value", response.value},
            {"message", response.message}};
}

HarnessWorkerResponse recorded_response(const json& value) {
    if (!value.is_object() || !value.contains("kind") || !value.at("kind").is_string() ||
        !value.contains("value") || !value.contains("message") ||
        !value.at("message").is_string()) {
        throw std::invalid_argument("recorded Harness capability evidence is malformed");
    }
    return {response_kind(value.at("kind").get<std::string>()), value.at("value"),
            value.at("message").get<std::string>()};
}

struct RecordedHarnessCall {
    std::string           worker_id;
    std::size_t           attempt;
    HarnessWorkerResponse response;
};

class RecordedHarnessCalls {
public:
    explicit RecordedHarnessCalls(std::deque<RecordedHarnessCall> calls)
        : calls_(std::move(calls)) {}

    HarnessWorkerResponse next(const HarnessWorkerCall& call) {
        const auto worker_id = call.worker.at("worker_id").get<std::string>();
        std::lock_guard lock(mutex_);
        if (calls_.empty()) {
            throw std::runtime_error(
                "recorded Harness replay exhausted exact worker capability evidence");
        }
        if (calls_.front().worker_id != worker_id || calls_.front().attempt != call.attempt) {
            throw std::runtime_error(
                "recorded Harness replay call order differs from the source run");
        }
        auto result = std::move(calls_.front().response);
        calls_.pop_front();
        return result;
    }

private:
    std::mutex                      mutex_;
    std::deque<RecordedHarnessCall> calls_;
};

class HarnessWorkerNode final : public graph::GraphNode {
public:
    HarnessWorkerNode(json config, std::shared_ptr<HarnessBoundProvider> provider)
        : config_(std::move(config)), provider_(std::move(provider)) {}
    std::string get_name() const override { return config_.at("worker_id").get<std::string>(); }
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
        HarnessWorkerCall call;
        call.task   = in.state.get("task");
        call.worker = config_;
        call.policy = {{"evidence_required", config_.value("evidence_required", json::array())},
                       {"read_only", config_.value("read_only", false)}};
        call.resume_value = in.ctx.resume_value;
        call.run_id       = in.ctx.run_id;
        call.usage        = in.ctx.usage;
        call.model_token_budget = in.ctx.model_token_budget;
        call.budget_exhausted   = in.ctx.budget_exhausted;
        const auto provider_timeout_ms = config_.at("provider_timeout_ms").get<std::uint64_t>();
        const auto max_output_tokens = config_.at("max_output_tokens").get<std::uint64_t>();
        const auto input_token_ceiling =
            config_.at("input_token_ceiling").get<std::uint64_t>();
        const auto max_tool_rounds =
            config_.at("max_provider_tool_rounds").get<std::uint64_t>();
        if (provider_timeout_ms == 0 || provider_timeout_ms % 1000 != 0 ||
            max_output_tokens == 0 || input_token_ceiling == 0 || max_tool_rounds == 0) {
            throw std::invalid_argument("Harness worker has invalid sealed provider limits");
        }
        call.worker["_harness_provider_budget"] = {
            {"provider_timeout_seconds", provider_timeout_ms / 1000},
            {"max_output_tokens", max_output_tokens},
            {"input_token_ceiling", input_token_ceiling},
        };
        for (const auto& id : config_.at("tool_ids")) {
            const auto found = provider_->tools().find(id.get<std::string>());
            if (found == provider_->tools().end())
                throw std::invalid_argument("unbound Harness Tool");
            call.tool_catalog.push_back(found->second);
        }
        json recorded_calls = json::array();
        auto worker_cancel =
            (call.model_token_budget != 0 && in.ctx.budget_cancel_token)
                ? in.ctx.budget_cancel_token
                : (in.ctx.cancel_token ? in.ctx.cancel_token->fork()
                                        : std::make_shared<graph::CancelToken>());
        const auto attempts = config_.at("max_retries").get<std::size_t>() + 1;
        for (std::size_t attempt = 1; attempt <= attempts; ++attempt) {
            call.attempt = attempt;
            detail::HarnessJournalContext journal_context;
            journal_context.run_id    = call.run_id;
            journal_context.node_id   = get_name();
            journal_context.worker_id = call.worker.at("worker_id").get<std::string>();
            journal_context.attempt   = attempt;
            detail::ScopedHarnessJournalContext journal_scope(std::move(journal_context));
            auto response = provider_->executor()(call, worker_cancel);
            recorded_calls.push_back(recorded_call(call, response));
            if (response.kind == HarnessWorkerResponseKind::AWAITING_TOOL_RESULTS ||
                response.kind == HarnessWorkerResponseKind::INPUT_REQUIRED) {
                auto pending = response.value;
                if (pending.is_object() && pending.contains("call_id") &&
                    pending.at("call_id").is_string() && pending.contains("result_schema") &&
                    pending.at("result_schema").is_object()) {
                    pending[std::string(PROGRAM_PENDING_KIND_FIELD)] =
                        pending.contains("effect")
                            ? "effect"
                            : response.kind == HarnessWorkerResponseKind::AWAITING_TOOL_RESULTS
                                  ? "capability_result"
                                  : "input";
                }
                throw graph::NodeInterrupt(
                    response.message.empty() ? "Harness worker input required" : response.message,
                    std::move(pending));
            }
            if (response.kind == HarnessWorkerResponseKind::VALUE) {
                try {
                    validate_json_value(response.value, config_.at("output_schema"),
                                        "Harness worker result", "$");
                    co_return graph::NodeOutput{{graph::ChannelWrite{
                        "worker_results",
                        json{{"worker_id", config_.at("worker_id")},
                             {"status", "completed"},
                             {"result", std::move(response.value)},
                             {std::string(RECORDED_CALLS_FIELD), std::move(recorded_calls)}}}}};
                } catch (const std::exception& error) {
                    call.repair_feedback = error.what();
                }
            } else {
                call.repair_feedback = response.message;
                if (response.kind == HarnessWorkerResponseKind::CANCELLED) {
                    if (!call.budget_exhausted ||
                        !call.budget_exhausted->load(std::memory_order_acquire))
                        throw graph::CancelledException(
                            response.message.empty() ? "Harness worker cancelled"
                                                     : response.message);
                    break;
                }
            }
        }
        co_return graph::NodeOutput{{graph::ChannelWrite{
            "worker_results",
            json{{"worker_id", config_.at("worker_id")},
                 {"status", "failed"},
                 {"error", call.repair_feedback},
                 {std::string(RECORDED_CALLS_FIELD), std::move(recorded_calls)}}}}};
    }

private:
    json                                  config_;
    std::shared_ptr<HarnessBoundProvider> provider_;
};

class HarnessJudgeNode final : public graph::GraphNode {
public:
    std::string                        get_name() const override { return "harness-judge"; }
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
        const auto  workers = in.state.get("worker_results");
        json        public_workers = json::array();
        json        findings = json::array(), sources = json::array();
        std::size_t valid = 0, failed = 0;
        for (const auto& worker : workers) {
            json public_worker = json::object();
            for (const auto& [field, value] : worker.items()) {
                if (field != RECORDED_CALLS_FIELD) public_worker[field] = value;
            }
            public_workers.push_back(std::move(public_worker));
            if (worker.value("status", "failed") == "completed") {
                ++valid;
                const auto& result = worker.at("result");
                if (result.is_object() && result.contains("findings") &&
                    result.at("findings").is_array())
                    for (const auto& finding : result.at("findings")) {
                        findings.push_back(finding);
                        sources.push_back(worker.at("worker_id"));
                    }
            } else
                ++failed;
        }
        std::string outcome =
            valid == 0 ? "failed"
                       : (failed ? "partial" : (findings.empty() ? "zero_findings" : "ok"));
        json      result{{"outcome", outcome},
                         {"workers", std::move(public_workers)},
                         {"findings", std::move(findings)},
                         {"finding_sources", std::move(sources)},
                         {"valid_workers", valid},
                         {"failed_workers", failed}};
        co_return graph::NodeOutput{{graph::ChannelWrite{"final_result", std::move(result)}}};
    }
};

program::ExecutableIdentity installed_node_identity(program::ExecutableKind kind,
                                                    std::string            name,
                                                    std::string_view       component,
                                                    std::string_view       compiler_build_id) {
    const json descriptor = {
        {"component", component},
        {"compiler_build_id", compiler_build_id},
        {"kind", std::string(program::to_string(kind))},
        {"name", name},
        {"semantic_version", "1.0.0"},
    };
    return {kind, std::move(name), "1.0.0",
            program::detail::sha256_identity(
                "neograph-harness-installed-node/v1",
                program::detail::canonical_json_bytes(descriptor))};
}

HarnessNodeRegistration worker_registration(std::string_view compiler_build_id) {
    HarnessNodeRegistration r;
    r.manifest = {
        installed_node_identity(program::ExecutableKind::Node,
                                std::string(HARNESS_WORKER_NODE_TYPE), "worker-node/v1",
                                compiler_build_id),
        program::EffectMode::Brokered,
        "neograph-harness-worker-v1",
        {},
        {},
        {}};
    r.config_schema = {
        {"type", "object"},
        {"required",
         json::array({"worker_id", "instructions", "tool_ids", "tool_descriptions",
                      "output_schema", "provider_timeout_ms", "max_output_tokens",
                      "input_token_ceiling", "max_retries", "max_provider_tool_rounds",
                      "evidence_required", "read_only"})},
        {"properties",
         {{"worker_id", {{"type", "string"}}},
          {"instructions", {{"type", "string"}}},
          {"tool_ids", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"tool_descriptions", {{"type", "object"}}},
          {"output_schema", {{"type", "object"}}},
          {"provider_timeout_ms", {{"type", "integer"}}},
          {"max_output_tokens", {{"type", "integer"}}},
          {"input_token_ceiling", {{"type", "integer"}}},
          {"max_retries", {{"type", "integer"}}},
          {"max_provider_tool_rounds", {{"type", "integer"}}},
          {"evidence_required", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"read_only", {{"type", "boolean"}}}}},
        {"additionalProperties", false}};
    r.effects =
        json{{"reads", json::array({"task"})}, {"writes", json::array({"worker_results"})}};
    r.factory = [](const std::string&, const json& config, const graph::NodeContext& context) {
        auto provider = std::dynamic_pointer_cast<HarnessBoundProvider>(context.provider);
        if (!provider)
            throw std::invalid_argument("Harness worker requires owned Harness Provider binding");
        return std::make_unique<HarnessWorkerNode>(config, std::move(provider));
    };
    return r;
}

HarnessNodeRegistration judge_registration(std::string_view compiler_build_id) {
    HarnessNodeRegistration r;
    r.manifest = {
        installed_node_identity(program::ExecutableKind::Node,
                                std::string(HARNESS_JUDGE_NODE_TYPE), "judge-node/v1",
                                compiler_build_id),
        program::EffectMode::Brokered,
        "neograph-harness-judge-v1",
        {},
        {},
        {}};
    r.config_schema = {
        {"type", "object"},
        {"properties",
         {{"barrier",
           {{"type", "object"},
            {"required", json::array({"wait_for"})},
            {"properties",
             {{"wait_for", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
            {"additionalProperties", false}}}}},
        {"additionalProperties", false}};
    r.effects =
        json{{"reads", json::array({"worker_results"})}, {"writes", json::array({"final_result"})}};
    r.factory = [](const std::string&, const json&, const graph::NodeContext&) {
        return std::make_unique<HarnessJudgeNode>();
    };
    return r;
}

program::RecordedBindingSet make_recorded_binding(
    const program::ProgramVersion& version, const HarnessCapabilityBindingRequest& requested,
    const std::vector<program::ProgramEvent>& events) {
    const auto exact_bindings =
        version.core_materialization_receipt().capability_bindings;
    const auto provider_receipt = std::find_if(
        exact_bindings.begin(), exact_bindings.end(), [](const auto& receipt) {
            return receipt.executable.kind == program::ExecutableKind::Provider;
        });

    std::deque<RecordedHarnessCall>                    replay_calls;
    std::vector<program::RecordedCapabilityCallReference> expected_calls;
    std::vector<program::RecordedCapabilityEvidence>  evidence;
    std::uint64_t                                      call_sequence = 0;
    for (const auto& event : events) {
        if (event.kind != program::ProgramEventKind::Core) continue;
        const auto* typed = std::get_if<graph::TypedGraphEvent>(&event.payload);
        if (!typed) continue;
        const auto* write = std::get_if<graph::ChannelWriteEvent>(typed);
        if (!write || write->channel != "worker_results" || !write->value.is_object() ||
            !write->value.contains(std::string(RECORDED_CALLS_FIELD)))
            continue;
        const auto& calls = write->value.at(std::string(RECORDED_CALLS_FIELD));
        if (!calls.is_array())
            throw std::invalid_argument("recorded Harness call ledger is not an array");
        if (provider_receipt == exact_bindings.end())
            throw std::invalid_argument(
                "recorded Harness calls have no exact Provider binding receipt");

        for (const auto& call : calls) {
            if (!call.is_object() || !call.contains("worker_id") ||
                !call.at("worker_id").is_string() || !call.contains("attempt") ||
                !call.at("attempt").is_number_unsigned())
                throw std::invalid_argument("recorded Harness call coordinate is malformed");
            const auto worker_id = call.at("worker_id").get<std::string>();
            const auto attempt   = call.at("attempt").get<std::size_t>();
            const auto response  = recorded_response(call);
            const auto call_id   = "harness-call-" + std::to_string(++call_sequence);

            program::RecordedCapabilityCallReference reference{
                call_sequence, *provider_receipt, event.operation_id, call_id, std::nullopt};
            program::ProgramPendingInputData outcome_data{
                event.operation_id,
                call_id,
                program::ProgramPendingInputKind::CapabilityResult,
                json::object(),
                json{{"worker_id", worker_id}, {"attempt", attempt}},
                std::nullopt,
                worker_id,
                json::object(),
                program::ProgramPendingState::Consumed,
                call};
            program::RecordedCapabilityEvidenceData evidence_data;
            evidence_data.reference     = reference;
            evidence_data.coverage      = program::RecordedEvidenceCoverage::Full;
            evidence_data.input_outcome =
                program::ProgramPendingInput(std::move(outcome_data));
            expected_calls.push_back(std::move(reference));
            evidence.emplace_back(std::move(evidence_data));
            replay_calls.push_back({worker_id, attempt, response});
        }
    }

    std::map<std::string, json> tool_configs;
    for (const auto& binding : requested.tools)
        tool_configs.emplace(binding.host_configuration.at("id").get<std::string>(),
                             binding.host_configuration);
    auto recorded_calls = std::make_shared<RecordedHarnessCalls>(std::move(replay_calls));
    program::CatalogCapabilityBinding owned;
    std::vector<std::unique_ptr<Tool>> tools;
    for (const auto& receipt : exact_bindings) {
        if (receipt.executable.kind == program::ExecutableKind::Provider) {
            if (!requested.provider || *requested.provider != receipt.executable)
                throw std::invalid_argument(
                    "recorded Harness Provider identity differs from the target request");
            owned.node_context.provider = std::make_shared<HarnessBoundProvider>(
                [recorded_calls](const HarnessWorkerCall& call,
                                 const std::shared_ptr<graph::CancelToken>&) {
                    return recorded_calls->next(call);
                },
                tool_configs);
        } else if (receipt.executable.kind == program::ExecutableKind::Tool) {
            const auto binding = std::find_if(
                requested.tools.begin(), requested.tools.end(),
                [&receipt](const auto& value) {
                    return value.executable == receipt.executable;
                });
            if (binding == requested.tools.end())
                throw std::invalid_argument(
                    "recorded Harness Tool identity differs from the target request");
            tools.push_back(std::make_unique<HarnessBoundTool>(
                receipt.executable.name, binding->host_configuration,
                [](const json&, const json&, const std::shared_ptr<graph::CancelToken>&)
                    -> json {
                    throw std::runtime_error(
                        "recorded Harness replay cannot execute an unrecorded Tool call");
                }));
        }
    }
    owned.tools    = ToolSet(std::move(tools));
    owned.receipts = exact_bindings;
    return program::RecordedBindingSet(exact_bindings, std::move(expected_calls),
                                       std::move(owned), std::move(evidence));
}

}  // namespace

HarnessServiceResources make_harness_program_service_resources(HarnessProgramHostConfig config) {
    if (!config.worker_executor || !config.checkpoints || !config.state_store ||
        (!config.snapshots.registry.tools.empty() && !config.capability_executor) ||
        config.compiler_build_id.empty() || config.provider_binding_identity.empty() ||
        config.snapshots.owner_scope.empty() || !config.provider_host_configuration.is_object())
        throw std::invalid_argument("incomplete Harness Program host config");

    config.snapshots.registry.worker = worker_registration(config.compiler_build_id);
    config.snapshots.registry.judge  = judge_registration(config.compiler_build_id);
    std::map<std::string, json> tool_metadata;
    for (const auto& registration : config.snapshots.registry.tools) {
        tool_metadata.emplace(
            registration.manifest.identity.name,
            json{{"input_schema", registration.metadata.input_schema},
                 {"output_schema", registration.metadata.output_schema}});
    }
    auto snapshots                   = build_harness_program_snapshots(std::move(config.snapshots));
    auto engines =
        config.engines ? config.engines : std::make_shared<program::EngineGenerationCache>();
    auto compiler = std::make_shared<program::ProgramCompiler>(
        snapshots.registry, program::ProgramCompilerConfig{config.compiler_build_id});
    auto worker_executor           = std::move(config.worker_executor);
    auto capability_executor       = std::move(config.capability_executor);
    auto provider_binding_identity = std::move(config.provider_binding_identity);
    auto provider_host_configuration = std::move(config.provider_host_configuration);
    auto artifact_binding_identity =
        configured_binding_identity(provider_binding_identity, provider_host_configuration);
    auto tool_binding_identities   = std::move(config.tool_binding_identities);
    auto registry                  = snapshots.registry;
    auto compiler_build_id         = std::move(config.compiler_build_id);
    auto catalog_factory =
        [registry, engines, compiler_build_id, worker_executor, capability_executor,
         provider_binding_identity, provider_host_configuration, tool_binding_identities,
         tool_metadata](
                               std::shared_ptr<program::ProgramStore> store,
                               const HarnessCapabilityBindingRequest& requested) {
        program::CatalogConfig cc{std::move(store), registry, engines, compiler_build_id, {}, 1};
        cc.capability_binder =
            [requested, worker_executor, capability_executor, provider_binding_identity,
             provider_host_configuration, tool_binding_identities, tool_metadata](
                const std::vector<program::ExecutableIdentity>& closure) {
                program::CatalogCapabilityBinding  result;
                std::map<std::string, json>       tool_configs;
                std::vector<std::unique_ptr<Tool>> tools;
                for (const auto& binding : requested.tools) {
                    const auto id       = binding.host_configuration.at("id").get<std::string>();
                    const auto metadata = tool_metadata.find(id);
                    if (metadata == tool_metadata.end())
                        throw std::invalid_argument("missing exact Harness Tool metadata");
                    auto provider_configuration = binding.host_configuration;
                    provider_configuration["input_schema"] =
                        metadata->second.at("input_schema");
                    provider_configuration["output_schema"] =
                        metadata->second.at("output_schema");
                    tool_configs.emplace(id, std::move(provider_configuration));
                }
            json provider_configuration = provider_host_configuration;
            provider_configuration["tools"] = json::object();
            for (const auto& [id, definition] : tool_configs)
                provider_configuration["tools"][id] = definition;
            for (const auto& executable : closure) {
                if (executable.kind == program::ExecutableKind::Provider) {
                    if (!requested.provider || *requested.provider != executable ||
                        provider_binding_identity.empty())
                        throw std::invalid_argument(
                            "missing exact stable Harness Provider binding");
                    result.node_context.provider =
                        std::make_shared<HarnessBoundProvider>(worker_executor, tool_configs);
                    result.receipts.push_back(
                        {executable,
                         configured_binding_identity(provider_binding_identity,
                                                     provider_configuration)});
                    continue;
                }
                const auto binding = std::find_if(
                    requested.tools.begin(), requested.tools.end(),
                    [&executable](const auto& value) { return value.executable == executable; });
                const auto receipt = tool_binding_identities.find(executable.name);
                if (binding == requested.tools.end() || receipt == tool_binding_identities.end() ||
                    receipt->second.empty())
                    throw std::invalid_argument("missing exact stable Harness Tool binding");
                json definition = binding->host_configuration;
                tools.push_back(std::make_unique<HarnessBoundTool>(
                    executable.name, std::move(definition), capability_executor));
                result.receipts.push_back(
                    {executable,
                     configured_binding_identity(receipt->second,
                                                 binding->host_configuration)});
            }
            result.tools = ToolSet(std::move(tools));
            return result;
        };
        return std::make_shared<program::ProgramCatalog>(std::move(cc));
    };
    auto checkpoints       = std::move(config.checkpoints);
    auto state_store       = std::move(config.state_store);
    auto scheduler_threads = config.scheduler_threads;
    auto runtime_factory   = [checkpoints, state_store, scheduler_threads](
                               std::shared_ptr<program::ProgramCatalog>         catalog,
                               std::shared_ptr<program::ProgramTransitionStore> transitions) {
        program::RuntimeConfig rc;
        rc.catalog           = std::move(catalog);
        rc.checkpoints       = checkpoints;
        rc.state_store       = state_store;
        rc.transitions       = std::move(transitions);
        rc.scheduler_threads = scheduler_threads;
        return std::make_shared<program::ProgramRuntime>(std::move(rc));
    };
    auto recorded_binding_factory = [](const program::ProgramVersion& version,
                                       const HarnessCapabilityBindingRequest& requested,
                                       const std::vector<program::ProgramEvent>& events) {
        return make_recorded_binding(version, requested, events);
    };
    auto owner_scope = snapshots.policy.owner_scope();
    return {std::move(snapshots), std::move(compiler), std::move(catalog_factory),
            std::move(runtime_factory), std::move(recorded_binding_factory),
            std::move(owner_scope), std::move(artifact_binding_identity)};
}

}  // namespace neograph::mcp
