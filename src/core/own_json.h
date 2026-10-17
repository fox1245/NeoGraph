// Internal: reads back JSON text that NeoGraph wrote itself (#483).
//
// json::parse refuses text nested deeper than json::max_parse_depth, which is
// meant for text from peers, files and callers. Writing a value has no depth
// limit, and a stored record wraps values in further levels, so a store that
// read its own records with that limit could save a checkpoint it then fails
// to load. Text NeoGraph wrote is read at any depth, as before the limit.
#pragma once

#include <neograph/json.h>

#include <cstddef>
#include <limits>
#include <string_view>

namespace neograph::detail {

// Parses a record a store saved, or a value serialized a moment ago to copy it.
inline json parse_own_json(std::string_view text) {
    return json::parse(text, std::numeric_limits<std::size_t>::max());
}

}  // namespace neograph::detail
