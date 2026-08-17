#pragma once

#include <neograph/api.h>
#include <neograph/json.h>

#include <initializer_list>
#include <string>
#include <string_view>

namespace neograph::detail {

NEOGRAPH_API json        owned_json_copy(const json& value);
NEOGRAPH_API std::string canonical_json_bytes(const json& value);
NEOGRAPH_API json        parse_json_strict(std::string_view bytes);
NEOGRAPH_API std::string sha256_identity(std::string_view preamble,
                                         std::string_view domain,
                                         std::string_view bytes);
NEOGRAPH_API bool        is_sha256_identity(std::string_view value) noexcept;
NEOGRAPH_API bool        is_semantic_version(std::string_view value) noexcept;
NEOGRAPH_API void        validate_utf8(std::string_view value);
NEOGRAPH_API void        validate_token(std::string_view value, std::string_view field);
NEOGRAPH_API void        validate_json_pointer(std::string_view value);
NEOGRAPH_API void        reject_unknown_fields(
    const json& value,
    std::string_view object_name,
    std::initializer_list<std::string_view> allowed_fields);

}  // namespace neograph::detail
