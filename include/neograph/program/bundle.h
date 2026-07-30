/**
 * @file program/bundle.h
 * @brief Immutable, content-addressed Program compilation artifacts.
 */
#pragma once

#include <neograph/program/source.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

struct ProgramBundleData {
    std::string                 source_hash;
    std::string                 canonical_program_hash;
    std::string                 compiler_build_id;
    std::uint32_t               program_schema_version = 1;
    std::string                 registry_snapshot_fingerprint;
    std::string                 module_dependency_merkle_root;
    json                        input_contract                 = json::object();
    json                        output_contract                = json::object();
    json                        orchestration_plan             = json::object();
    json                        core_compiled_plan_identities  = json::array();
    json                        capability_effect_closure      = json::object();
    json                        executable_registry_identities = json::array();
    json                        declared_budget_requirements   = json::object();
    std::vector<SourceMapEntry> source_map;
    std::vector<Diagnostic>     diagnostics;
};

class NEOGRAPH_PROGRAM_API ProgramBundle {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    explicit ProgramBundle(ProgramBundleData data);
    static ProgramBundle parse(std::string_view stored_bytes);

    const std::string&                 id() const noexcept;
    const std::string&                 source_hash() const noexcept;
    const std::string&                 canonical_program_hash() const noexcept;
    const std::string&                 compiler_build_id() const noexcept;
    std::uint32_t                      program_schema_version() const noexcept;
    const std::string&                 registry_snapshot_fingerprint() const noexcept;
    const std::string&                 module_dependency_merkle_root() const noexcept;
    json                               input_contract() const;
    json                               output_contract() const;
    json                               orchestration_plan() const;
    json                               core_compiled_plan_identities() const;
    json                               capability_effect_closure() const;
    json                               executable_registry_identities() const;
    json                               declared_budget_requirements() const;
    const std::vector<SourceMapEntry>& source_map() const noexcept;
    const std::vector<Diagnostic>&     diagnostics() const noexcept;
    std::string                        serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramBundle(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

}  // namespace neograph::program
