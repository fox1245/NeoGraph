#include <neograph/json.h>
#include <sp/config_defaults.h>
#include <iostream>
#include <string_view>

// `policy ORIGIN` prints the embedded descriptor policy with ORIGIN declared an OpenRouter origin;
// `catalogue` prints the model catalogue of the synthetic fixture models.
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string_view mode(argv[1]);
    if (mode == "catalogue" && argc == 2) {
        // Explicit facts for these isolated synthetic fixtures, not public models.
        auto models = neograph::json::array();
        for (const auto model : {"program-chat-mock", "fixture-model"})
            models.push_back(neograph::json{{"id", model}, {"families", neograph::json::array({"openai.chat"})},
                                            {"input_limit", 4096}, {"output_limit", 65536}});
        std::cout << neograph::json{{"version", 1}, {"models", std::move(models)}}.dump() << '\n';
        return 0;
    }
    if (mode != "policy" || argc != 3) return 2;
    const std::string_view origin(argv[2]);
    if (!origin.empty() && !origin.starts_with("http://127.0.0.1:") &&
        !origin.starts_with("http://localhost:")) return 2;
    auto policy = neograph::json::parse(sp::config_defaults::descriptor_policy_json);
    for (auto family : policy.at("families"))
        if (family.at("family") == "openai.chat" && !origin.empty())
            family.at("openrouter_origins").push_back(neograph::json(origin));
    std::cout << policy.dump() << '\n';
}
