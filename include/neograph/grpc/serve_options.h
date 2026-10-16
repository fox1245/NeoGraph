// neograph::grpc — how a built-in server runs.
//
// run_server, run_tool_server and run_checkpoint_server each start one
// insecure gRPC server for one NeoGraph service. Every built-in server also
// answers the standard health service, grpc.health.v1.Health: SERVING for the
// server ("") and for its service ("neograph.v1.GraphService",
// "neograph.v1.ToolService" or "neograph.v1.CheckpointService") while it
// accepts calls, NOT_SERVING once it begins to stop. The overloads that take
// ServeOptions can also be stopped and report the bound port. For TLS,
// authentication, message size limits or more services on one port, register
// make_graph_service(), make_tool_service() or make_checkpoint_service() with
// your own grpc::ServerBuilder instead.
//
// Server reflection is the host's choice: link gRPC::grpc++_reflection and
// call grpc::reflection::InitProtoReflectionServerBuilderPlugin() once before
// the servers start. gRPC then adds it to every server of the process, the
// built-in ones included.
//
// Opt-in (NEOGRAPH_BUILD_GRPC). Whole header behind NEOGRAPH_HAVE_GRPC.

#pragma once

#ifdef NEOGRAPH_HAVE_GRPC

#include <chrono>
#include <functional>
#include <stop_token>

namespace neograph::grpc {

struct ServeOptions {
    /// When a stop is requested, the server reports NOT_SERVING, stops taking
    /// calls, gives the calls in flight `shutdown_grace` to finish, cancels
    /// the rest (a graph run sees its cancel token) and the call returns. A
    /// default token never stops: the call blocks until the process ends.
    std::stop_token stop;
    /// Non-negative.
    std::chrono::milliseconds shutdown_grace{std::chrono::seconds(5)};
    /// Called once, on the serving thread, with the port the server bound
    /// (the one it chose when the address names port 0) when it is serving.
    /// An exception it throws stops the server and leaves the call.
    std::function<void(int port)> on_serving;
};

}  // namespace neograph::grpc

#endif  // NEOGRAPH_HAVE_GRPC
