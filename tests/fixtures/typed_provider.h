#pragma once

#include <neograph/provider.h>
#include <neograph/llm/schema_provider.h>
#include <descriptor/descriptor.h>
#include <descriptor/policy.h>
#include <sp/config_defaults.h>
#include <json/json.h>
#include <exception>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <utility>

namespace neograph::test {

// True when `actual` is the exception a test installed as `expected`.
// libstdc++ and libc++ keep one exception object across rethrow_exception and
// current_exception, so pointer equality is the strict check there. MSVC's
// current_exception() hands back a fresh copy of the object, so a cause that
// travelled through a catch block never compares equal to the original pointer;
// there it can only be matched by dynamic type and message.
inline bool same_exception(const std::exception_ptr& actual, const std::exception_ptr& expected) {
    if (actual == expected) return true;
#if defined(_MSC_VER)
    if (!actual || !expected) return false;
    const auto describe = [](const std::exception_ptr& pointer)
        -> std::pair<std::type_index, std::string> {
        try { std::rethrow_exception(pointer); }
        catch (const std::exception& error) { return {std::type_index(typeid(error)), error.what()}; }
        catch (...) { return {std::type_index(typeid(void)), std::string()}; }
    };
    const auto left = describe(actual);
    return left.first != std::type_index(typeid(void)) && left == describe(expected);
#else
    return false;
#endif
}

inline sp::descriptor::ValidatedDescriptor descriptor(
    std::string family = "openai.chat", std::string origin = "https://fixture.invalid",
    sp::descriptor::PolicySnapshot policy = sp::descriptor::builtin_policy()) {
    std::string path = "/v1/chat/completions";
    if (family == "anthropic.messages") path = "/v1/messages";
    else if (family == "openai.responses") path = "/v1/responses";
    else if (family == "google.generate") path = "/v1beta/models/fixture-model:generateContent";
    else if (family == "google.interactions") path = "/v1beta/interactions";
    const auto streaming = family == "google.generate"
        ? "/v1beta/models/fixture-model:streamGenerateContent?alt=sse" : path;
    const auto source = neograph::json{{"descriptor_version", 1}, {"revision", 1},
        {"id", "neograph-test"}, {"family", family},
        {"connection", {{"base_url", origin},
            {"paths", {{"buffered", path}, {"streaming", streaming}}}}}}.dump();
    auto admitted = sp::descriptor::load(source, std::move(policy));
    if (auto* error = std::get_if<sp::descriptor::ConfigError>(&admitted))
        throw std::invalid_argument(error->pointer + ": " + error->message);
    return std::get<sp::descriptor::ValidatedDescriptor>(std::move(admitted));
}

inline std::shared_ptr<sp::runtime::Client> client() {
    return std::make_shared<sp::runtime::Client>(descriptor());
}

inline std::shared_ptr<sp::runtime::Client> bounded_client(
    std::string model = "test-model", std::uint64_t input_limit = 1) {
    auto source = neograph::json::parse(sp::config_defaults::descriptor_policy_json);
    neograph::json defaults;
    for (const auto& family : source.at("families")) {
        if (family.at("family") == "openai.chat") {
            defaults = family.at("defaults");
            break;
        }
    }
    const neograph::json row{{"family", "openai.chat"}, {"model", model},
        {"defaults", std::move(defaults)}, {"output_limit", nullptr}, {"input_limit", input_limit}};
    bool replaced = false;
    for (auto admitted : source.at("models")) {
        if (admitted.at("family") == "openai.chat" && admitted.at("model") == model) {
            admitted = row;
            replaced = true;
            break;
        }
    }
    if (!replaced) source.at("models").push_back(row);
    auto admitted = sp::descriptor::load_policy(
        source.dump(), sp::config_defaults::codec_defaults_json);
    if (auto* error = std::get_if<sp::descriptor::ConfigError>(&admitted))
        throw std::invalid_argument(error->pointer + ": " + error->message);
    return std::make_shared<sp::runtime::Client>(descriptor(
        "openai.chat", "https://fixture.invalid",
        std::get<sp::descriptor::PolicySnapshot>(std::move(admitted))));
}

inline sp::Message message(std::string text, sp::Role role = sp::Role::Assistant) {
    sp::Message result;
    result.role = role;
    result.parts.emplace_back(sp::Text{std::move(text)});
    return result;
}

inline sp::Usage usage(std::optional<std::uint64_t> input,
                       std::optional<std::uint64_t> output,
                       std::optional<std::uint64_t> total,
                       sp::UsageStage stage = sp::UsageStage::Final) {
    sp::Usage result;
    if (input) result.input_total = sp::Count{*input};
    if (output) result.output_total = sp::Count{*output};
    if (total) result.total = sp::Count{*total};
    result.stage = stage;
    return result;
}

inline sp::runtime::Result success(std::vector<sp::Message> messages, sp::Usage usage = {}) {
    sp::Completion result;
    result.messages = std::move(messages);
    result.usage = std::move(usage);
    result.stop = {sp::StopKind::EndTurn, "stop"};
    return std::make_shared<const sp::Outcome>(std::move(result));
}
inline sp::runtime::Result success(std::string text, sp::Usage usage = {}) {
    return success(std::vector<sp::Message>{message(std::move(text))}, std::move(usage));
}
inline sp::runtime::Result failure(sp::ErrorKind kind, sp::PartialCompletion partial = {}) {
    sp::Failure result;
    result.error.kind = kind;
    result.partial = std::move(partial);
    return std::make_shared<const sp::Outcome>(std::move(result));
}
inline const sp::Completion& completion(const sp::runtime::Result& result) {
    if (!result) throw std::logic_error("missing provider outcome");
    return std::get<sp::Completion>(*result);
}
inline std::string text(const sp::runtime::Result& result) {
    std::string value;
    for (const auto& message : completion(result).messages)
        for (const auto& part : message.parts)
            if (const auto* text = std::get_if<sp::Text>(&part)) value += text->value;
    return value;
}
inline std::shared_ptr<const sp::json::Document> document(std::string_view source) {
    auto parsed = sp::json::parse(source);
    if (auto* error = std::get_if<sp::json::ParseError>(&parsed))
        throw std::invalid_argument(error->message);
    return std::make_shared<const sp::json::Document>(
        std::get<sp::json::Document>(std::move(parsed)));
}
inline ProviderRequest request(std::string model = "fixture-model", std::string text = "hello",
                               ProviderMode mode = ProviderMode::Collect) {
    sp::chat::Request payload;
    payload.model = std::move(model);
    payload.canonical_messages.push_back(message(std::move(text), sp::Role::User));
    ProviderRequest result;
    result.payload = std::move(payload);
    result.mode = mode;
    return result;
}

// The handler and request are owned by the prepared operation. Fixtures pass a
// handler capturing shared state by value; destruction of the Provider is safe.
class LocalProvider : public Provider {
public:
    using EventCallback = std::function<void(const sp::Event&)>;
    using Handler = std::function<asio::awaitable<sp::runtime::Result>(
        ProviderRequest, const PreparedProviderRequest&, const EventCallback&)>;
    explicit LocalProvider(Handler handler, std::string name = "fixture-provider",
                           std::shared_ptr<sp::runtime::Client> admitted_client = client())
        : client_(std::move(admitted_client)), handler_(std::move(handler)), name_(std::move(name)) {}
    std::string get_name() const override { return name_; }
    std::string_view family() const noexcept override { return "openai.chat"; }
    PreparedProviderRequest prepare(ProviderRequest request) override {
        auto owned = request;
        return prepare_local(client_, std::move(request),
            [handler = handler_, owned = std::move(owned)](
                const PreparedProviderRequest& prepared, const EventCallback& on_event) {
                return handler(owned, prepared, on_event);
            });
    }
private:
    std::shared_ptr<sp::runtime::Client> client_;
    Handler handler_;
    std::string name_;
};

} // namespace neograph::test
