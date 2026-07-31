#pragma once

#include <neograph/api.h>
#include <neograph/graph/loader.h>
#include <neograph/program/admission.h>
#include <neograph/program/runtime.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::mcp {

inline constexpr std::string_view HARNESS_WORKER_NODE_TYPE = "neograph_harness_worker";
inline constexpr std::string_view HARNESS_JUDGE_NODE_TYPE  = "neograph_harness_judge";

class NEOGRAPH_HARNESS_API HarnessTranslationError final : public std::invalid_argument {
public:
    HarnessTranslationError(std::string code, std::string pointer, std::string message);

    const std::string& code() const noexcept;
    const std::string& pointer() const noexcept;

private:
    std::string code_;
    std::string pointer_;
};

struct HarnessTranslationDefaults {
    std::uint64_t timeout_seconds              = 600;
    std::uint32_t max_parallel_workers         = 4;
    std::uint64_t max_core_steps               = 40;
    std::uint32_t max_worker_retries           = 1;
    std::uint64_t provider_timeout_seconds     = 600;
    std::uint64_t max_output_tokens            = 4096;
    std::uint64_t input_token_ceiling_per_round = 16384;
    std::uint32_t max_provider_tool_rounds     = 8;
    std::uint64_t monetary_microunits          = 1;
    std::optional<program::ExecutableIdentity> provider;
    std::vector<std::string>                   read_only_effects;
    std::string                                source_id_prefix = "harness";
};

struct HarnessWireReceipt {
    std::string              source_id;
    std::string              mode;
    std::string              preset;
    std::vector<std::string> worker_ids;
    std::vector<std::string> tool_ids;
    json                     legacy_projection;
};

struct HarnessCapabilityBinding {
    program::ExecutableIdentity executable;
    json                        host_configuration;
};

struct HarnessCapabilityBindingRequest {
    std::optional<program::ExecutableIdentity> provider;
    std::vector<HarnessCapabilityBinding>      tools;
    json                                       policy_restrictions;
};

struct HarnessTranslation {
    program::ProgramSource             source;
    program::ProgramInvocation         invocation;
    HarnessWireReceipt                 wire;
    HarnessCapabilityBindingRequest    bindings;
};

class NEOGRAPH_HARNESS_API HarnessRequestTranslator final {
public:
    static HarnessTranslation translate(const json&                          request,
                                        const program::RegistrySnapshot&      registry,
                                        const HarnessTranslationDefaults&     defaults);
};

NEOGRAPH_HARNESS_API json harness_program_request_schema();
NEOGRAPH_HARNESS_API json harness_program_output_schema();

struct HarnessNodeRegistration {
    program::ExecutableManifest            manifest;
    graph::NodeFactoryFn                   factory;
    json                                   config_schema;
    json                                   effects;
    program::ExecutableRequirementResolver requirement_resolver;
};

struct HarnessReducerRegistration {
    program::ExecutableManifest manifest;
    graph::ReducerFn             reducer;
};

struct HarnessConditionRegistration {
    program::ExecutableManifest          manifest;
    graph::ConditionFn                   condition;
    std::optional<graph::ConditionSpec>  spec;
};

struct HarnessProviderRegistration {
    program::ExecutableManifest manifest;
    program::ProviderMetadata   metadata;
};

struct HarnessToolRegistration {
    program::ExecutableManifest manifest;
    program::ToolMetadata       metadata;
};

struct HarnessProgramRegistryConfig {
    HarnessNodeRegistration                    worker;
    HarnessNodeRegistration                    judge;
    std::vector<HarnessNodeRegistration>       additional_nodes;
    std::vector<HarnessReducerRegistration>    additional_reducers;
    std::vector<HarnessConditionRegistration>  conditions;
    std::optional<HarnessProviderRegistration> provider;
    std::vector<HarnessToolRegistration>       tools;
};

struct HarnessProgramSnapshotConfig {
    HarnessProgramRegistryConfig registry;
    std::string                  admission_profile_id      = "neograph-harness-v1";
    std::string                  admission_semantic_version = "1.0.0";
    program::AdmissionMode       admission_mode = program::AdmissionMode::MultiTenant;
    std::string                  policy_id       = "neograph-harness-policy-v1";
    std::string                  policy_semantic_version = "1.0.0";
    std::string                  owner_scope;
    std::vector<std::string>     allowed_capabilities;
    std::vector<std::string>     allowed_effects;
    std::vector<std::string>     allowed_module_digests;
    program::BudgetLimits        budget_ceiling;
};

struct HarnessProgramSnapshots {
    program::RegistrySnapshot registry;
    program::AdmissionProfile admission_profile;
    program::PolicySnapshot   policy;
};

NEOGRAPH_HARNESS_API HarnessProgramSnapshots
build_harness_program_snapshots(HarnessProgramSnapshotConfig config);

}  // namespace neograph::mcp
