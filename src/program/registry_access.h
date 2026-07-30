#pragma once

#include <neograph/graph/compiler.h>
#include <neograph/graph/node.h>
#include <neograph/graph/validator.h>
#include <neograph/program/registry.h>

namespace neograph::program::detail {

class RegistrySnapshotAccess {
public:
    static const ExecutableManifest& require_manifest(const RegistrySnapshot& snapshot,
                                                      ExecutableKind          kind,
                                                      std::string_view        name);
    static graph::TopologySpec       parse_local(const RegistrySnapshot& snapshot,
                                                 const json&             definition);
    static graph::ValidationReport   validate_local(const RegistrySnapshot&    snapshot,
                                                    const graph::TopologySpec& topology);
    static graph::CompiledGraph      link_local(const RegistrySnapshot&   snapshot,
                                                graph::TopologySpec       topology,
                                                const graph::NodeContext& context);
};

}  // namespace neograph::program::detail
