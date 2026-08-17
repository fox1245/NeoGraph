#pragma once

#include "../core/canonical_json.h"

namespace neograph::program::detail {

using neograph::detail::canonical_json_bytes;
using neograph::detail::is_semantic_version;
using neograph::detail::is_sha256_identity;
using neograph::detail::owned_json_copy;
using neograph::detail::parse_json_strict;
using neograph::detail::reject_unknown_fields;
using neograph::detail::validate_json_pointer;
using neograph::detail::validate_token;
using neograph::detail::validate_utf8;

inline std::string sha256_identity(std::string_view domain, std::string_view bytes) {
    return neograph::detail::sha256_identity(
        "NeoGraph Program identity v1", domain, bytes);
}

}  // namespace neograph::program::detail
