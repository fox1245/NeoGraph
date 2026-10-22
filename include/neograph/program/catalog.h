/**
 * @file program/catalog.h
 * @brief Admission and process-local materialization of immutable Programs.
 */
#pragma once

#include <neograph/graph/configuration_failure.h>
#include <neograph/graph/types.h>
#include <neograph/program/activation.h>
#include <neograph/program/admission.h>
#include <neograph/program/migration.h>
#include <neograph/program/module.h>
#include <neograph/program/store.h>
#include <neograph/tool_set.h>

#include <asio/awaitable.hpp>

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
class RecordedBindingSet;
struct RecordedCapabilityMaterialization;
struct ProgramEvent;

struct ProgramAdmission {
    std::string                    owner_scope;
    AdmissionProfile               profile;
    PolicySnapshot                 policy;
    std::vector<DependencyReceipt> dependency_receipts;
};

/// Also a graph::ConfigurationFailure (#364): inside a Core node it is
/// NG.ConfigError, never retried and never routed.
class NEOGRAPH_PROGRAM_API ProgramAdmissionError final : public std::runtime_error,
                                          public graph::ConfigurationFailure {
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
 * with the same immutable registry/compiler identity; materialization context
 * identity is an additional key so incompatible provider/tool/store bindings
 * cannot reuse a generation. Context identities are non-secret host labels.
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
    /** Required for non-empty dependency receipts and composed admissions. */
    std::shared_ptr<const ModuleStore>     module_store;
    /**
     * Authenticated embedding identity for trusted-native materialization.
     * Empty is valid for brokered-only catalogs; a trusted-native closure
     * fails closed unless this identity matches every trusted registry entry's
     * host attestation.
     */
    std::string                            host_identity;
    /**
     * Stable, non-secret construction-context identity supplied by trusted
     * host code. It partitions generation-cache entries when the same
     * topology is bound to different Provider, ToolSet, policy, Store, or
     * host registry resources. Credentials and topology JSON MUST NOT be
     * placed here. Empty retains legacy identity derived from exact receipts.
     */
    std::string                            materialization_context_identity;
    /**
     * Trusted host mediation for recorded execution. Receives only persisted
     * source events, and constructs captured-only capabilities for this exact
     * admitted version. Public replay callers cannot supply this dispatcher.
     * Absent mediation rejects recorded execution before source allocation.
     */
    std::function<RecordedCapabilityMaterialization(const ProgramVersion&, const std::vector<ProgramEvent>&)>
        recorded_capability_binder;
    /**
     * #364: the redactor every Core engine this Catalog links uses for the
     * free text of failure records (graph::FailureTextRedactor). Empty writes
     * no text. It is a host policy: Catalogs that share an
     * EngineGenerationCache with different redactors need distinct
     * materialization_context_identity values.
     */
    graph::FailureTextRedactor failure_text_redactor;
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
    /**
     * Validate and admit a parent only after every child bundle/version is
     * already admitted and the immutable module closure is resolved.
     */
    ProgramVersion                admit_composed(const ProgramBundle& bundle,
                                                 ProgramAdmission admission,
                                                 const ProgramComposition& composition);
    std::optional<ProgramVersion> find_version(std::string_view id) const;
    std::optional<ProgramVersion> find_version(std::string_view owner_scope,
                                               std::string_view id) const;
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
    /** CAS-publish a previously admitted version as the next owner-scoped activation. */
    ProgramActivationResult rollback(std::string_view owner_scope,
                                     std::string_view version_id,
                                     std::uint64_t expected_generation);
    std::optional<ProgramActivation>
    activation(std::string_view owner_scope) const;
    /** Read one coherent activation/version tuple from the durable store. */
    std::optional<ProgramActivationBinding>
    active_binding(std::string_view owner_scope) const;
    ProgramRetentionReport collect_retention(
        std::string_view owner_scope,
        const std::vector<std::string>& pinned_version_ids);

    /**
     * Awaitable forms of the operations above. Each one is the body its synchronous namesake runs: it awaits the
     * catalog's stores on the executor of the coroutine that awaits it, so a store that completes only when a
     * host's event loop is pumped works, and nothing blocks a thread. They take their arguments by value. The
     * synchronous forms keep their meaning for a threaded host; inside a turn of an async::HostLoop they are
     * refused (ProgramBlockingCallRefused) when a store is not synchronous.
     */
    asio::awaitable<ProgramVersion> admit_async(ProgramBundle bundle, ProgramAdmission admission);
    asio::awaitable<ProgramVersion> admit_composed_async(ProgramBundle bundle,
                                                         ProgramAdmission admission,
                                                         ProgramComposition composition);
    asio::awaitable<std::optional<ProgramVersion>> find_version_async(std::string id) const;
    asio::awaitable<std::optional<ProgramVersion>> find_version_async(std::string owner_scope,
                                                                      std::string id) const;
    asio::awaitable<std::optional<ProgramVersion>> resolve_version_async(std::string owner_scope,
                                                                         std::string id);
    asio::awaitable<std::optional<ProgramVersion>> resolve_version_with_binding_async(
        std::string owner_scope, std::string id, CatalogCapabilityBinding binding);
    asio::awaitable<MigrationPlan> plan_migration_async(std::string owner_scope,
                                                        std::string source_version_id,
                                                        std::string target_version_id) const;
    asio::awaitable<ProgramActivationResult> activate_async(std::string owner_scope,
                                                            std::string version_id,
                                                            std::uint64_t expected_generation);
    asio::awaitable<ProgramActivationResult> rollback_async(std::string owner_scope,
                                                            std::string version_id,
                                                            std::uint64_t expected_generation);
    asio::awaitable<std::optional<ProgramActivation>> activation_async(std::string owner_scope) const;
    asio::awaitable<std::optional<ProgramActivationBinding>> active_binding_async(
        std::string owner_scope) const;
    /// True when every store this catalog reads completes its awaitable operations at once (Sync...Store), so that a
    /// synchronous call can never wait for another thread or for a host's event loop.
    bool completes_inline() const noexcept;
    asio::awaitable<ProgramRetentionReport> collect_retention_async(
        std::string owner_scope, std::vector<std::string> pinned_version_ids);

private:
    // The bodies behind the public operations; each is awaited by the awaitable form and run to completion by the
    // synchronous one. Arguments are by value so that nothing dangles across a suspension.
    asio::awaitable<ProgramVersion> materialize_async(
        ProgramBundle bundle, ProgramAdmission admission,
        std::optional<CatalogCapabilityBinding> supplied_binding,
        std::optional<ProgramVersion> expected_version, bool publish,
        ExecutionGuarantee effective_execution_guarantee,
        std::shared_ptr<const detail::MaterializedProgram>* isolated_materialization = nullptr);
    asio::awaitable<std::optional<ProgramVersion>> resolve_version_impl_async(
        std::string owner_scope, std::string id,
        std::optional<CatalogCapabilityBinding> supplied_binding,
        std::shared_ptr<const detail::MaterializedProgram>* isolated_materialization = nullptr);

    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class detail::CatalogRuntimeAccess;
};

}  // namespace neograph::program
