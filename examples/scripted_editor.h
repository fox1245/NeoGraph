// A scripted ACP client for the examples: the stand-in for an editor.
//
// In production an editor starts the agent as a child process and talks JSON-RPC over its stdin
// and stdout. Here the "editor" lives in the same process: it hands envelopes to
// ACPServer::handle_message and receives everything the agent sends through the notification
// sink. It does what the protocol asks of a client:
//   * initialize, with the client capabilities the example passes in, and session/new;
//   * session/prompt, waiting for the turn's response;
//   * every request the agent sends back (session/request_permission, elicitation/create, ...)
//     is answered by the `answer` function the example supplies, on its own thread, the way an
//     editor answers after the user clicks.
// It prints a one-line trace of each message, so an example's output is the protocol exchange.
//
// Link: neograph::core neograph::acp.
#pragma once

#include <neograph/acp/server.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace examples {

class ScriptedEditor {
public:
    // Result body for one request from the agent; `request` is the whole JSON-RPC envelope.
    using Answer = std::function<neograph::json(const neograph::json& request)>;

    ScriptedEditor(neograph::acp::ACPServer& server, neograph::json client_capabilities, Answer answer)
        : server_(server), answer_(std::move(answer)) {
        server_.set_notification_sink([this](const neograph::json& envelope) { on_message(envelope); });
        const auto reply = server_.handle_message(request(next_id_++, "initialize",
            {{"protocolVersion", 1}, {"clientCapabilities", std::move(client_capabilities)}}));
        if (!reply.contains("result")) throw std::runtime_error("initialize failed: " + reply.dump());
    }
    ~ScriptedEditor() {
        for (auto& thread : answering_) thread.join();
    }
    ScriptedEditor(const ScriptedEditor&) = delete;
    ScriptedEditor& operator=(const ScriptedEditor&) = delete;

    std::string new_session() {
        const auto reply = server_.handle_message(request(next_id_++, "session/new",
            {{"cwd", "/workspace"}, {"mcpServers", neograph::json::array()}}));
        return reply.at("result").at("sessionId").get<std::string>();
    }

    // Send one prompt and wait for the turn to end. Returns the session/prompt response.
    neograph::json prompt(const std::string& session_id, const std::string& text) {
        const int id = next_id_++;
        std::cout << "[editor] -> prompt: " << text << "\n";
        server_.handle_message(request(id, "session/prompt",
            {{"sessionId", session_id},
             {"prompt", neograph::json::array({{{"type", "text"}, {"text", text}}})}}));
        std::unique_lock lock(mutex_);
        if (!arrived_.wait_for(lock, std::chrono::seconds(10), [&] { return responses_.count(id) != 0; }))
            throw std::runtime_error("the agent did not finish the turn within 10 s");
        return responses_[id];
    }

private:
    static neograph::json request(int id, const char* method, neograph::json params) {
        return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
    }

    void say(const std::string& line) {
        std::lock_guard lock(mutex_);
        std::cout << "[editor] " << line << "\n" << std::flush;
    }

    void on_message(const neograph::json& envelope) {
        const auto method = envelope.value("method", std::string());
        if (method == "session/update") {
            const auto update = envelope.at("params").at("update");
            std::string line = "<- update " + update.value("sessionUpdate", std::string());
            if (update.contains("content") && update["content"].contains("text"))
                line += ": " + update["content"]["text"].get<std::string>();
            say(line);
        } else if (!method.empty() && envelope.contains("id")) {
            // A request from the agent: answer it later, off the agent's thread.
            say("<- request " + method);
            std::lock_guard lock(mutex_);
            answering_.emplace_back([this, envelope] {
                neograph::json reply = {{"jsonrpc", "2.0"}, {"id", envelope["id"]}, {"result", answer_(envelope)}};
                say("-> answer " + reply["result"].dump());
                server_.handle_message(reply);
            });
        } else if (method.empty() && envelope.contains("id")) {
            std::lock_guard lock(mutex_);
            responses_[envelope["id"].get<int>()] = envelope;
            arrived_.notify_all();
        }
    }

    neograph::acp::ACPServer& server_;
    Answer answer_;
    int next_id_ = 1;
    std::mutex mutex_;
    std::condition_variable arrived_;
    std::map<int, neograph::json> responses_;
    std::vector<std::thread> answering_;
};

}  // namespace examples
