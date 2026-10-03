#include <neograph/json.h>
#include <sp/config_defaults.h>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string_view origin(argv[1]);
    if (!origin.empty() && !origin.starts_with("http://127.0.0.1:") &&
        !origin.starts_with("http://localhost:")) return 2;
    auto policy = neograph::json::parse(sp::config_defaults::descriptor_policy_json);
    neograph::json defaults;
    for (auto family : policy.at("families")) {
        if (family.at("family") != "openai.chat") continue;
        defaults = family.at("defaults");
        if (!origin.empty()) family.at("openrouter_origins").push_back(neograph::json(origin));
    }
    defaults["max_output_tokens"] = nullptr;
    // Explicit facts for these isolated synthetic fixtures, not public models.
    for (const auto model : {"program-chat-mock", "fixture-model"})
        policy.at("models").push_back(neograph::json{{"family", "openai.chat"},
            {"model", model}, {"defaults", defaults},
            {"input_limit", 4096}, {"output_limit", 65536}});
    std::cout << policy.dump() << '\n';
}
