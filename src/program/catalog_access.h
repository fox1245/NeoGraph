#pragma once

#include <neograph/graph/engine.h>
#include <neograph/program/catalog.h>

#include <memory>
#include <string>

namespace neograph::program::detail {

/** Complete owner for one linked Core generation. Engine is declared last so
 * it is destroyed before the registry and factory capture graph it depends on.
 */
struct PinnedCoreGeneration {
    RegistrySnapshot                            registry_snapshot;
    std::shared_ptr<const graph::GraphRegistry> runtime_registry;
    std::string                                 core_name;
    std::string                                 compiled_plan_identity;
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
};

}  // namespace neograph::program::detail
