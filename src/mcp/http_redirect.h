// Which redirects the MCP Streamable HTTP client follows: a 307 or 308 on the
// endpoint's own origin only, since the request carries the session and may
// carry credentials (#449).
#pragma once

#include <neograph/async/endpoint.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>

namespace neograph::mcp::detail {

/// The request target that the Location @p location of a 307 or 308 names on
/// @p endpoint's origin (scheme, host and port, RFC 6454), resolved against
/// the current request target @p from. Nothing when it names another origin,
/// including the same host over another scheme, or cannot be read.
inline std::optional<std::string> same_origin_redirect_target(
    const async::AsyncEndpoint& endpoint, const std::string& location,
    const std::string& from) {
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    std::string to = location.substr(0, location.find('#'));
    if (to.empty() || to.find_first_of(" \t\r\n") != std::string::npos) return std::nullopt;
    if (async::has_explicit_http_scheme(to)) {
        // split_async_endpoint fills in the default port and refuses userinfo.
        const auto query = to.find('?');
        async::AsyncEndpoint origin;
        try {
            origin = async::split_async_endpoint(to.substr(0, query));
        } catch (const std::invalid_argument&) {
            return std::nullopt;
        }
        if (origin.tls != endpoint.tls || origin.port != endpoint.port ||
            lower(origin.host) != lower(endpoint.host))
            return std::nullopt;
        std::string next = origin.prefix.empty() ? std::string("/") : origin.prefix;
        if (query != std::string::npos) next += to.substr(query);
        return next;
    }
    // A network-path reference names an authority, and any other scheme is
    // another origin.
    if (to.rfind("//", 0) == 0 || to.find("://") != std::string::npos) return std::nullopt;
    if (to.front() == '/') return to;
    const std::string path = from.substr(0, from.find('?'));
    if (to.front() == '?') return path + to;
    // A relative path replaces the last segment of the current path.
    return path.substr(0, path.rfind('/') + 1) + to;
}

}  // namespace neograph::mcp::detail
