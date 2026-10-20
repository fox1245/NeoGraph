#pragma once
// A SchemaProvider whose endpoint calls reach a loopback peer: the built-in policy, with every list that names a
// provider origin also naming the peer's.

#include "typed_wire_peer.h"
#include <string>
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
}  // namespace neograph::test::endpoints
