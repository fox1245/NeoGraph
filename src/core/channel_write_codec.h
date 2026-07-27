#pragma once

#include <neograph/graph/types.h>

namespace neograph::graph::detail {

inline json serialize_channel_writes(const std::vector<ChannelWrite>& writes) {
    json result = json::array();
    for (const auto& write : writes) {
        json item{{"channel", write.channel}, {"value", write.value}};
        if (write.mode == ChannelWrite::Mode::Overwrite) {
            item["mode"] = "overwrite";
        }
        result.push_back(std::move(item));
    }
    return result;
}

inline std::vector<ChannelWrite> deserialize_channel_writes(const json& value) {
    std::vector<ChannelWrite> result;
    if (!value.is_array()) return result;
    result.reserve(value.size());
    for (const auto& item : value) {
        ChannelWrite write{
            item.value("channel", std::string{}),
            item.contains("value") ? item["value"] : json(),
        };
        if (item.value("mode", std::string{}) == "overwrite") {
            write.mode = ChannelWrite::Mode::Overwrite;
        }
        result.push_back(std::move(write));
    }
    return result;
}

}  // namespace neograph::graph::detail
