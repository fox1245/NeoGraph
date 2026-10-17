// Internal: a depth limit for NeoGraph's own recursive walks over json values (#485).
//
// json::parse and Program canonicalization refuse values nested deeper than
// json::max_parse_depth, but a walk can be given a value that passed neither:
// one the host built in code, text parsed with a higher limit, or a record a
// store read back. A walk that recurses once per level calls check_walk_depth
// for each array or object it enters, and fails past the limit instead of
// running out of stack. It throws rather than returning early, which would
// skip a check or a redaction.
#pragma once

#include <neograph/json.h>

#include <cstddef>
#include <stdexcept>
#include <string>

namespace neograph::detail {

// `depth` counts the arrays and objects around the one the walk enters.
inline void check_walk_depth(std::size_t depth, const char* walk) {
    if (depth < json::max_parse_depth) return;
    throw std::invalid_argument(std::string(walk) + ": arrays and objects nested deeper than " +
                                std::to_string(json::max_parse_depth) + " levels");
}

}  // namespace neograph::detail
