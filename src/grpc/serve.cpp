// The built-in gRPC servers' common loop: listen, health, stop.
//
// Built only with -DNEOGRAPH_BUILD_GRPC=ON.

#ifdef NEOGRAPH_HAVE_GRPC

#include "serve.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/server_builder.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>

namespace neograph::grpc::detail {
namespace {

// gRPC adds its default health service to the servers built while a
// process-wide flag is on. It is turned on only around this server's start
// and then set back, so the host's own servers keep the host's choice; the
// lock keeps two built-in servers that start at once from interleaving.
std::unique_ptr<::grpc::Server> build_with_health(::grpc::ServerBuilder& builder) {
    static std::mutex flag;
    std::lock_guard lock(flag);
    const bool previous = ::grpc::DefaultHealthCheckServiceEnabled();
    ::grpc::EnableDefaultHealthCheckService(true);
    std::unique_ptr<::grpc::Server> server;
    try {
        server = builder.BuildAndStart();
    } catch (...) {
        ::grpc::EnableDefaultHealthCheckService(previous);
        throw;
    }
    ::grpc::EnableDefaultHealthCheckService(previous);
    return server;
}

bool starts_with(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

// A dotted-quad IPv4 address of 127.0.0.0/8.
bool is_ipv4_loopback(const std::string& host) {
    int parts = 0;
    int first = -1;
    std::size_t at = 0;
    while (at <= host.size()) {
        const auto end = std::min(host.find('.', at), host.size());
        if (end == at || end - at > 3) return false;
        int value = 0;
        for (auto i = at; i < end; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(host[i]))) return false;
            value = value * 10 + (host[i] - '0');
        }
        if (value > 255) return false;
        if (parts == 0) first = value;
        ++parts;
        if (end == host.size()) break;
        at = end + 1;
    }
    return parts == 4 && first == 127;
}

// One `host[:port]` entry of a target, the host in brackets when it is an
// IPv6 address with a port.
bool is_loopback_entry(const std::string& entry) {
    std::string host;
    if (!entry.empty() && entry.front() == '[') {
        const auto close = entry.find(']');
        if (close == std::string::npos) return false;
        host = entry.substr(1, close - 1);
    } else if (std::count(entry.begin(), entry.end(), ':') == 1) {
        host = entry.substr(0, entry.find(':'));
    } else {
        host = entry;  // a name or address without a port; a bare IPv6 address
    }
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return host == "localhost" || host == "::1" || is_ipv4_loopback(host);
}

}  // namespace

bool is_loopback_target(const std::string& address) {
    if (starts_with(address, "unix:") || starts_with(address, "unix-abstract:")) return true;
    std::string target = address;
    for (const char* scheme : {"dns:", "ipv4:", "ipv6:"}) {
        if (!starts_with(target, scheme)) continue;
        target.erase(0, std::string(scheme).size());
        if (std::string(scheme) == "dns:" && starts_with(target, "//")) {
            // dns://authority/host:port; the authority is a resolver, not the listener.
            const auto slash = target.find('/', 2);
            if (slash == std::string::npos) return false;
            target.erase(0, slash + 1);
        }
        break;
    }
    std::size_t at = 0;
    while (true) {
        const auto comma = std::min(target.find(',', at), target.size());
        if (!is_loopback_entry(target.substr(at, comma - at))) return false;
        if (comma == target.size()) return true;
        at = comma + 1;
    }
}

void serve(const std::string& address, ::grpc::Service& service, const char* service_name,
           const ServeOptions& options, const char* what) {
    if (options.shutdown_grace < std::chrono::milliseconds::zero())
        throw std::invalid_argument("ServeOptions::shutdown_grace must not be negative");
    if (options.max_receive_message_bytes > static_cast<std::size_t>(INT_MAX))
        throw std::invalid_argument("ServeOptions::max_receive_message_bytes must not exceed INT_MAX");
    if (options.max_send_message_bytes > static_cast<std::size_t>(INT_MAX))
        throw std::invalid_argument("ServeOptions::max_send_message_bytes must not exceed INT_MAX");

    ::grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort(
        address, options.credentials ? options.credentials : ::grpc::InsecureServerCredentials(),
        &port);
    if (options.max_receive_message_bytes != 0)
        builder.SetMaxReceiveMessageSize(static_cast<int>(options.max_receive_message_bytes));
    if (options.max_send_message_bytes != 0)
        builder.SetMaxSendMessageSize(static_cast<int>(options.max_send_message_bytes));
    builder.RegisterService(&service);
    auto server = build_with_health(builder);
    if (!server) throw std::runtime_error(std::string("failed to start ") + what + " on " + address);
    if (!options.credentials && !is_loopback_target(address)) {
        // FILE* stderr: see graph_executor.cpp.
        std::fprintf(stderr,
                     "[neograph] warning: the gRPC server for %s listens on %s with insecure "
                     "credentials: its traffic is not encrypted and any peer that can reach "
                     "the address can call it. Set ServeOptions::credentials to serve TLS, "
                     "or listen on a loopback address.\n",
                     service_name, address.c_str());
        std::fflush(stderr);
    }
    auto* health = server->GetHealthCheckService();
    if (health) health->SetServingStatus(service_name, true);

    const auto stop = [&] {
        if (health) {
            health->SetServingStatus(false);
            health->Shutdown();  // and no later change
        }
        server->Shutdown(std::chrono::system_clock::now() + options.shutdown_grace);
        server->Wait();
    };
    if (options.on_serving) {
        try {
            options.on_serving(port);
        } catch (...) {
            stop();
            throw;
        }
    }
    if (!options.stop.stop_possible()) {
        server->Wait();
        return;
    }
    {
        // A stop_callback and a plain condition variable rather than the
        // stop_token overload of condition_variable_any, which older libc++
        // releases do not ship.
        std::mutex mutex;
        std::condition_variable requested;
        bool stopping = false;
        std::stop_callback on_stop(options.stop, [&] {
            {
                std::lock_guard notify_lock(mutex);
                stopping = true;
            }
            requested.notify_all();
        });
        std::unique_lock lock(mutex);
        requested.wait(lock, [&] { return stopping; });
    }
    stop();
}

}  // namespace neograph::grpc::detail

#endif  // NEOGRAPH_HAVE_GRPC
