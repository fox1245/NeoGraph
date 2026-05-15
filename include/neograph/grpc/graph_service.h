// neograph::grpc — expose a compiled GraphEngine over gRPC.
//
// Opt-in component (NEOGRAPH_BUILD_GRPC=ON, default OFF). The whole
// header is behind NEOGRAPH_HAVE_GRPC so a default build that includes
// <neograph/neograph.h> umbrella never pulls grpc++ headers.
//
// This is a NeoGraph-native gRPC API, independent of the still-
// unratified MCP-over-gRPC transport (modelcontextprotocol #966).

#pragma once

#ifdef NEOGRAPH_HAVE_GRPC

#include <memory>
#include <string>

#include <neograph/graph/engine.h>
#include <neograph/graph/types.h>

namespace neograph::grpc {

class GraphServiceImpl;

/// Build a GraphService bound to a NodeContext. When
/// `default_graph_json` is non-empty, a `RunGraphRequest` that omits
/// `graph_def_json` reuses a precompiled engine for that default.
/// Per-distinct-graph engines are compiled lazily and cached
/// (hash-keyed, the same multi-tenant pattern as the cookbook).
std::unique_ptr<GraphServiceImpl> make_graph_service(
    neograph::graph::NodeContext ctx,
    std::string default_graph_json = "");

/// Convenience: build + run a blocking gRPC server on `address`
/// (e.g. "0.0.0.0:50051") with insecure credentials until the
/// process is signalled. For TLS / auth wire your own
/// `grpc::ServerBuilder` against `make_graph_service()`.
void run_server(const std::string& address,
                neograph::graph::NodeContext ctx,
                std::string default_graph_json = "");

}  // namespace neograph::grpc

#endif  // NEOGRAPH_HAVE_GRPC
