#pragma once

#include <neograph/graph/engine.h>
#include <neograph/program/catalog.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::program::detail {

/** Complete owner for one linked Core generation. Engine is declared last so
 * it is destroyed before the registry and factory capture graph it depends on.
 */
struct PinnedCoreGeneration {
    RegistrySnapshot                            registry_snapshot;
    std::shared_ptr<const graph::GraphRegistry> runtime_registry;
    std::string                                 core_name;
    std::string                                 compiled_plan_identity;
    std::shared_ptr<Provider>                    provider;
    std::shared_ptr<graph::GraphEngine>         engine;
};
struct AdmittedNativeControlBinding {
    std::uint32_t            import_slot = 0;
    CapabilityBindingReceipt receipt;
    NativeControlBinding     binding;
};

struct MaterializedProgram {
    ProgramBundle                               bundle;
    ProgramVersion                              version;
    std::shared_ptr<const PinnedCoreGeneration> root;
    std::map<std::uint32_t, AdmittedNativeControlBinding> native_bindings;
};

class CatalogRuntimeAccess {
public:
    static std::shared_ptr<const MaterializedProgram> pin(const ProgramCatalog& catalog,
                                                          const ProgramVersion& version);
    static std::shared_ptr<const MaterializedProgram> pin_with_binding(
        ProgramCatalog& catalog, std::string_view owner_scope, std::string_view version_id,
        CatalogCapabilityBinding binding);
    static std::shared_ptr<const MaterializedProgram> pin_recorded(
        ProgramCatalog& catalog, const ProgramVersion& version,
        const RecordedBindingSet& requested, const std::vector<ProgramEvent>& source_events);
    /**
     * Read the immutable admitted version without materializing a live
     * capability binding. Recorded replay uses this before pin_recorded().
     */
    static std::optional<ProgramVersion> load_admitted_version(
        const ProgramCatalog& catalog, std::string_view owner_scope, std::string_view version_id);

    /**
     * Throws ProgramBlockingCallRefused when a synchronous call cannot wait for this catalog here, as the catalog's
     * own synchronous operations do (see ProgramCatalog::Impl::require_blocking_allowed). For a caller that runs the
     * catalog's awaitable operations to completion itself.
     */
    static void require_blocking_allowed(const ProgramCatalog& catalog, std::string_view operation);

    // The same operations, awaited (arguments by value). They are the bodies of the four above.
    static asio::awaitable<std::shared_ptr<const MaterializedProgram>> pin_async(
        const ProgramCatalog& catalog, ProgramVersion version);
    static asio::awaitable<std::shared_ptr<const MaterializedProgram>> pin_with_binding_async(
        ProgramCatalog& catalog, std::string owner_scope, std::string version_id,
        CatalogCapabilityBinding binding);
    static asio::awaitable<std::shared_ptr<const MaterializedProgram>> pin_recorded_async(
        ProgramCatalog& catalog, ProgramVersion version, RecordedBindingSet requested,
        std::vector<ProgramEvent> source_events);
    static asio::awaitable<std::optional<ProgramVersion>> load_admitted_version_async(
        const ProgramCatalog& catalog, std::string owner_scope, std::string version_id);

private:
    // The body of pin_recorded and pin_recorded_async. A RecordedBindingSet cannot be copied and only its fingerprint
    // is compared, so the body takes that.
    static asio::awaitable<std::shared_ptr<const MaterializedProgram>> pin_recorded_core(
        ProgramCatalog& catalog, ProgramVersion version, std::string requested_fingerprint,
        std::vector<ProgramEvent> source_events);
};

}  // namespace neograph::program::detail
