#pragma once

#include <neograph/graph/engine.h>
#include <neograph/program/catalog.h>

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

struct MaterializedProgram {
    ProgramBundle                               bundle;
    ProgramVersion                              version;
    std::shared_ptr<const PinnedCoreGeneration> root;
};

class CatalogRuntimeAccess {
public:
    static std::shared_ptr<const MaterializedProgram> pin(const ProgramCatalog& catalog,
                                                          const ProgramVersion& version);
    static std::shared_ptr<const MaterializedProgram> pin_with_binding(
        ProgramCatalog& catalog, std::string_view owner_scope, std::string_view version_id,
        CatalogCapabilityBinding binding);
    /**
     * Read the immutable admitted version without materializing a live
     * capability binding. Recorded replay uses this before pin_with_binding().
     */
    static std::optional<ProgramVersion> load_admitted_version(
        const ProgramCatalog& catalog, std::string_view owner_scope, std::string_view version_id);
};

}  // namespace neograph::program::detail
