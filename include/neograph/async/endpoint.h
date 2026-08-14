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

#include <algorithm>
#include <charconv>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>

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
        std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        if (scheme != "http" && scheme != "https") {
            throw std::invalid_argument("unsupported HTTP endpoint scheme");
        }
        out.tls = scheme == "https";
        rest = rest.substr(scheme_end + 3);
    }

    auto path_start = rest.find_first_of("/?#");
    std::string authority;
    if (path_start != std::string::npos) {
        authority = rest.substr(0, path_start);
        if (rest[path_start] != '/') {
            throw std::invalid_argument(
                "HTTP endpoint base URL must not contain a query or fragment");
        }
        out.prefix = rest.substr(path_start);
        if (out.prefix.find('#') != std::string::npos) {
            throw std::invalid_argument(
                "HTTP endpoint base URL must not contain a fragment");
        }
    } else {
        authority = rest;
    }

    if (authority.empty() || authority.find('@') != std::string::npos) {
        throw std::invalid_argument("invalid HTTP endpoint authority");
    }
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) {
            throw std::invalid_argument("invalid bracketed HTTP endpoint host");
        }
        out.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':' || close + 2 == authority.size()) {
                throw std::invalid_argument("invalid HTTP endpoint port");
            }
            out.port = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.find(':');
        if (colon != std::string::npos) {
            if (authority.find(':', colon + 1) != std::string::npos) {
                throw std::invalid_argument(
                    "IPv6 HTTP endpoint hosts must use brackets");
            }
            out.host = authority.substr(0, colon);
            out.port = authority.substr(colon + 1);
        } else {
            out.host = authority;
        }
    }
    if (out.host.empty()) {
        throw std::invalid_argument("HTTP endpoint host is empty");
    }
    if (out.port.empty()) out.port = out.tls ? "443" : "80";
    unsigned int numeric_port = 0;
    const auto [port_end, port_error] = std::from_chars(
        out.port.data(), out.port.data() + out.port.size(), numeric_port);
    if (port_error != std::errc{} ||
        port_end != out.port.data() + out.port.size() ||
        numeric_port == 0 || numeric_port > 65535) {
        throw std::invalid_argument("invalid HTTP endpoint port");
    }
    out.port = std::to_string(numeric_port);
    return out;
}

inline bool has_explicit_http_scheme(std::string_view url) {
    const auto separator = url.find("://");
    if (separator == std::string_view::npos) return false;
    std::string scheme(url.substr(0, separator));
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return scheme == "http" || scheme == "https";
}

inline bool is_loopback_host(std::string_view host) {
    std::string lower(host);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (!lower.empty() && lower.back() == '.') lower.pop_back();
    if (lower == "::1") return true;

    unsigned int octets[4]{};
    std::size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        const auto end = lower.find('.', start);
        const auto token_end = end == std::string::npos ? lower.size() : end;
        if (token_end == start) return false;
        const auto [parsed, error] = std::from_chars(
            lower.data() + start, lower.data() + token_end, octets[i]);
        if (error != std::errc{} || parsed != lower.data() + token_end ||
            octets[i] > 255) {
            return false;
        }
        if (i < 3 && end == std::string::npos) return false;
        if (i == 3 && end != std::string::npos) return false;
        start = token_end + 1;
    }
    return octets[0] == 127;
}

inline AsyncEndpoint validate_credential_endpoint(
    const std::string& base_url,
    bool credentialed,
    bool allow_insecure_loopback) {
    if (credentialed && !has_explicit_http_scheme(base_url)) {
        throw std::invalid_argument(
            "credentialed provider endpoint requires an explicit http:// or https:// scheme");
    }
    auto endpoint = split_async_endpoint(base_url);
    if (credentialed && !endpoint.tls &&
        !(allow_insecure_loopback && is_loopback_host(endpoint.host))) {
        throw std::invalid_argument(
            "credentialed provider endpoint requires TLS; plaintext is allowed only for explicit loopback development");
    }
    return endpoint;
}

} // namespace neograph::async
