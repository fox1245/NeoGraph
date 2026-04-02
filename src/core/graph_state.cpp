#include <neograph/graph/state.h>
#include <stdexcept>

namespace neograph::graph {

void GraphState::init_channel(const std::string& name,
                               ReducerType type,
                               ReducerFn reducer,
                               const json& initial_value) {
    std::unique_lock lock(mutex_);
    Channel ch;
    ch.name         = name;
    ch.reducer_type = type;
    ch.reducer      = std::move(reducer);
    ch.value        = initial_value;
    channels_[name] = std::move(ch);
}

json GraphState::get(const std::string& channel) const {
    std::shared_lock lock(mutex_);
    auto it = channels_.find(channel);
    if (it == channels_.end()) return json();
    return it->second.value;
}

std::vector<ChatMessage> GraphState::get_messages() const {
    auto msgs_json = get("messages");
    std::vector<ChatMessage> messages;
    if (msgs_json.is_array()) {
        for (const auto& j : msgs_json) {
            ChatMessage msg;
            from_json(j, msg);
            messages.push_back(std::move(msg));
        }
    }
    return messages;
}

void GraphState::write(const std::string& channel, const json& value) {
    std::unique_lock lock(mutex_);
    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        throw std::runtime_error("Write to unknown channel: " + channel);
    }
    auto& ch  = it->second;
    ch.value   = ch.reducer(ch.value, value);
    ch.version = ++global_version_;
}

void GraphState::apply_writes(const std::vector<ChannelWrite>& writes) {
    std::unique_lock lock(mutex_);
    for (const auto& w : writes) {
        auto it = channels_.find(w.channel);
        if (it == channels_.end()) {
            throw std::runtime_error("Write to unknown channel: " + w.channel);
        }
        auto& ch  = it->second;
        ch.value   = ch.reducer(ch.value, w.value);
        ch.version = ++global_version_;
    }
}

uint64_t GraphState::channel_version(const std::string& channel) const {
    std::shared_lock lock(mutex_);
    auto it = channels_.find(channel);
    return it != channels_.end() ? it->second.version : 0;
}

uint64_t GraphState::global_version() const {
    std::shared_lock lock(mutex_);
    return global_version_;
}

json GraphState::serialize() const {
    std::shared_lock lock(mutex_);
    json data;
    for (const auto& [name, ch] : channels_) {
        data["channels"][name] = {
            {"value",   ch.value},
            {"version", ch.version}
        };
    }
    data["global_version"] = global_version_;
    return data;
}

void GraphState::restore(const json& data) {
    std::unique_lock lock(mutex_);
    if (data.contains("channels")) {
        for (const auto& [name, ch_data] : data["channels"].items()) {
            auto it = channels_.find(name);
            if (it != channels_.end()) {
                it->second.value   = ch_data["value"];
                it->second.version = ch_data.value("version", uint64_t(0));
            }
        }
    }
    global_version_ = data.value("global_version", uint64_t(0));
}

std::vector<std::string> GraphState::channel_names() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> names;
    for (const auto& [name, _] : channels_) {
        names.push_back(name);
    }
    return names;
}

} // namespace neograph::graph
