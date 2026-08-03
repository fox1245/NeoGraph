/**
 * @file program/catalog.h
 * @brief Admission and process-local materialization of immutable Programs.
 */
#pragma once

#include <neograph/program/activation.h>
#include <neograph/program/admission.h>
#include <neograph/program/migration.h>
#include <neograph/tool_set.h>

#include <functional>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

namespace detail {
class CatalogRuntimeAccess;
struct MaterializedProgram;
}

class ProgramStore;

struct ProgramAdmission {
    std::string                    owner_scope;
    AdmissionProfile               profile;
    PolicySnapshot                 policy;
    std::vector<DependencyReceipt> dependency_receipts;
};

class NEOGRAPH_PROGRAM_API ProgramAdmissionError final : public std::runtime_error {
public:
    explicit ProgramAdmissionError(std::vector<Diagnostic> diagnostics);

    const std::vector<Diagnostic>& diagnostics() const noexcept;

private:
    std::vector<Diagnostic> diagnostics_;
};

/**
 * Process-local cache of fully linked Core generations.
 *
 * The cache exposes no GraphEngine surface. It may be shared only by Catalogs
 * with the same immutable registry fingerprint and compiler build identity.
 */
class NEOGRAPH_PROGRAM_API EngineGenerationCache {
public:
    EngineGenerationCache();
    EngineGenerationCache(EngineGenerationCache&&) noexcept;
    EngineGenerationCache& operator=(EngineGenerationCache&&) noexcept;
    EngineGenerationCache(const EngineGenerationCache&)            = delete;
    EngineGenerationCache& operator=(const EngineGenerationCache&) = delete;
    ~EngineGenerationCache();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class ProgramCatalog;
};

/**
 * Exact host-resource receipt returned by the Catalog capability binder.
 *
 * The NodeContext owns its Provider through shared ownership. ToolSet owns every
 * Tool; Catalog replaces NodeContext::tools with ToolSet::view() before invoking
 * any node factory, so no borrowed Tool pointer crosses materialization.
 */
struct CatalogCapabilityBinding {
    graph::NodeContext                    node_context;
    ToolSet                               tools;
    std::vector<CapabilityBindingReceipt> receipts;
};

using CatalogCapabilityBinder =
    std::function<CatalogCapabilityBinding(const std::vector<ExecutableIdentity>&)>;

struct CatalogConfig {
    std::shared_ptr<ProgramStore>          program_store;
    RegistrySnapshot                       registry;
    std::shared_ptr<EngineGenerationCache> engines;
    std::string                            compiler_build_id;
    CatalogCapabilityBinder                capability_binder;
    std::size_t                            worker_count = 1;
};

class NEOGRAPH_PROGRAM_API ProgramCatalog {
public:
    explicit ProgramCatalog(CatalogConfig config);
    ProgramCatalog(ProgramCatalog&&) noexcept;
    ProgramCatalog& operator=(ProgramCatalog&&) noexcept;
    ProgramCatalog(const ProgramCatalog&)            = delete;
    ProgramCatalog& operator=(const ProgramCatalog&) = delete;
    ~ProgramCatalog();

    ProgramVersion                admit(const ProgramBundle& bundle, ProgramAdmission admission);
    std::optional<ProgramVersion> find_version(std::string_view id) const;
    std::optional<ProgramVersion> resolve_version(std::string_view owner_scope,
                                                  std::string_view id);
    std::optional<ProgramVersion> resolve_version_with_binding(
        std::string_view owner_scope, std::string_view id, CatalogCapabilityBinding binding);

    /** Build a side-effect-free compatibility proof for a version transition. */
    MigrationPlan plan_migration(std::string_view owner_scope,
                                 std::string_view source_version_id,
                                 std::string_view target_version_id) const;
    /** Materialize and preflight a version before publishing its activation pointer. */
    ProgramActivationResult activate(std::string_view owner_scope,
                                     std::string_view version_id,
                                     std::uint64_t expected_generation);
    std::optional<ProgramActivation>
    activation(std::string_view owner_scope) const;
    ProgramRetentionReport collect_retention(
        std::string_view owner_scope,
        const std::vector<std::string>& pinned_version_ids);

private:
    ProgramVersion materialize(
        const ProgramBundle& bundle, ProgramAdmission admission,
        std::optional<CatalogCapabilityBinding> supplied_binding,
        const ProgramVersion* expected_version, bool publish,
        std::shared_ptr<const detail::MaterializedProgram>* isolated_materialization = nullptr);
    std::optional<ProgramVersion> resolve_version_impl(
        std::string_view owner_scope, std::string_view id,
        std::optional<CatalogCapabilityBinding> supplied_binding,
        std::shared_ptr<const detail::MaterializedProgram>* isolated_materialization = nullptr);

    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class detail::CatalogRuntimeAccess;
};

}  // namespace neograph::program
