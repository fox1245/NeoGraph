// Scripted offline provider for the examples.
//
// A neograph::Provider whose replies are written in the example itself, so a
// model-driven example runs with no API key and no network. Tool calls and
// streaming behave like the real provider: a reply that carries tool calls stops
// with StopKind::ToolUse, and a streaming request receives ordered sp::Event
// observations (one text delta per word). It goes through the same prepare /
// dispatch path as the real provider (prepare_local on an admitted client), so
// the graph, tool and checkpoint code under test is the code you would ship.
//
// Not a model: nothing here understands the prompt. The script decides.
//
//   // 1. A fixed sequence: reply N is returned for call N.
//   auto provider = std::make_shared<examples::ScriptedProvider>(std::vector<examples::Reply>{
//       examples::call_tool("calculator", {{"expression", "2 + 3"}}).with_text("Thought: add."),
//       examples::say("The answer is 5."),
//   });
//
//   // 2. A function of what the "model" sees: needed for parallel fan-out and loops.
//   auto provider = std::make_shared<examples::ScriptedProvider>(
//       [](const examples::ScriptedTurn& turn) {
//           return turn.has_tool_result() ? examples::say("done")
//                                         : examples::call_tool("lookup", {{"q", turn.last_user_text()}});
//       });
//
//   // 3. Offline by default, a real provider with --live (key from OPENROUTER_API_KEY only).
//   auto provider = examples::choose_provider(argc, argv, scripted);
//
// Link: neograph::core neograph::llm (the same targets provider_example_support.h needs).
// Contract notes:
//   * A sequence script is consumed in call order. With parallel provider calls the order is
//     not defined; use a responder keyed on turn.last_user_text() instead.
//   * When a sequence runs out the call fails with ErrorKind::Misuse (loud on purpose), unless
//     OnExhausted::RepeatLast was requested.
//   * No usage is reported unless a reply calls with_usage(); the offline provider does not
//     invent provider usage, cost or native replay seals.
//   * calls() records what each call saw, for assertions after the run.
#pragma once

#include "provider_example_support.h"

#include <neograph/provider.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace examples {

// One tool call inside a scripted reply. An empty id becomes call_001, call_002, ...
struct ScriptedToolCall {
    std::string id;
    std::string name;
    neograph::json arguments = neograph::json::object();
};

// What the scripted "model" answers on one call: visible text, tool calls, or a failure.
struct Reply {
    std::string text;
    std::vector<ScriptedToolCall> tool_calls;
    std::optional<sp::ErrorKind> error;      // set: the call returns an sp::Failure
    std::string error_message = "scripted failure";
    std::optional<std::pair<std::uint64_t, std::uint64_t>> usage;  // input, output tokens
    std::optional<sp::StopKind> stop;        // default: ToolUse with tool calls, else EndTurn

    Reply with_text(std::string value) const { Reply r = *this; r.text = std::move(value); return r; }
    Reply with_call(std::string name, neograph::json args = neograph::json::object(),
                    std::string id = {}) const {
        Reply r = *this;
        r.tool_calls.push_back({std::move(id), std::move(name), std::move(args)});
        return r;
    }
    Reply with_usage(std::uint64_t input_tokens, std::uint64_t output_tokens) const {
        Reply r = *this;
        r.usage = std::make_pair(input_tokens, output_tokens);
        return r;
    }
    Reply with_stop(sp::StopKind kind) const { Reply r = *this; r.stop = kind; return r; }
};

inline Reply say(std::string text) {
    Reply reply;
    reply.text = std::move(text);
    return reply;
}

// A reply that asks the caller to run one tool. Chain with_call() for several calls in one turn.
inline Reply call_tool(std::string name, neograph::json args = neograph::json::object(),
                       std::string id = {}) {
    Reply reply;
    reply.tool_calls.push_back({std::move(id), std::move(name), std::move(args)});
    return reply;
}

// A reply that fails with a typed provider error (sp::Failure), for retry and routing examples.
inline Reply fail(sp::ErrorKind kind = sp::ErrorKind::RemoteFailure,
                  std::string message = "scripted failure") {
    Reply reply;
    reply.error = kind;
    reply.error_message = std::move(message);
    return reply;
}

// Read-only view of the request the scripted model is answering.
class ScriptedTurn {
public:
    ScriptedTurn(std::size_t index, bool streaming, const std::vector<sp::Message>& history)
        : index_(index), streaming_(streaming), history_(&history) {}

    std::size_t index() const noexcept { return index_; }          // zero-based call number
    bool streaming() const noexcept { return streaming_; }
    const std::vector<sp::Message>& history() const noexcept { return *history_; }

    std::string system_text() const {
        std::string result;
        for (const auto& message : *history_)
            if (message.role == sp::Role::System || message.role == sp::Role::Developer)
                result += text_of(message);
        return result;
    }
    std::string last_user_text() const {
        for (auto it = history_->rbegin(); it != history_->rend(); ++it)
            if (it->role == sp::Role::User) return text_of(*it);
        return {};
    }
    std::size_t tool_result_count() const {
        std::size_t count = 0;
        for (const auto& message : *history_)
            for (const auto& part : message.parts)
                if (std::holds_alternative<sp::ToolResult>(part)) ++count;
        return count;
    }
    bool has_tool_result() const { return tool_result_count() != 0; }
    bool has_tool_result(std::string_view tool_use_id) const {
        for (const auto& message : *history_)
            for (const auto& part : message.parts)
                if (const auto* result = std::get_if<sp::ToolResult>(&part))
                    if (result->tool_use_id == tool_use_id) return true;
        return false;
    }
    std::string last_tool_result() const {
        std::string content;
        for (const auto& message : *history_)
            for (const auto& part : message.parts)
                if (const auto* result = std::get_if<sp::ToolResult>(&part)) content = result->content;
        return content;
    }

private:
    static std::string text_of(const sp::Message& message) {
        std::string text;
        for (const auto& part : message.parts)
            if (const auto* value = std::get_if<sp::Text>(&part)) text += value->value;
        return text;
    }
    std::size_t index_;
    bool streaming_;
    const std::vector<sp::Message>* history_;
};

// What one call saw, kept for assertions.
struct ScriptedCallRecord {
    std::size_t index = 0;
    bool streaming = false;
    std::size_t message_count = 0;
    std::size_t tool_result_count = 0;
    std::string system_text;
    std::string last_user_text;
};

class ScriptedProvider final : public neograph::Provider {
public:
    using Responder = std::function<Reply(const ScriptedTurn&)>;
    enum class OnExhausted { Fail, RepeatLast };

    // Reply N of `script` answers call N.
    explicit ScriptedProvider(std::vector<Reply> script, OnExhausted on_exhausted = OnExhausted::Fail,
                              std::string name = "scripted")
        : state_(std::make_shared<State>()), name_(std::move(name)) {
        state_->script = std::move(script);
        state_->on_exhausted = on_exhausted;
    }
    // `responder` decides every reply from what the request contains.
    explicit ScriptedProvider(Responder responder, std::string name = "scripted")
        : state_(std::make_shared<State>()), name_(std::move(name)) {
        state_->responder = std::move(responder);
    }

    std::size_t call_count() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->calls.size();
    }
    std::vector<ScriptedCallRecord> calls() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->calls;
    }

    neograph::PreparedProviderRequest prepare(neograph::ProviderRequest request) override {
        auto history = std::make_shared<const std::vector<sp::Message>>(
            examples::request_messages(request));
        auto state = state_;
        return prepare_local(client_, std::move(request),
            [state, history](const neograph::PreparedProviderRequest& prepared,
                             const std::function<void(const sp::Event&)>& observer)
                -> asio::awaitable<sp::runtime::Result> {
                co_return state->answer(
                    *history, prepared.mode() == neograph::ProviderMode::Stream, observer);
            });
    }
    std::string_view family() const noexcept override { return "openai.chat"; }
    std::string get_name() const override { return name_; }

private:
    struct State {
        std::mutex mutex;
        std::vector<Reply> script;
        OnExhausted on_exhausted = OnExhausted::Fail;
        Responder responder;
        std::vector<ScriptedCallRecord> calls;
        std::size_t next_tool_id = 0;

        sp::runtime::Result answer(const std::vector<sp::Message>& history, bool streaming,
                                   const std::function<void(const sp::Event&)>& observer) {
            std::size_t index = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                index = calls.size();
            }
            const ScriptedTurn turn(index, streaming, history);
            std::optional<Reply> reply;
            if (responder) {
                reply = responder(turn);
            } else {
                std::lock_guard<std::mutex> lock(mutex);
                if (index < script.size()) reply = script[index];
                else if (on_exhausted == OnExhausted::RepeatLast && !script.empty()) reply = script.back();
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                calls.push_back({index, streaming, history.size(), turn.tool_result_count(),
                                 turn.system_text(), turn.last_user_text()});
                if (reply)
                    for (auto& call : reply->tool_calls)
                        if (call.id.empty()) call.id = next_id();
            }
            if (!reply) reply = fail(sp::ErrorKind::Misuse,
                "scripted provider has no reply for call " + std::to_string(index + 1) +
                " (script holds " + std::to_string(script.size()) + ")");
            return reply->error ? failure(*reply, observer) : completion(*reply, streaming, observer);
        }

        std::string next_id() {
            char buffer[24];
            std::snprintf(buffer, sizeof buffer, "call_%03zu", ++next_tool_id);
            return buffer;
        }

        static std::shared_ptr<const sp::json::Document> document(const neograph::json& value) {
            auto parsed = sp::json::parse(value.dump());
            if (auto* error = std::get_if<sp::json::ParseError>(&parsed))
                throw std::invalid_argument("scripted tool arguments are not valid JSON: " + error->message);
            return std::make_shared<const sp::json::Document>(std::get<sp::json::Document>(std::move(parsed)));
        }

        static sp::runtime::Result failure(const Reply& reply,
                                           const std::function<void(const sp::Event&)>& observer) {
            sp::Failure failed;
            failed.error.kind = *reply.error;
            failed.error.safe_message = reply.error_message;
            switch (*reply.error) {
                case sp::ErrorKind::RateLimited:
                case sp::ErrorKind::Overloaded:
                    failed.error.retry_class = sp::RetryClass::Transient;
                    failed.error.retry_safety = sp::RetrySafety::RejectedBeforeOutput;
                    break;
                case sp::ErrorKind::Transport:
                case sp::ErrorKind::DeadlineExceeded:
                    failed.error.retry_class = sp::RetryClass::Transient;
                    break;
                default: break;
            }
            if (observer) {
                observer(sp::Begin{"scripted"});
                observer(sp::Fail{failed.error});
            }
            return std::make_shared<const sp::Outcome>(std::move(failed));
        }

        static sp::runtime::Result completion(const Reply& reply, bool streaming,
                                              const std::function<void(const sp::Event&)>& observer) {
            sp::Completion result;
            sp::Message message;
            message.role = sp::Role::Assistant;
            if (!reply.text.empty()) message.parts.emplace_back(sp::Text{reply.text});
            for (const auto& call : reply.tool_calls)
                message.parts.emplace_back(sp::ToolCall{
                    call.id, call.name, sp::ToolCallKind::ClientExecuted, document(call.arguments)});
            result.messages.push_back(std::move(message));
            result.stop.kind = reply.stop.value_or(
                reply.tool_calls.empty() ? sp::StopKind::EndTurn : sp::StopKind::ToolUse);
            if (reply.usage) {
                result.usage.input_total = sp::Count{reply.usage->first};
                result.usage.output_total = sp::Count{reply.usage->second};
                result.usage.total = sp::Count{reply.usage->first + reply.usage->second};
                result.usage.stage = sp::UsageStage::Final;
            }
            if (streaming) emit(result, observer);
            return std::make_shared<const sp::Outcome>(std::move(result));
        }

        // Ordered events in the runtime's shape; text arrives one word per delta.
        static void emit(const sp::Completion& result,
                         const std::function<void(const sp::Event&)>& observer) {
            if (!observer) return;
            observer(sp::Begin{"scripted"});
            std::uint32_t message_id = 0, part_id = 0;
            std::uint64_t order = 0;
            for (const auto& message : result.messages) {
                const sp::LocalId current_message{++message_id};
                observer(sp::MessageBegin{current_message, std::nullopt, message.role});
                for (const auto& part : message.parts) {
                    const sp::LocalId current_part{++part_id};
                    if (const auto* text = std::get_if<sp::Text>(&part)) {
                        observer(sp::PartBegin{current_message, current_part, sp::PartKind::Text, {}, order++});
                        std::size_t at = 0;
                        while (at < text->value.size()) {
                            const auto space = text->value.find(' ', at);
                            const auto end = space == std::string::npos ? text->value.size() : space + 1;
                            const std::string chunk = text->value.substr(at, end - at);
                            observer(sp::PartDelta{current_part, {sp::PartKind::Text, chunk}});
                            at = end;
                        }
                        observer(sp::PartSeal{current_part, std::nullopt});
                    } else if (const auto* call = std::get_if<sp::ToolCall>(&part)) {
                        sp::PartHeader header;
                        header.wire_id = call->id;
                        header.name = call->name;
                        header.tool_kind = call->kind;
                        observer(sp::PartBegin{current_message, current_part, sp::PartKind::ToolCall,
                                               std::move(header), order++});
                        const std::string arguments = call->input ? call->input->root().dump() : "{}";
                        observer(sp::PartDelta{current_part, {sp::PartKind::ToolCall, arguments}});
                        observer(sp::PartSeal{current_part, std::nullopt});
                    }
                }
                observer(sp::MessageSeal{current_message, message.wire_output});
            }
            if (result.usage.stage != sp::UsageStage::Missing) observer(sp::UsageUpdate{result.usage});
            observer(sp::Stop{result.stop});
            observer(sp::Commit{"scripted"});
        }
    };

    std::shared_ptr<State> state_;
    std::string name_;
    std::shared_ptr<sp::runtime::Client> client_ = examples::make_local_client();
};

// ---- Offline by default, real provider on request -------------------------------------------

// True when the command line has --live or NG_EXAMPLE_LIVE is 1/true.
inline bool live_requested(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--live") return true;
    const char* env = std::getenv("NG_EXAMPLE_LIVE");
    return env && (std::string_view(env) == "1" || std::string_view(env) == "true");
}

// The real provider (OpenRouter Responses). The key comes from the process environment only;
// nothing is read from a file. Throws std::runtime_error naming the variable when it is unset.
inline std::shared_ptr<neograph::Provider> make_live_provider(const char* key_variable = "OPENROUTER_API_KEY") {
    const char* key = std::getenv(key_variable);
    if (!key || !*key)
        throw std::runtime_error(std::string("--live needs the ") + key_variable +
                                 " environment variable (live calls are billed by the provider)");
    return std::shared_ptr<neograph::Provider>(make_openrouter_provider(key).release());
}

// main() helper: `scripted` unless --live is given. Prints which one is in use.
inline std::shared_ptr<neograph::Provider> choose_provider(
    int argc, char** argv, std::shared_ptr<neograph::Provider> scripted) {
    if (live_requested(argc, argv)) {
        auto live = make_live_provider();
        std::cout << "provider: live (OpenRouter, model " << openrouter_model << ")\n";
        return live;
    }
    std::cout << "provider: scripted (offline; pass --live to call a real model)\n";
    return scripted;
}

} // namespace examples
