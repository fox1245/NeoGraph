#pragma once

#include "typed_provider.h"
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>
#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <set>
#include <thread>

namespace neograph::test::wire {
using namespace std::chrono_literals;

struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<json> requests;
    std::vector<std::string> paths, authorization;
    std::string body;
    std::string content_type = "application/json";
    int status = 200;
    std::string retry_after;
    bool hold = false, released = false;
    std::size_t entered = 0, finished = 0;
};

class Peer {
public:
    explicit Peer(std::string body, bool streaming = false)
        : state(std::make_shared<State>()), server(std::make_shared<httplib::Server>()) {
        state->body = std::move(body);
        if (streaming) state->content_type = "text/event-stream";
        server->Post(R"(/.*)", [owned = state](const httplib::Request& req, httplib::Response& res) {
            std::unique_lock lock(owned->mutex);
            owned->requests.push_back(json::parse(req.body));
            owned->paths.push_back(req.path);
            owned->authorization.push_back(req.get_header_value("Authorization"));
            ++owned->entered;
            owned->cv.notify_all();
            owned->cv.wait(lock, [&] { return !owned->hold || owned->released; });
            res.status = owned->status;
            if (!owned->retry_after.empty()) res.set_header("Retry-After", owned->retry_after);
            res.set_content(owned->body, owned->content_type);
            ++owned->finished;
            owned->cv.notify_all();
        });
        port = server->bind_to_any_port("127.0.0.1");
        if (port <= 0) throw std::runtime_error("wire fixture failed to bind");
        worker = std::thread([owned = server] { owned->listen_after_bind(); });
        for (int i = 0; i != 200 && !server->is_running(); ++i) std::this_thread::sleep_for(5ms);
        if (!server->is_running()) { release(); server->stop(); worker.join(); throw std::runtime_error("wire fixture failed to listen"); }
    }
    ~Peer() { release(); server->stop(); if (worker.joinable()) worker.join(); }
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;
    std::string origin() const { return "http://127.0.0.1:" + std::to_string(port); }
    bool await_requests(std::size_t count, std::chrono::milliseconds timeout = 2s) const {
        std::unique_lock lock(state->mutex);
        return state->cv.wait_for(lock, timeout, [&] { return state->entered >= count; });
    }
    void release() const { std::lock_guard lock(state->mutex); state->released = true; state->cv.notify_all(); }
    std::shared_ptr<State> state;
    std::shared_ptr<httplib::Server> server;
    int port = 0;
private:
    std::thread worker;
};

inline std::unique_ptr<llm::SchemaProvider> provider(
    std::string family, const std::string& origin, sp::runtime::Options options = {}) {
    options.default_timeout = 5s;
    options.retry_tokens = 0;
    options.retry_tokens_per_second = 0;
    return llm::SchemaProvider::create(descriptor(std::move(family), origin), std::move(options));
}

// Real SDK dispatch with a shared client, allowing public admission to fence
// producer completion while the separate awaiting executor is paused.
class RuntimeProvider final : public Provider {
public:
    RuntimeProvider(std::shared_ptr<sp::runtime::Client> client, std::string family)
        : client_(std::move(client)), family_(std::move(family)) {}
    std::string get_name() const override { return "wire-runtime"; }
    std::string_view family() const noexcept override { return family_; }
    PreparedProviderRequest prepare(ProviderRequest request) override {
        return prepare_runtime(client_, std::move(request));
    }
private:
    std::shared_ptr<sp::runtime::Client> client_;
    std::string family_;
};
inline ProviderRequest request(std::string_view family = "openai.chat", ProviderMode mode = ProviderMode::Collect,
                               std::string text = "ping") {
    ProviderRequest result;
    result.mode = mode;
    const auto user = message(std::move(text), sp::Role::User);
    if (family == "openai.chat") {
        sp::chat::Request payload; payload.model = "fixture-model"; payload.max_output_tokens = 64;
        payload.canonical_messages.push_back(user); result.payload = std::move(payload);
    } else if (family == "anthropic.messages") {
        sp::messages::Request payload; payload.model = "fixture-model"; payload.max_tokens = 64;
        payload.account_scope = "fixture-account"; payload.messages.push_back(user); result.payload = std::move(payload);
    } else if (family == "openai.responses") {
        sp::responses::Request payload; payload.model = "fixture-model"; payload.max_output_tokens = 64;
        payload.account_scope = "fixture-account"; payload.messages.push_back(user); result.payload = std::move(payload);
    } else if (family == "google.generate") {
        sp::gemini::Request payload; payload.model = "fixture-model"; payload.max_output_tokens = 64;
        payload.account_scope = "fixture-account"; payload.messages.push_back(user); result.payload = std::move(payload);
    } else if (family == "google.interactions") {
        sp::interactions::Request payload; payload.model = "fixture-model"; payload.max_output_tokens = 64;
        payload.account_scope = "fixture-account"; payload.messages.push_back(user); result.payload = std::move(payload);
    } else throw std::invalid_argument("unknown fixture family");
    return result;
}
inline const sp::Failure& failure(const sp::runtime::Result& result) { return std::get<sp::Failure>(*result); }
inline json chat_envelope(json body, bool streaming = false) {
    body["id"] = "chat-fixture";
    body["model"] = "fixture-model";
    body["object"] = streaming ? "chat.completion.chunk" : "chat.completion";
    body["created"] = 1;
    return body;
}
inline std::string chat_frame(json body) {
    return "data: " + chat_envelope(std::move(body), true).dump() + "\n\n";
}
inline std::string chat_response(std::string text = "pong", std::string stop = "stop") {
    return chat_envelope(json{{"choices", json::array({{{"index", 0},
        {"message", {{"role", "assistant"}, {"content", std::move(text)}}}, {"finish_reason", std::move(stop)}}})},
        {"usage", {{"prompt_tokens", 3}, {"completion_tokens", 2}, {"total_tokens", 5}}}}).dump();
}
inline std::string chat_sse(std::string text = "pong") {
    return chat_frame(json{{"choices", json::array({{{"index", 0},
        {"delta", {{"role", "assistant"}, {"content", std::move(text)}}}, {"finish_reason", "stop"}}})}})
        + chat_frame(json{{"choices", json::array()}, {"usage", {{"prompt_tokens", 3}, {"completion_tokens", 2}, {"total_tokens", 5}}}})
        + "data: [DONE]\n\n";
}
inline std::string responses_body(std::string text = "pong") {
    return json{{"id", "response-fixture"}, {"object", "response"}, {"model", "fixture-model"},
        {"created_at", 1}, {"status", "completed"},
        {"output", json::array({{{"type", "message"}, {"id", "msg-fixture"}, {"role", "assistant"}, {"status", "completed"},
            {"content", json::array({{{"type", "output_text"}, {"text", std::move(text)}, {"annotations", json::array()}}})}}})},
        {"usage", {{"input_tokens", 3}, {"output_tokens", 2}, {"total_tokens", 5}}}}.dump();
}
inline std::string responses_sse(std::string text = "pong", bool named = true) {
    auto frame = [named](std::string type, json payload) {
        payload["type"] = type;
        return (named ? "event: " + type + "\n" : "") + "data: " + payload.dump() + "\n\n";
    };
    const auto response = json::parse(responses_body(text));
    const auto item = response.at("output").at(0);
    auto initial = response;
    initial["status"] = "in_progress";
    initial["output"] = json::array();
    return frame("response.created", {{"response", initial}})
        + frame("response.output_item.added", {{"output_index", 0}, {"item", {{"id", "msg-fixture"}, {"type", "message"}, {"role", "assistant"}, {"status", "in_progress"}, {"content", json::array()}}}})
        + frame("response.content_part.added", {{"output_index", 0}, {"item_id", "msg-fixture"}, {"content_index", 0}, {"part", {{"type", "output_text"}, {"text", ""}, {"annotations", json::array()}}}})
        + frame("response.output_text.delta", {{"output_index", 0}, {"item_id", "msg-fixture"}, {"content_index", 0}, {"delta", text}})
        + frame("response.output_text.done", {{"output_index", 0}, {"item_id", "msg-fixture"}, {"content_index", 0}, {"text", text}})
        + frame("response.output_item.done", {{"output_index", 0}, {"item", item}})
        + frame("response.completed", {{"response", response}});
}
} // namespace neograph::test::wire
