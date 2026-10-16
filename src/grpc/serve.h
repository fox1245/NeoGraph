// Internal: run one built-in gRPC server (see <neograph/grpc/serve_options.h>).

#pragma once

#ifdef NEOGRAPH_HAVE_GRPC

#include <neograph/grpc/serve_options.h>

#include <string>

namespace grpc {
class Service;
}

namespace neograph::grpc::detail {

/// Serve `service` (whose full name is `service_name`) on `address` with
/// insecure credentials and the standard health service, until
/// `options.stop` is requested. Throws std::runtime_error naming `what` when
/// the server cannot start.
void serve(const std::string& address, ::grpc::Service& service, const char* service_name,
           const ServeOptions& options, const char* what);

}  // namespace neograph::grpc::detail

#endif  // NEOGRAPH_HAVE_GRPC
