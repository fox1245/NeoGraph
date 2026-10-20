#pragma once
// A SchemaProvider whose endpoint calls reach a loopback peer: the built-in policy, with every list that names a
// provider origin also naming the peer's.

#include "typed_wire_peer.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace neograph::test::endpoints {
using namespace std::chrono_literals;

inline sp::descriptor::PolicySnapshot policy_at(const std::string& origin,
                                                const std::vector<std::string>& providers = {"https://api.openai.com"}) {
    auto policy = json::parse(sp::config_defaults::descriptor_policy_json).dump();
    for (const auto& provider : providers) {
        const auto quoted = "\"" + provider + "\"";
        for (auto at = policy.find(quoted); at != std::string::npos; at = policy.find(quoted, at + 1)) {
            const auto added = quoted + ",\"" + origin + "\"";
            policy.replace(at, quoted.size(), added);
            at += added.size() - 1;
        }
    }
    auto admitted = sp::descriptor::load_policy(policy, sp::config_defaults::codec_defaults_json);
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&admitted))
        throw std::invalid_argument(error->pointer + ": " + error->message);
    return std::get<sp::descriptor::PolicySnapshot>(std::move(admitted));
}

inline std::shared_ptr<llm::SchemaProvider> provider_at(
    const std::string& origin, bool admitted = true,
    const std::vector<std::string>& providers = {"https://api.openai.com"}) {
    sp::runtime::Options options;
    options.api_key = "fixture-key";
    options.default_timeout = 5s;
    options.retry_tokens = 0;
    options.retry_tokens_per_second = 0;
    auto policy = admitted ? policy_at(origin, providers) : sp::descriptor::builtin_policy();
    return std::make_shared<llm::SchemaProvider>(test::descriptor("openai.chat", origin, std::move(policy)),
                                                 std::move(options));
}

inline std::string embedding_body() {
    return json{{"object", "list"}, {"model", "fixture-embed"},
                {"data", json::array({{{"object", "embedding"}, {"index", 0}, {"embedding", {0.25, -0.5}}}})},
                {"usage", {{"prompt_tokens", 3}, {"total_tokens", 3}}}}.dump();
}

// A 1x1 PNG.
inline const std::string png =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";
inline std::string image_body() {
    return json{{"created", 1}, {"data", json::array({{{"b64_json", png}}})},
                {"usage", {{"input_tokens", 5}, {"output_tokens", 7}, {"total_tokens", 12}}}}.dump();
}
// OpenRouter video jobs: a submit answers pending, polls answer from `polls` in order, and the content is fixed.
class VideoPeer {
public:
    explicit VideoPeer(std::vector<std::string> polls) : polls_(std::move(polls)) {
        server_.Post("/api/v1/videos", [this](const httplib::Request&, httplib::Response& response) {
            ++submits;
            response.set_content(R"({"id":"job-1","status":"pending"})", "application/json");
        });
        server_.Get(R"(/api/v1/videos/job-1)", [this](const httplib::Request&, httplib::Response& response) {
            const auto index = std::min<std::size_t>(polls_seen++, polls_.size() - 1);
            json body = {{"id", "job-1"}, {"status", polls_[index]}};
            if (polls_[index] == "completed")
                body["unsigned_urls"] = json::array({origin() + "/api/v1/videos/job-1/content?index=0"});
            response.set_content(body.dump(), "application/json");
        });
        server_.Get(R"(/api/v1/videos/job-1/content)", [this](const httplib::Request&, httplib::Response& response) {
            ++downloads;
            response.set_content("fixture-video-bytes", "video/mp4");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        worker_ = std::thread([this] { server_.listen_after_bind(); });
        for (int i = 0; i != 200 && !server_.is_running(); ++i) std::this_thread::sleep_for(5ms);
    }
    ~VideoPeer() { server_.stop(); worker_.join(); }
    std::string origin() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::atomic<int> submits{0}, downloads{0};
    std::atomic<std::size_t> polls_seen{0};
private:
    std::vector<std::string> polls_;
    httplib::Server server_;
    int port_ = 0;
    std::thread worker_;
};
// OpenAI files: each upload answers a file numbered by the uploads so far, and the peer keeps the bytes it received.
class UploadPeer {
public:
    UploadPeer() {
        server_.Post("/v1/files", [this](const httplib::Request& request, httplib::Response& response) {
            std::lock_guard lock(mutex_);
            const auto file = request.form.get_file("file");
            received_.push_back(file.content);
            response.set_content(
                json{{"object", "file"}, {"id", "file-" + std::to_string(received_.size())},
                     {"purpose", request.form.get_field("purpose")}, {"filename", file.filename},
                     {"bytes", file.content.size()}, {"created_at", 1}, {"status", "processed"}}.dump(),
                "application/json");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        worker_ = std::thread([this] { server_.listen_after_bind(); });
        for (int i = 0; i != 200 && !server_.is_running(); ++i) std::this_thread::sleep_for(5ms);
    }
    ~UploadPeer() { server_.stop(); worker_.join(); }
    std::string origin() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::vector<std::string> received() const {
        std::lock_guard lock(mutex_);
        return received_;
    }
private:
    mutable std::mutex mutex_;
    std::vector<std::string> received_;
    httplib::Server server_;
    int port_ = 0;
    std::thread worker_;
};
}  // namespace neograph::test::endpoints
