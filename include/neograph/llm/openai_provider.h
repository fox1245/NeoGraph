#pragma once

#include <neograph/provider.h>
#include <memory>
#include <string>

namespace neograph::llm {

class OpenAIProvider : public Provider {
  public:
    struct Config {
        std::string api_key;
        std::string base_url = "https://api.openai.com";
        std::string default_model = "gpt-4o-mini";
        int timeout_seconds = 60;
    };

    static std::unique_ptr<OpenAIProvider> create(const Config& config);

    ChatCompletion complete(const CompletionParams& params) override;
    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override;
    std::string get_name() const override { return "openai"; }

  private:
    explicit OpenAIProvider(Config config);
    json build_body(const CompletionParams& params) const;
    Config config_;
};

} // namespace neograph::llm
