/**
 * @file graph/channel_key.h
 * @brief Typed names for JSON-backed graph state channels.
 */
#pragma once

#include <string>
#include <utility>

namespace neograph::graph {

/**
 * @brief Bind a channel name to its expected C++ value type.
 *
 * ChannelKey does not change the JSON-backed channel model. Keep keys as
 * reusable constants and pass them to GraphState or RunResult accessors.
 */
template <typename T>
class ChannelKey {
public:
    using value_type = T;

    explicit ChannelKey(std::string name) : name_(std::move(name)) {}

    const std::string& name() const noexcept { return name_; }

private:
    std::string name_;
};

}  // namespace neograph::graph
