#pragma once

// The Host and Origin checks of NeoGraph's HTTP servers (the MCP Streamable
// HTTP server and the A2A server), kept in one place so that they cannot
// drift apart.
//
// A page on another origin reaches a server bound to a loopback address when
// its host name is re-pointed at this machine (DNS rebinding): the browser
// then sends that name in `Host`, and its own origin in `Origin` when it
// sends one. The guard decides from those two header values and from the
// address the server is bound to. Turning a verdict into a response is the
// caller's: each protocol has its own error shape.
//
// Header-only and internal: it does not include httplib, and it is not
// installed.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace neograph::detail {

class HttpRequestGuard {
public:
    enum class Verdict {
        Allowed,
        HostMissingOrInvalid,  ///< no Host, or not `uri-host [":" port]`: answer 400
        HostNotAllowed,        ///< answer 403
        OriginNotAllowed,      ///< answer 403
    };

    /// @param server  names the server in the line written to stderr when
    ///                its own loopback origin is admitted, e.g. "A2A server".
    explicit HttpRequestGuard(std::string server) : server_(std::move(server)) {}

    HttpRequestGuard(const HttpRequestGuard&)            = delete;
    HttpRequestGuard& operator=(const HttpRequestGuard&) = delete;

    /// Exact allowed Origin values.
    std::vector<std::string> allowed_origins;
    /// Host names the server answers, without a port; `*` admits any.
    std::vector<std::string> allowed_hosts;

    /// Record what the server is bound to. Set it before the listener starts
    /// accepting; the checks below read it without a lock.
    void bind(std::string host, int port) {
        bind_host_  = std::move(host);
        bound_port_ = port;
    }

    static bool is_loopback_bind(const std::string& host) {
        return host == "127.0.0.1" || host == "::1" || host == "localhost";
    }

    /// The host of a Host header value, `uri-host [":" port]` (RFC 9110
    /// section 7.2), in lowercase and without its port; nullopt when the
    /// value is not of that form. An IPv6 address keeps its brackets. The
    /// HTTP layer has already answered 400 to a repeated Host and to
    /// characters a Host cannot hold.
    static std::optional<std::string> host_name_of(const std::string& value) {
        std::size_t end = 0;
        if (!value.empty() && value.front() == '[') {
            end = value.find(']');
            if (end == std::string::npos) return std::nullopt;
            ++end;
        } else {
            end = std::min(value.find(':'), value.size());
        }
        if (end == 0 || value.compare(0, end, "[]") == 0) return std::nullopt;
        if (end < value.size()) {
            if (value[end] != ':') return std::nullopt;
            for (std::size_t i = end + 1; i < value.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(value[i]))) return std::nullopt;
            }
        }
        return lower(value.substr(0, end));
    }

    /// Throws std::invalid_argument(@p message) for an `allowed_hosts` entry
    /// that is not a host name without a port, an IPv4 address, a bracketed
    /// IPv6 address or `*`.
    static void require_host_names(const std::vector<std::string>& names, const char* message) {
        for (const auto& name : names) {
            if (name != "*" && host_name_of(name) != lower(name)) {
                throw std::invalid_argument(message);
            }
        }
    }

    /// A page whose name resolves to this machine sends that name in Host,
    /// with or without an Origin. A loopback bind answers the loopback names
    /// and the listed ones; a remote bind checks Host only when
    /// allowed_hosts lists names. @p host_header is the Host header's value,
    /// nullopt when the request has none.
    Verdict check_host(const std::optional<std::string>& host_header) const {
        const bool loopback = is_loopback_bind(bind_host_);
        if ((!loopback && allowed_hosts.empty()) ||
            std::find(allowed_hosts.begin(), allowed_hosts.end(), "*") != allowed_hosts.end()) {
            return Verdict::Allowed;
        }
        const auto name = host_header ? host_name_of(*host_header) : std::nullopt;
        if (!name) return Verdict::HostMissingOrInvalid;
        if (loopback && (*name == "localhost" || *name == "127.0.0.1" || *name == "[::1]")) {
            return Verdict::Allowed;
        }
        for (const auto& entry : allowed_hosts) {
            if (lower(entry) == *name) return Verdict::Allowed;
        }
        return Verdict::HostNotAllowed;
    }

    /// An Origin is refused unless listed, except that a loopback bind also
    /// admits its own loopback origin. @p origin is the Origin header's
    /// value, nullopt when the request has none.
    Verdict check_origin(const std::optional<std::string>& origin) const {
        if (!origin) return Verdict::Allowed;
        if (std::find(allowed_origins.begin(), allowed_origins.end(), *origin) !=
            allowed_origins.end()) {
            return Verdict::Allowed;
        }
        if (own_loopback_origin(*origin)) {
            report_admitted_origin(*origin);
            return Verdict::Allowed;
        }
        return Verdict::OriginNotAllowed;
    }

private:
    static std::string lower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    // A browser sends the origin of the page it runs. On a loopback bind, a
    // page with this server's own loopback address and port is served by this
    // machine, so its origin needs no listing. Browsers send it in this
    // form: lowercase, no path, no default port. A DNS-rebinding page sends
    // its own origin and is refused.
    bool own_loopback_origin(const std::string& origin) const {
        if (!is_loopback_bind(bind_host_)) return false;
        const auto port = bound_port_ == 80 ? std::string() : ":" + std::to_string(bound_port_);
        for (const char* host : {"localhost", "127.0.0.1", "[::1]"}) {
            if (origin == "http://" + std::string(host) + port) return true;
        }
        return false;
    }

    void report_admitted_origin(const std::string& origin) const {
        {
            std::lock_guard lock(admitted_mutex_);
            if (std::find(admitted_.begin(), admitted_.end(), origin) != admitted_.end()) return;
            admitted_.push_back(origin);
        }
        // FILE* stderr: see graph_executor.cpp.
        std::fprintf(stderr,
                     "[neograph] warning: the %s admits Origin %s, its own loopback "
                     "origin, although allowed_origins does not list it. A page on another "
                     "origin still has to be listed.\n",
                     server_.c_str(), origin.c_str());
        std::fflush(stderr);
    }

    std::string server_;
    std::string bind_host_;
    int         bound_port_ = 0;
    /// Own loopback origins admitted without a listing, each reported once.
    mutable std::mutex               admitted_mutex_;
    mutable std::vector<std::string> admitted_;
};

}  // namespace neograph::detail
