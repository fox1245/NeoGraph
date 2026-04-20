/**
 * @file async/endpoint.h
 * @brief URL → (host, port, path-prefix, tls) decomposition for async_post.
 *
 * Stage 3 / Semester 2.8 — hoisted from three near-identical copies that
 * accumulated in openai_provider.cpp, schema_provider.cpp, and
 * mcp/client.cpp during Semester 2.3-2.6. Each provider parses its
 * configured base_url into the shape neograph::async::async_post wants
 * (host/port/path/tls separately, unlike httplib which takes the
 * scheme+authority verbatim).
 *
 * Header-only — the function is small and used at most once per request.
 */
#pragma once

#include <string>

namespace neograph::async {

/// Result of decomposing a URL into the pieces async_post needs.
/// `port` defaults to 443 for https and 80 for http when the URL omits
/// it. `prefix` is the path portion (with leading slash) or empty when
/// the URL has no path component.
struct AsyncEndpoint {
    std::string host;
    std::string port;
    std::string prefix;
    bool        tls = false;
};

/// Parse a base URL like `https://api.openai.com` or
/// `http://localhost:8080/v1`. Returns an AsyncEndpoint with the
/// pieces split out. Tolerant of missing scheme (defaults to http,
/// non-tls) and missing path (prefix empty).
inline AsyncEndpoint split_async_endpoint(const std::string& base_url) {
    AsyncEndpoint out;
    std::string rest = base_url;

    auto scheme_end = rest.find("://");
    if (scheme_end != std::string::npos) {
        std::string scheme = rest.substr(0, scheme_end);
        out.tls = (scheme == "https");
        rest = rest.substr(scheme_end + 3);
    }

    auto path_start = rest.find('/');
    std::string authority;
    if (path_start != std::string::npos) {
        authority = rest.substr(0, path_start);
        out.prefix = rest.substr(path_start);
    } else {
        authority = rest;
    }

    auto colon = authority.find(':');
    if (colon != std::string::npos) {
        out.host = authority.substr(0, colon);
        out.port = authority.substr(colon + 1);
    } else {
        out.host = authority;
        out.port = out.tls ? "443" : "80";
    }
    return out;
}

} // namespace neograph::async
