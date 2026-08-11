#pragma once

#include <neograph/json.h>

#include <initializer_list>
#include <string>
#include <string_view>

namespace neograph::program::detail {

json        owned_json_copy(const json& value);
std::string canonical_json_bytes(const json& value);
json        parse_json_strict(std::string_view bytes);
std::string sha256_identity(std::string_view domain, std::string_view bytes);
bool        is_sha256_identity(std::string_view value) noexcept;
bool        is_semantic_version(std::string_view value) noexcept;
void        validate_utf8(std::string_view value);
void        validate_token(std::string_view value, std::string_view field);
void        validate_json_pointer(std::string_view value);
void        reject_unknown_fields(const json&                             value,
                                  std::string_view                        object_name,
                                  std::initializer_list<std::string_view> allowed_fields);

}  // namespace neograph::program::detail
