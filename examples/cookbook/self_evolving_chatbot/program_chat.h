#pragma once

#include <neograph/provider.h>

#include <functional>
#include <memory>
#include <string>

namespace evolving_chat {
using neograph::json;

struct Options {
    std::string database = "evolving-chat.sqlite";
    std::string postgres_url;
    std::string session = "demo";
    std::string model;
    std::string api_key;
    std::string base_url          = "https://openrouter.ai/api/v1";
    bool        mock              = true;
    bool        allow_loopback    = false;
    unsigned    max_turns         = 12;
    unsigned    max_calls         = 100;
    unsigned    max_tokens        = 200000;
    unsigned    max_output_tokens = 2048;
    /// Trusted host guidance; empty selects the packaged skill and chat-mode reference.
    std::string authoring_guidance;
};

// One process hosts both tenants. Each tenant has a separate registry capture,
// materialization cache and Runtime; durable stores enforce the owner boundary.
class Chat final {
public:
    explicit Chat(Options options);
    ~Chat();
    json state(const std::string& tenant) const;
    json turn(const std::string& tenant,
              const std::string& request_id,
              const std::string& message,
              bool               force_swap = false);
    void cancel(const std::string& tenant);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace evolving_chat
