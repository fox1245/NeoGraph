#include "prompt_prefix.h"

#include "canonical_json.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace neograph::detail {
namespace {

constexpr std::string_view PREFIX_PREAMBLE = "NeoGraph Runtime turn identity v1";

using Step = std::variant<std::string, std::size_t>;
using Path = std::vector<Step>;

void find_marker(const json& value, std::string_view marker, Path& path, std::vector<Path>& found) {
    if (value.is_string()) {
        if (value.get<std::string>().find(marker) != std::string::npos) found.push_back(path);
    } else if (value.is_array()) {
        for (std::size_t index = 0; index < value.size(); ++index) {
            path.emplace_back(index);
            find_marker(value.at(index), marker, path, found);
            path.pop_back();
        }
    } else if (value.is_object()) {
        for (const auto& [key, member] : value.items()) {
            path.emplace_back(key);
            find_marker(member, marker, path, found);
            path.pop_back();
        }
    }
}

// The members that lead to the part of a message that holds the marker: the keys in front of the first
// array index on the path. A message whose content is a string, or whose element is the part itself, has
// no such array and is a single part.
struct Parts {
    std::size_t              index = 0;
    bool                     has_array = false;
    std::vector<std::string> container;  // keys from the message element to the array that holds its parts
};

Parts parts_of(const Path& path) {
    Parts result;
    for (std::size_t step = 0; step < path.size(); ++step) {
        if (const auto* index = std::get_if<std::size_t>(&path[step])) {
            result.index = *index;
            result.has_array = true;
            return result;
        }
        result.container.push_back(std::get<std::string>(path[step]));
    }
    result.container.clear();
    return result;
}

// The array of parts of `element` at `container`, or null when it is not there.
std::optional<json> parts_array(const json& element, const std::vector<std::string>& container) {
    json current = element;
    for (const auto& key : container) {
        if (!current.is_object() || !current.contains(key)) return std::nullopt;
        current = current.at(key);
    }
    if (!current.is_array()) return std::nullopt;
    return current;
}

// `element` with the array at `container` cut to its first `keep` entries.
json cut_parts(const json& element, const std::vector<std::string>& container, std::size_t step, std::size_t keep) {
    if (step == container.size()) {
        json cut = json::array();
        for (std::size_t index = 0; index < keep && index < element.size(); ++index) cut.push_back(element.at(index));
        return cut;
    }
    if (!element.is_object()) return element;
    json result = json::object();
    for (const auto& [name, member] : element.items()) {
        result[name] = name == container[step] ? cut_parts(member, container, step + 1, keep) : member;
    }
    return result;
}

bool other_members_equal(const json& full, const json& probe, const WireMembers& members) {
    const auto named = [&](const std::string& key) {
        return key == members.messages || key == members.stream || key == members.output_cap;
    };
    std::size_t counted = 0;
    for (const auto& [key, value] : full.items()) {
        if (named(key)) continue;
        ++counted;
        if (!probe.contains(key) || !(probe.at(key) == value)) return false;
    }
    std::size_t in_probe = 0;
    for (const auto& [key, value] : probe.items()) {
        (void)value;
        if (!named(key)) ++in_probe;
    }
    return counted == in_probe;
}

}  // namespace

ProbeResult locate_prefix_boundary(const json& full, const json& probe, const WireMembers& members,
                                   std::string_view marker) {
    ProbeResult result;
    if (!full.is_object() || !probe.is_object() || members.messages.empty() ||
        !full.contains(members.messages) || !probe.contains(members.messages)) {
        return result;
    }
    const auto full_messages = full.at(members.messages);
    const auto probe_messages = probe.at(members.messages);
    if (!full_messages.is_array() || !probe_messages.is_array()) return result;
    if (!other_members_equal(full, probe, members)) {
        result.status = PrefixProbe::MembersDiffer;
        return result;
    }

    std::vector<Path> found;
    Path path;
    for (std::size_t index = 0; index < probe_messages.size(); ++index) {
        Path inner;
        std::vector<Path> in_message;
        find_marker(probe_messages.at(index), marker, inner, in_message);
        for (auto& hit : in_message) {
            Path whole{index};
            whole.insert(whole.end(), hit.begin(), hit.end());
            found.push_back(std::move(whole));
        }
    }
    if (found.size() != 1) return result;
    const auto message_index = std::get<std::size_t>(found.front().front());
    const Path inside(found.front().begin() + 1, found.front().end());
    const auto parts = parts_of(inside);

    // Everything in front of the marker has to be what the full body carries.
    if (message_index > full_messages.size()) return result;
    for (std::size_t index = 0; index < message_index; ++index) {
        if (!(full_messages.at(index) == probe_messages.at(index))) return result;
    }
    if (parts.has_array && parts.index > 0) {
        if (message_index >= full_messages.size()) return result;
        const auto full_parts = parts_array(full_messages.at(message_index), parts.container);
        if (!full_parts || full_parts->size() < parts.index) return result;
        const auto cut_probe = cut_parts(probe_messages.at(message_index), parts.container, 0, parts.index);
        const auto cut_full = cut_parts(full_messages.at(message_index), parts.container, 0, parts.index);
        if (!(cut_probe == cut_full)) return result;
        // A prefix that takes every part of a message ends at the next message.
        if (full_parts->size() == parts.index) {
            result.boundary = ContextPrefixBoundary{message_index + 1, 0};
        } else {
            result.boundary = ContextPrefixBoundary{message_index, parts.index};
            result.container = parts.container;
        }
    } else {
        result.boundary = ContextPrefixBoundary{message_index, 0};
    }
    result.status = PrefixProbe::Located;
    return result;
}

ContextPrefixBoundary whole_body_boundary(const json& full, const WireMembers& members) {
    const auto size = full.is_object() && full.contains(members.messages) ? full.at(members.messages).size() : 0;
    return ContextPrefixBoundary{size, 0};
}

std::string wire_prefix_digest(const json& full, const WireMembers& members, const ContextPrefixBoundary& boundary,
                               const std::vector<std::string>& container) {
    json prefix = json::object();
    for (const auto& [key, value] : full.items()) {
        if (key == members.messages || key == members.stream || key == members.output_cap) continue;
        prefix[key] = value;
    }
    json messages = json::array();
    if (full.contains(members.messages)) {
        const auto all = full.at(members.messages);
        for (std::size_t index = 0; index < boundary.message_index && index < all.size(); ++index) {
            messages.push_back(all.at(index));
        }
        if (boundary.part_index > 0 && boundary.message_index < all.size()) {
            // The indices are 64 bits wide and json::at takes a size_t, which is 32 bits on wasm32.
            const auto message_index = static_cast<std::size_t>(boundary.message_index);
            const auto keep = static_cast<std::size_t>(
                std::min<std::uint64_t>(boundary.part_index, std::numeric_limits<std::size_t>::max()));
            messages.push_back(cut_parts(all.at(message_index), container, 0, keep));
        }
    }
    prefix[members.messages] = messages;
    return sha256_identity(PREFIX_PREAMBLE, "wire-prefix/v1", canonical_json_bytes(prefix));
}

}  // namespace neograph::detail
