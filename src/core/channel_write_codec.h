#pragma once

#include <neograph/graph/types.h>
#include <neograph/provider_outcome_codec.h>

namespace neograph::graph::detail {

inline json serialize_channel_writes(const std::vector<ChannelWrite>& writes,
    const std::shared_ptr<sp::NativeArchive>& archive = {}, bool native_custody = false) {
    json result = json::array();
    for (const auto& write : writes) {
        json item{{"channel", write.channel}, {"value", write.value}};
        if (write.mode == ChannelWrite::Mode::Overwrite) {
            item["mode"] = "overwrite";
        }
        if (write.native_messages) {
            sp::Completion completion;
            completion.messages = *write.native_messages;
            const sp::Outcome outcome(std::move(completion));
            if (native_custody) item["provider_messages_projection"] = provider_codec::observe_outcome(outcome);
            else item["provider_messages"] = provider_codec::encode_outcome(
                outcome, archive, "channel-write:" + write.channel + write.value.dump());
        }
        result.push_back(std::move(item));
    }
    return result;
}

inline std::vector<ChannelWrite> deserialize_channel_writes(const json& value,
    const std::shared_ptr<sp::NativeArchive>& archive = {}) {
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
        if (item.contains("provider_messages_projection"))
            throw std::invalid_argument("Native pending write requires original C++ custody");
        if (item.contains("provider_messages")) {
            auto outcome = provider_codec::decode_outcome(item.at("provider_messages"), archive,
                "channel-write:" + write.channel + write.value.dump());
            write.native_messages = std::make_shared<const std::vector<sp::Message>>(outcome_messages(*outcome));
        }
        result.push_back(std::move(write));
    }
    return result;
}

}  // namespace neograph::graph::detail
