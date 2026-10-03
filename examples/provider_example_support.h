#pragma once

#include <neograph/llm/schema_provider.h>
#include <json/json.h>
#include <core/request_controls.h>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace examples {
inline constexpr const char* openrouter_model = "~deepseek/deepseek-v4-flash-latest";

// Actual closed/versioned SDK admission, not an editable request-body override.
inline sp::descriptor::ValidatedDescriptor admitted_descriptor(
    std::string base_url, std::string family, std::string path, std::string id,
    sp::descriptor::PolicySnapshot policy = {}) {
    if (family == "responses") family = "openai.responses";
    if (family == "chat") family = "openai.chat";
    if (family != "openai.responses" && family != "openai.chat")
        throw std::invalid_argument("example descriptor requires Chat or Responses");
    const auto source = neograph::json{
        {"descriptor_version", 1}, {"revision", 1}, {"id", std::move(id)},
        {"family", family},
        {"connection", {{"base_url", std::move(base_url)},
                        {"paths", {{"buffered", path}, {"streaming", path}}}}}
    }.dump();
    auto loaded = policy ? sp::descriptor::load(source, std::move(policy))
                         : sp::descriptor::load(source);
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&loaded))
        throw std::invalid_argument("example descriptor rejected at " + error->pointer + ": " + error->message);
    return std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded));
}

inline sp::descriptor::ValidatedDescriptor openrouter_descriptor(std::string family = "responses") {
    const bool chat = family == "chat" || family == "openai.chat";
    return admitted_descriptor("https://openrouter.ai", std::move(family),
        chat ? "/api/v1/chat/completions" : "/api/v1/responses",
        chat ? "neograph-example-openrouter-chat" : "neograph-example-openrouter-responses");
}

inline std::unique_ptr<neograph::llm::SchemaProvider> make_openrouter_provider(
    std::string api_key, std::string family = "responses",
    std::chrono::milliseconds timeout = std::chrono::seconds(120),
    std::optional<sp::OpenRouterRouting> routing = std::nullopt) {
    sp::runtime::Options options;
    options.api_key = std::move(api_key);
    options.default_timeout = timeout;
    if (!routing) routing.emplace();
    if (!routing->zdr) routing->zdr = true;
    neograph::llm::SchemaProvider::Defaults defaults;
    defaults.provider = std::move(routing);
    if (family == "responses" || family == "openai.responses")
        defaults.responses_store = false;
    return neograph::llm::SchemaProvider::create(openrouter_descriptor(std::move(family)),
                                                std::move(options), std::move(defaults));
}

// This client genuinely admits and encodes local fixture requests. prepare_local
// dispatches the owned fixture callback instead of starting network transport.
inline std::shared_ptr<sp::runtime::Client> make_local_client() {
    return std::make_shared<sp::runtime::Client>(admitted_descriptor(
        "http://127.0.0.1:18080", "openai.chat", "/v1/chat/completions", "neograph-example-local"));
}

inline sp::Message message(sp::Role role, std::string text) {
    sp::Message value;
    value.role = role;
    value.parts.emplace_back(sp::Text{std::move(text)});
    return value;
}

inline sp::runtime::Result require_outcome(sp::runtime::Result result) {
    return neograph::outcome_or_throw(std::move(result));
}
inline std::string visible_text(const sp::Outcome& outcome) {
    return neograph::outcome_text(outcome);
}
inline const std::vector<sp::Message>& request_messages(const neograph::ProviderRequest& request) {
    return std::visit([](const auto& payload) -> const std::vector<sp::Message>& {
        if constexpr (std::is_same_v<std::decay_t<decltype(payload)>, sp::chat::Request>) {
            if (!payload.messages.empty())
                throw std::invalid_argument("local fixture requires full canonical message history");
            return payload.canonical_messages;
        } else {
            return payload.messages;
        }
    }, request.payload);
}

// Synthetic fixtures emit the same typed, ordered observer contract as the
// runtime. They do not invent provider usage, native replay seals or authority.
inline void emit_local_events(const sp::Completion& completion,
                              const std::function<void(const sp::Event&)>& on_event) {
    if (!on_event) return;
    for (const auto& value : completion.messages)
        for (const auto& part : value.parts)
            if (!std::holds_alternative<sp::Text>(part) && !std::holds_alternative<sp::ToolCall>(part))
                throw std::invalid_argument("local event fixture supports Text and ToolCall parts only");
    on_event(sp::Begin{"local-fixture"});
    std::uint32_t message_id = 0, part_id = 0;
    std::uint64_t order = 0;
    for (const auto& value : completion.messages) {
        const sp::LocalId current_message{++message_id};
        on_event(sp::MessageBegin{current_message, value.id.empty() ? std::nullopt : std::optional<std::string>(value.id), value.role});
        for (const auto& part : value.parts) {
            const sp::LocalId current_part{++part_id};
            if (const auto* text = std::get_if<sp::Text>(&part)) {
                on_event(sp::PartBegin{current_message, current_part, sp::PartKind::Text, {}, order++});
                on_event(sp::PartDelta{current_part, {sp::PartKind::Text, text->value}});
                on_event(sp::PartSeal{current_part, std::nullopt});
            } else {
                const auto& call = std::get<sp::ToolCall>(part);
                sp::PartHeader header;
                header.wire_id = call.id;
                header.name = call.name;
                header.tool_kind = call.kind;
                header.wire_type = call.wire_type;
                header.wire_metadata = call.wire_metadata;
                on_event(sp::PartBegin{current_message, current_part, sp::PartKind::ToolCall, std::move(header), order++});
                const auto arguments = call.input ? call.input->root().dump() : "{}";
                on_event(sp::PartDelta{current_part, {sp::PartKind::ToolCall, arguments}});
                on_event(sp::PartSeal{current_part, std::nullopt, call.wire_metadata});
            }
        }
        on_event(sp::MessageSeal{current_message, value.wire_output});
    }
    if (completion.usage.stage != sp::UsageStage::Missing)
        on_event(sp::UsageUpdate{completion.usage});
    on_event(sp::Stop{completion.stop});
    on_event(sp::Commit{"local-fixture"});
}
} // namespace examples
