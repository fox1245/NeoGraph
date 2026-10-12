#include "graph_host_contract.h"
#include <neograph/acp/server.h>

#include <optional>
#include <unordered_map>

namespace {
using namespace graph_host_contract;
namespace acp = neograph::acp;

struct Responses {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<json> messages;
    void emit(const json& message) {
        std::lock_guard lock(mu);
        messages.push_back(message);
        cv.notify_all();
    }
    static bool is_response(const json& message, int id) {
        return !message.contains("method") && message.value("id", -1) == id;
    }
    static bool is_interrupt_question(const json& message, const std::string& sid) {
        return message.value("method", std::string()) == "session/request_permission" &&
               message.at("params").value("sessionId", std::string()) == sid;
    }
    json wait_for(int id) {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, 3s, [&] {
            for (const auto& message : messages)
                if (is_response(message, id)) return true;
            return false;
        })) throw std::runtime_error("ACP prompt response timed out");
        for (const auto& message : messages)
            if (is_response(message, id)) return message;
        throw std::logic_error("lost ACP response");
    }
    /// The turn's response, or the session/request_permission an interrupt
    /// raised during the turn, whichever comes first.
    json wait_for_turn(int id, const std::string& sid) {
        std::unique_lock lock(mu);
        auto find = [&]() -> std::optional<json> {
            for (const auto& message : messages)
                if (is_response(message, id) || is_interrupt_question(message, sid))
                    return message;
            return std::nullopt;
        };
        if (!cv.wait_for(lock, 3s, [&] { return find().has_value(); }))
            throw std::runtime_error("ACP prompt response timed out");
        return *find();
    }
    std::vector<json> snapshot() {
        std::lock_guard lock(mu);
        return messages;
    }
};

class ACPHost final : public Host {
public:
    ACPHost(GraphFixture& fixture, std::size_t limit)
        : server_(fixture.engine, {{"name", "contract"}, {"version", "0.0.1"}}) {
        server_.set_max_inflight_prompts(limit);
        server_.set_notification_sink([this](const json& env) { responses_.emit(env); });
        auto response = server_.handle_message({
            {"jsonrpc", "2.0"}, {"id", "init"}, {"method", "initialize"},
            {"params", {{"protocolVersion", 1}, {"clientCapabilities", json::object()}}}});
        if (!response.contains("result")) throw std::runtime_error("ACP init failed");
    }
    ~ACPHost() override { shutdown(); }

    std::future<Result> start(const std::string& identity,
                               const std::string& prompt, int) override {
        auto session = server_.handle_message(request(
            1000, "session/new", {{"cwd", "/work"}, {"mcpServers", json::array()}}));
        if (!session.contains("result")) throw std::runtime_error("ACP session/new failed");
        auto sid = session["result"].at("sessionId").get<std::string>();
        const auto id = ++next_id_;
        {
            std::lock_guard lock(mu_);
            sessions_[identity] = sid;
        }
        auto immediate = server_.handle_message(request(id, "session/prompt",
            {{"sessionId", sid}, {"prompt", json::array({{{"type", "text"}, {"text", prompt}}})}}));
        if (!immediate.is_null()) throw std::runtime_error("ACP prompt did not dispatch asynchronously");
        return std::async(std::launch::async, [this, id, sid] {
            Result result;
            auto response = responses_.wait_for_turn(id, sid);
            if (response.contains("method")) {
                // ACP carries a NodeInterrupt as session/request_permission in
                // the same turn. The contract only observes the pause, so the
                // host declines to answer and the turn ends.
                (void)server_.handle_message({
                    {"jsonrpc", "2.0"}, {"id", response.at("id")},
                    {"result", {{"outcome", {{"outcome", "cancelled"}}}}}});
                (void)responses_.wait_for(id);
                result.terminal = Terminal::interrupted;
                result.events.push_back("terminal");
                return result;
            }
            if (response.contains("error")) {
                // A refused turn carries data.reason; a failed one data.code.
                const auto data = response["error"].value("data", json::object());
                result.terminal = data.contains("reason") ? Terminal::rejected : Terminal::error;
                result.events.push_back("terminal");
                return result;
            }
            auto payload = response.at("result");
            auto status = payload.value("_meta", json::object())
                                 .value("neograph/invocation_status", std::string());
            if (status == "completed") result.terminal = Terminal::completed;
            else if (status == "interrupted") result.terminal = Terminal::interrupted;
            else if (status == "max_steps") result.terminal = Terminal::max_steps;
            else if (status == "cancelled") result.terminal = Terminal::cancelled;
            else if (status == "error") result.terminal = Terminal::error;
            else throw std::runtime_error("ACP response lacks graph terminal status");
            for (auto& message : responses_.snapshot()) {
                if (message.value("method", std::string()) == "session/update" &&
                    message.at("params").value("sessionId", std::string()) == sid) {
                    result.events.push_back("update");
                    const auto update = message.at("params").at("update");
                    if (update.value("sessionUpdate", std::string()) == "agent_message_chunk" &&
                        update.contains("content") && update["content"].is_object())
                        result.output = update["content"].value("text", std::string());
                }
            }
            result.events.push_back("terminal");
            return result;
        });
    }
    std::string effective_identity(const std::string& identity) const override {
        std::lock_guard lock(mu_);
        return sessions_.at(identity);
    }
    void cancel(const std::string& identity) override {
        (void)server_.handle_message({{"jsonrpc", "2.0"}, {"method", "session/cancel"},
                                     {"params", {{"sessionId", effective_identity(identity)}}}});
    }
    void shutdown() override {
        std::vector<std::string> sessions;
        {
            std::lock_guard lock(mu_);
            for (const auto& [_, sid] : sessions_) sessions.push_back(sid);
        }
        for (const auto& sid : sessions)
            (void)server_.handle_message({{"jsonrpc", "2.0"}, {"method", "session/cancel"},
                                          {"params", {{"sessionId", sid}}}});
        server_.stop();
    }
private:
    static json request(int id, const std::string& method, json params) {
        return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method},
                {"params", std::move(params)}};
    }
    Responses responses_; // Must outlive the server and its worker threads.
    acp::ACPServer server_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::string> sessions_;
    std::atomic<int> next_id_{0};
};

TEST(GraphHostACPContract, SharedBoundary) {
    check_contract([](GraphFixture& fixture, std::size_t limit) {
        return std::make_unique<ACPHost>(fixture, limit);
    });
}
} // namespace
