// Unit tests for neograph::async error taxonomy (Stage 3 Semester 1.3).
//
// Pure classifier tests — no network. We build asio::error_codes the
// same way asio does internally (via make_error_code on the category
// enums) and feed them through classify_asio_error / classify_http_status.
//
// The classifier is header-only, so linking is just pulling in
// neograph::async for the OpenSSL categories and neograph::core for
// GTest (via the shared test binary).

#include <neograph/async/http_errors.h>

#include <asio/error.hpp>
#include <asio/ssl/error.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <gtest/gtest.h>

#include <cerrno>
#include <system_error>

namespace async = neograph::async;

TEST(HttpErrorTaxonomy, RetryableFlag) {
    EXPECT_TRUE(async::is_retryable(async::HttpErrorKind::ConnectRefused));
    EXPECT_TRUE(async::is_retryable(async::HttpErrorKind::PeerEofEarly));
    EXPECT_TRUE(async::is_retryable(async::HttpErrorKind::ServerError));
    EXPECT_TRUE(async::is_retryable(async::HttpErrorKind::TooManyRequests));
    EXPECT_TRUE(async::is_retryable(async::HttpErrorKind::TlsHandshakeReset));

    EXPECT_FALSE(async::is_retryable(async::HttpErrorKind::DnsPermanent));
    EXPECT_FALSE(async::is_retryable(async::HttpErrorKind::TlsVerifyFailed));
    EXPECT_FALSE(async::is_retryable(async::HttpErrorKind::PayloadInvalid));
    EXPECT_FALSE(async::is_retryable(async::HttpErrorKind::Unknown));
}

TEST(HttpErrorTaxonomy, ClassifyHttpStatus) {
    EXPECT_EQ(async::classify_http_status(500),
              async::HttpErrorKind::ServerError);
    EXPECT_EQ(async::classify_http_status(503),
              async::HttpErrorKind::ServerError);
    EXPECT_EQ(async::classify_http_status(429),
              async::HttpErrorKind::TooManyRequests);
    EXPECT_EQ(async::classify_http_status(413),
              async::HttpErrorKind::RequestTooLarge);
    EXPECT_EQ(async::classify_http_status(400),
              async::HttpErrorKind::PayloadInvalid);
    EXPECT_EQ(async::classify_http_status(401),
              async::HttpErrorKind::PayloadInvalid);

    // Success/redirect don't flow through here.
    EXPECT_EQ(async::classify_http_status(200),
              async::HttpErrorKind::Unknown);
    EXPECT_EQ(async::classify_http_status(301),
              async::HttpErrorKind::Unknown);
}

TEST(HttpErrorTaxonomy, ClassifyAsioSystemErrors) {
    // Build codes the way asio does — make_error_code on the
    // basic_errors enum puts them in the system category with
    // POSIX errno values.
    EXPECT_EQ(
        async::classify_asio_error(
            asio::error::make_error_code(asio::error::connection_refused)),
        async::HttpErrorKind::ConnectRefused);

    EXPECT_EQ(
        async::classify_asio_error(
            asio::error::make_error_code(asio::error::timed_out)),
        async::HttpErrorKind::ConnectTimeout);

    EXPECT_EQ(
        async::classify_asio_error(
            asio::error::make_error_code(asio::error::connection_reset)),
        async::HttpErrorKind::PeerReset);

    EXPECT_EQ(
        async::classify_asio_error(
            asio::error::make_error_code(asio::error::broken_pipe)),
        async::HttpErrorKind::PeerReset);
}

TEST(HttpErrorTaxonomy, ClassifyAsioEof) {
    // misc_category eof — what peer-close mid-read surfaces as.
    auto ec = asio::error::make_error_code(asio::error::eof);
    EXPECT_EQ(async::classify_asio_error(ec),
              async::HttpErrorKind::PeerEofEarly);
}

TEST(HttpErrorTaxonomy, ClassifyDns) {
    EXPECT_EQ(
        async::classify_asio_error(
            asio::error::make_error_code(asio::error::host_not_found)),
        async::HttpErrorKind::DnsPermanent);

    EXPECT_EQ(
        async::classify_asio_error(
            asio::error::make_error_code(
                asio::error::host_not_found_try_again)),
        async::HttpErrorKind::DnsTemporary);
}

TEST(HttpErrorTaxonomy, ClassifySslVerifyFailed) {
    // Build a raw SSL error_code with the verify-failed reason.
    // ERR_PACK(lib, func, reason) is what OpenSSL uses; asio wraps
    // the same integer in its ssl category.
    asio::error_code ec(
        ERR_PACK(ERR_LIB_SSL, 0, SSL_R_CERTIFICATE_VERIFY_FAILED),
        asio::error::get_ssl_category());
    EXPECT_EQ(async::classify_asio_error(ec),
              async::HttpErrorKind::TlsVerifyFailed);
    EXPECT_FALSE(async::is_retryable(async::classify_asio_error(ec)));
}

TEST(HttpErrorTaxonomy, ClassifySslShortRead) {
    // stream_truncated — peer closed mid-TLS-record. Treated as
    // peer-eof-early, retryable.
    auto ec = asio::ssl::error::make_error_code(
        asio::ssl::error::stream_truncated);
    EXPECT_EQ(async::classify_asio_error(ec),
              async::HttpErrorKind::PeerEofEarly);
}

TEST(HttpErrorTaxonomy, ClassifySslGenericHandshake) {
    // A non-verify SSL error → TlsHandshakeReset (retryable).
    // Use an arbitrary reason code that isn't verify-failed or
    // short-read.
    asio::error_code ec(
        ERR_PACK(ERR_LIB_SSL, 0, SSL_R_BAD_PACKET_LENGTH),
        asio::error::get_ssl_category());
    EXPECT_EQ(async::classify_asio_error(ec),
              async::HttpErrorKind::TlsHandshakeReset);
    EXPECT_TRUE(async::is_retryable(async::classify_asio_error(ec)));
}

TEST(HttpErrorTaxonomy, ClassifyUnknownFallsThrough) {
    // No error → Unknown.
    asio::error_code none;
    EXPECT_EQ(async::classify_asio_error(none),
              async::HttpErrorKind::Unknown);

    // An errno we don't map (ENOSYS = 38 on Linux) → Unknown.
    asio::error_code unmapped(ENOSYS, std::system_category());
    EXPECT_EQ(async::classify_asio_error(unmapped),
              async::HttpErrorKind::Unknown);
}

TEST(HttpErrorTaxonomy, HttpErrorCtor) {
    async::HttpError e(async::HttpErrorKind::TooManyRequests, "rate-limited");
    EXPECT_EQ(e.kind, async::HttpErrorKind::TooManyRequests);
    EXPECT_STREQ(e.what(), "rate-limited");
    EXPECT_EQ(e.http_status, 0);
    EXPECT_TRUE(e.retry_after.empty());
}
