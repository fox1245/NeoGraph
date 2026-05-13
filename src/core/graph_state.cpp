#include <neograph/graph/state.h>
#include <neograph/graph/cancel.h>
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace neograph::graph {

// DX helper: comma-separated sorted list of declared channel names. Used by
// the "Write to unknown channel" error so the user immediately sees what
// IS declared, instead of having to compare against the JSON definition.
// Caller already holds the mutex.
static std::string declared_channel_list(
    const std::map<std::string, Channel>& channels) {
    if (channels.empty()) return "(none — no channels declared in the graph definition)";
    std::string out;
    bool first = true;
    for (const auto& kv : channels) {  // std::map iterates sorted by key
        if (!first) out += ", ";
        first = false;
        out += kv.first;
    }
    return out;
}

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
        throw std::runtime_error(
            "Write to unknown channel: '" + channel + "'. "
            "Declared channels: " + declared_channel_list(channels_) + ". "
            "Channel names are case-sensitive; add it to the graph "
            "definition's \"channels\" block before writing. "
            "See docs/troubleshooting.md \"Write to unknown channel\".");
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
            throw std::runtime_error(
                "Write to unknown channel: '" + w.channel + "'. "
                "Declared channels: " + declared_channel_list(channels_) + ". "
                "Channel names are case-sensitive; add it to the graph "
                "definition's \"channels\" block before writing. "
                "See docs/troubleshooting.md \"Write to unknown channel\".");
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

void GraphState::set_run_cancel_token(std::shared_ptr<CancelToken> tok) {
    // No mutex needed — set once at the top of execute_graph_async,
    // before any node dispatch. Read-only thereafter for the run's
    // duration. The shared_ptr is the lifetime guard.
    run_cancel_token_ = std::move(tok);
}

CancelToken* GraphState::run_cancel_token() const noexcept {
    return run_cancel_token_.get();
}

std::shared_ptr<CancelToken> GraphState::run_cancel_token_shared() const noexcept {
    return run_cancel_token_;
}

} // namespace neograph::graph
