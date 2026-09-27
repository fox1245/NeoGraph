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

#include <cstddef>
#include <neograph.grpc.pb.h>
#include <memory>
#include <string>

#include <neograph/graph/engine.h>
#include <neograph/graph/types.h>

namespace neograph::grpc {

/// Build a concrete GraphService as a complete generated service base, so
/// embedders can register it with grpc::ServerBuilder.
/// A nonempty `default_graph_json` selects the default graph. An optional
/// `default_engine` must be compiled from those same bytes and is reused
/// instead of compiling another engine (for example, when a host configures
/// Store and ToolGate before accepting requests). Other graph definitions
/// compile lazily and are cached. `max_inflight_runs` is the concurrent RPC
/// admission cap; zero preserves the existing unbounded default.
std::unique_ptr<neograph::v1::GraphService::Service> make_graph_service(
    neograph::graph::NodeContext ctx,
    std::string default_graph_json = "",
    std::shared_ptr<neograph::graph::GraphEngine> default_engine = {},
    std::size_t max_inflight_runs = 0);

/// Convenience: build + run a blocking gRPC server on `address`
/// (e.g. "0.0.0.0:50051") with insecure credentials until the
/// process is signalled. For TLS / auth wire your own
/// `grpc::ServerBuilder` against `make_graph_service()`.
void run_server(const std::string& address,
                neograph::graph::NodeContext ctx,
                std::string default_graph_json = "");

}  // namespace neograph::grpc

#endif  // NEOGRAPH_HAVE_GRPC
