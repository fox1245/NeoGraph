// Internal. Where a prefix of an encoded request body ends, read from the body itself.
//
// The stable-first assembly needs two positions in the body a provider will see: the end of the Stable
// part and the end of the append-only part. Hoisting of System messages and regrouping of tool results
// happen while the body is written, so a position computed from the typed messages would be a guess.
// Instead the request is encoded again with only the messages of a prefix, followed by a message with a
// marker text. The marker shows where the prefix ends in the encoded body, and the rest of the body has to
// agree with the full one up to that point; if it does not, there is no boundary to report.
#pragma once

#include <neograph/json.h>
#include <neograph/runtime_context.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::detail {

// The members of an encoded body that the descriptor names by role.
struct WireMembers {
    std::string messages;
    std::string stream;
    std::string output_cap;
};

enum class PrefixProbe : std::uint8_t {
    Located,         // the marker was found and everything before it agrees with the full body
    MembersDiffer,   // a member other than the messages differs between the probe and the full body
    Unreadable,      // the marker is missing, repeated, or the bodies disagree in front of it
};

struct ProbeResult {
    PrefixProbe                          status = PrefixProbe::Unreadable;
    std::optional<ContextPrefixBoundary> boundary;
    // The keys from a message to the array of its parts, when the boundary falls inside a message.
    std::vector<std::string>             container;
};

// Compare an encoded probe with the encoded full body and read the boundary the marker names.
ProbeResult locate_prefix_boundary(const json& full, const json& probe, const WireMembers& members,
                                   std::string_view marker);

// The boundary of a prefix that is the whole body.
ContextPrefixBoundary whole_body_boundary(const json& full, const WireMembers& members);

// Digest of the body up to `boundary`: every member except the stream flag and the output cap, with the
// messages cut at the boundary.
std::string wire_prefix_digest(const json& full, const WireMembers& members,
                               const ContextPrefixBoundary& boundary,
                               const std::vector<std::string>& container = {});

}  // namespace neograph::detail
