// Internal: run one built-in gRPC server (see <neograph/grpc/serve_options.h>).

#pragma once

#ifdef NEOGRAPH_HAVE_GRPC

#include <neograph/api.h>  // NEOGRAPH_API
#include <neograph/grpc/serve_options.h>

#include <string>

namespace grpc {
class Service;
}

namespace neograph::grpc::detail {

/// Serve `service` (whose full name is `service_name`) on `address` with
/// `options.credentials` (insecure ones when null) and the standard health
/// service, until `options.stop` is requested. Throws std::invalid_argument
/// for options out of range and std::runtime_error naming `what` when the
/// server cannot start.
void serve(const std::string& address, ::grpc::Service& service, const char* service_name,
           const ServeOptions& options, const char* what);

/// Whether a server listening on the gRPC target `address` can be reached
/// only from this machine: a `unix:` or `unix-abstract:` socket, or a host
/// that is `localhost`, an IPv4 address of 127.0.0.0/8 or `::1`, with or
/// without a port and a `dns:`, `ipv4:` or `ipv6:` scheme. A list
/// (`ipv4:a:1,b:2`) is loopback when every entry is. Anything else is not,
/// among it a wildcard (`0.0.0.0`, `[::]`, a bare `:port`) and a form this
/// function does not know.
NEOGRAPH_API bool is_loopback_target(const std::string& address);

}  // namespace neograph::grpc::detail

#endif  // NEOGRAPH_HAVE_GRPC
