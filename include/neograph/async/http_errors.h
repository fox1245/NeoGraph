/**
 * @file http_errors.h
 * @brief Error taxonomy for async HTTP — retryable vs permanent.
 *
 * Stage 3 / Semester 1.3 — classifies the error_codes that surface
 * from asio::system_error (and OpenSSL) during async HTTP work into
 * a small enum, plus a single `is_retryable()` predicate. Higher
 * layers (Provider, MCPClient, future Engine retry) use this instead
 * of each reinventing "which errno means try again".
 *
 * Scope bounds:
 *   - This header defines the taxonomy and classifiers. It does
 *     *not* change async_post / ConnPool behavior — those still
 *     throw asio::system_error / std::runtime_error verbatim, so
 *     callers that already catch std::exception are unaffected.
 *     Wrapping internal throws in HttpError is deferred to
 *     Semester 2 when Provider layers want structured retry.
 *   - The classifier is a pure, header-friendly function: safe to
 *     call from any thread, any context, under error unwinding.
 */
#pragma once

#include <asio/error.hpp>
#include <asio/ssl/error.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>

namespace neograph::async {

enum class HttpErrorKind {
    // ── Retryable ─────────────────────────────────────────────────
    ConnectRefused,       ///< TCP RST on connect — server down / booting.
    ConnectTimeout,       ///< connect() didn't ACK within budget.
    DnsTemporary,         ///< EAI_AGAIN equivalent — try again later.
    ReadTimeout,          ///< no bytes within deadline (1.5 territory).
    WriteTimeout,         ///< send buffer full too long (1.5 territory).
    PeerReset,            ///< ECONNRESET / EPIPE mid-exchange.
    PeerEofEarly,         ///< EOF before the response was complete.
    TlsHandshakeReset,    ///< Any SSL error except verify failure —
                          ///< overloaded server or flaky link, worth retry.
    ServerError,          ///< HTTP 500-599 from classify_http_status.
    TooManyRequests,      ///< HTTP 429 — caller should honor Retry-After.

    // ── Not retryable ─────────────────────────────────────────────
    DnsPermanent,         ///< NXDOMAIN.
    TlsVerifyFailed,      ///< Cert chain / hostname mismatch. Never retry.
    ProtocolError,        ///< Malformed HTTP, unsupported encoding.
    RequestTooLarge,      ///< HTTP 413.
    PayloadInvalid,       ///< HTTP 4xx other than 429 / 413.

    // ── Catch-all ─────────────────────────────────────────────────
    Unknown,              ///< Code we haven't classified. Default = don't
                          ///< retry (safer than unbounded retries on a
                          ///< misbehaving wire).
};

/// True if transparent retry on a different connection may succeed.
/// Pure function — safe to call during exception unwinding.
constexpr bool is_retryable(HttpErrorKind k) noexcept {
    switch (k) {
        case HttpErrorKind::ConnectRefused:
        case HttpErrorKind::ConnectTimeout:
        case HttpErrorKind::DnsTemporary:
        case HttpErrorKind::ReadTimeout:
        case HttpErrorKind::WriteTimeout:
        case HttpErrorKind::PeerReset:
        case HttpErrorKind::PeerEofEarly:
        case HttpErrorKind::TlsHandshakeReset:
        case HttpErrorKind::ServerError:
        case HttpErrorKind::TooManyRequests:
            return true;
        case HttpErrorKind::DnsPermanent:
        case HttpErrorKind::TlsVerifyFailed:
        case HttpErrorKind::ProtocolError:
        case HttpErrorKind::RequestTooLarge:
        case HttpErrorKind::PayloadInvalid:
        case HttpErrorKind::Unknown:
            return false;
    }
    return false;
}

/// Classify an asio error_code — the kind returned inside
/// asio::system_error::code() from any connect/read/write/handshake
/// op. Unknown category or unknown value returns HttpErrorKind::Unknown.
inline HttpErrorKind classify_asio_error(const asio::error_code& ec) noexcept {
    if (!ec) return HttpErrorKind::Unknown;

    const auto& cat = ec.category();

    // asio::error::get_ssl_category() — OpenSSL error_codes.
    // Short-read (peer closed the TLS stream without close_notify)
    // is shaped differently in OpenSSL 1.x vs 3.x; asio normalizes
    // it as asio::ssl::error::stream_truncated, so compare against
    // the enum instead of a raw SSL_R_* reason.
    if (ec == asio::ssl::error::stream_truncated) {
        return HttpErrorKind::PeerEofEarly;
    }
    if (cat == asio::error::get_ssl_category()) {
        const int reason = ERR_GET_REASON(ec.value());
        if (reason == SSL_R_CERTIFICATE_VERIFY_FAILED) {
            return HttpErrorKind::TlsVerifyFailed;
        }
        return HttpErrorKind::TlsHandshakeReset;
    }

    // Name resolution errors.
    if (cat == asio::error::get_netdb_category()) {
        switch (ec.value()) {
            case asio::error::host_not_found:
                return HttpErrorKind::DnsPermanent;
            case asio::error::host_not_found_try_again:
                return HttpErrorKind::DnsTemporary;
            default:
                return HttpErrorKind::DnsPermanent;
        }
    }

    // Misc category — asio-specific codes (e.g. eof).
    if (cat == asio::error::get_misc_category()) {
        if (ec.value() == asio::error::eof) {
            return HttpErrorKind::PeerEofEarly;
        }
        return HttpErrorKind::Unknown;
    }

    // System / socket errno category. Covers connect/read/write.
    // Compare against asio::error::* — values are ECONNREFUSED etc.
    switch (ec.value()) {
        case ECONNREFUSED:      return HttpErrorKind::ConnectRefused;
        case ETIMEDOUT:         return HttpErrorKind::ConnectTimeout;
        case ECONNRESET:        return HttpErrorKind::PeerReset;
        case EPIPE:             return HttpErrorKind::PeerReset;
        case ENOTCONN:          return HttpErrorKind::PeerReset;
        default:                return HttpErrorKind::Unknown;
    }
}

/// Classify an HTTP status code on its own. 2xx/3xx don't surface
/// as errors through this path (success, or a redirect the client
/// transparently follows), so they map to Unknown.
constexpr HttpErrorKind classify_http_status(int status) noexcept {
    if (status >= 500 && status < 600) return HttpErrorKind::ServerError;
    if (status == 429)                 return HttpErrorKind::TooManyRequests;
    if (status == 413)                 return HttpErrorKind::RequestTooLarge;
    if (status >= 400 && status < 500) return HttpErrorKind::PayloadInvalid;
    return HttpErrorKind::Unknown;
}

/// Structured exception. Optional — today's async_post / ConnPool do
/// not throw this, but higher layers can construct one from a caught
/// asio::system_error plus classify_asio_error().
struct HttpError : std::runtime_error {
    HttpErrorKind kind        = HttpErrorKind::Unknown;
    int           http_status = 0;           ///< When kind is 4xx/5xx-derived.
    std::string   retry_after;               ///< Verbatim `Retry-After` header.

    HttpError(HttpErrorKind k, std::string msg)
        : std::runtime_error(std::move(msg)), kind(k) {}
};

}  // namespace neograph::async
