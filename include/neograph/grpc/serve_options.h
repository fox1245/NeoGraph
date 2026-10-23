// neograph::grpc — how a built-in server runs.
//
// run_server, run_tool_server and run_checkpoint_server each start one gRPC
// server for one NeoGraph service. Without ServeOptions::credentials the
// server is insecure (no TLS, no authentication), the way it always was;
// ServeOptions::credentials takes the caller's server credentials, TLS and
// mutual TLS among them. Every built-in server also answers the standard
// health service, grpc.health.v1.Health: SERVING for the server ("") and for
// its service ("neograph.v1.GraphService", "neograph.v1.ToolService" or
// "neograph.v1.CheckpointService") while it accepts calls, NOT_SERVING once
// it begins to stop. The overloads that take ServeOptions can also be
// stopped and report the bound port. For authentication interceptors or more
// services on one port, register make_graph_service(), make_tool_service()
// or make_checkpoint_service() with your own grpc::ServerBuilder instead.
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
#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>

// Declared, not included: a program that only starts a server with the
// defaults needs no gRPC header beyond the ones of the service it serves.
namespace grpc {
class ServerCredentials;
}

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
    /// The server's credentials, for example `grpc::SslServerCredentials(...)`
    /// (with client certificate verification for mutual TLS). Null, the
    /// default, serves without TLS and without authentication, as before. A
    /// server started that way on an address other than loopback writes one
    /// warning line to stderr; pass `grpc::InsecureServerCredentials()` to say
    /// that this is intended, for example behind a proxy that terminates TLS.
    /// "Loopback" means `localhost`, an IPv4 address of 127.0.0.0/8, `::1` and
    /// a `unix:` or `unix-abstract:` address; `0.0.0.0` and `[::]` are not.
    std::shared_ptr<::grpc::ServerCredentials> credentials;
    /// The largest message the server accepts, in bytes. A larger one is
    /// refused with RESOURCE_EXHAUSTED before the service sees it. Zero, the
    /// default, leaves gRPC's own limit of 4 MiB. Values above INT_MAX throw
    /// std::invalid_argument.
    std::size_t max_receive_message_bytes = 0;
    /// The largest message the server sends, in bytes: a response above it
    /// fails the call with RESOURCE_EXHAUSTED. Zero, the default, leaves
    /// gRPC's own setting, which is no limit. Values above INT_MAX throw
    /// std::invalid_argument.
    std::size_t max_send_message_bytes = 0;
};

}  // namespace neograph::grpc

#endif  // NEOGRAPH_HAVE_GRPC
