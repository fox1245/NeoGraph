/**
 * @file ids.h
 * @brief Internal: A2A identifiers and timestamps.
 *
 * Message, task and context ids are RFC 9562 version-4 UUIDs drawn from a
 * cryptographically secure generator, so two processes never produce the same
 * id (spec 4.1.4: agents may de-duplicate on messageId). Status timestamps are
 * RFC 3339 UTC with millisecond precision, the ProtoJSON form of
 * google.protobuf.Timestamp.
 *
 * Internal header: not installed, no compatibility promise.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::a2a::detail {

/// A random version-4 UUID in lower-case 8-4-4-4-12 form. Throws
/// std::runtime_error when the platform generator fails.
std::string random_uuid();

/// The current UTC time as "YYYY-MM-DDTHH:MM:SS.mmmZ".
std::string utc_timestamp_now();

/// Milliseconds since the Unix epoch of an RFC 3339 timestamp
/// ("2026-10-12T10:00:00Z", fractional seconds and a numeric offset allowed),
/// or nullopt when the text is not one.
std::optional<std::int64_t> parse_timestamp_ms(std::string_view text);

}  // namespace neograph::a2a::detail
