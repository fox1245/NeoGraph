/**
 * @file program/coordinate.h
 * @brief Immutable content-addressed coordinates for reusable Program modules.
 */
#pragma once

#include <neograph/api.h>

#include <string>

namespace neograph::program {

/** Exact module address; the content identity is never selected by a range. */
struct ModuleCoordinate {
    std::string namespace_name;
    std::string name;
    std::string semantic_version;
    std::string content_identity;

    bool operator==(const ModuleCoordinate&) const = default;
    std::string qualified_name() const;
};

}  // namespace neograph::program
