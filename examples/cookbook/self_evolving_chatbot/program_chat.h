#pragma once

#include <neograph/provider.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace evolving_chat {
using neograph::json;

struct Options {
    std::string database = "evolving-chat.sqlite";
    std::string postgres_url;
    /// Host-only protected archive root; empty selects database + ".native".
    std::string native_archive_directory;
    std::string session = "demo";
    std::string model;
    std::string api_key;
    std::string base_url                 = "https://openrouter.ai/api/v1";
    /// Host-owned descriptor policy file; empty selects the built-in SDK policy.
    std::string descriptor_policy_file;
    bool        mock                     = true;
    bool        allow_loopback           = false;
    unsigned    max_turns                = 12;
    unsigned    max_calls                = 100;
    unsigned    max_tokens               = 200000;
    std::uint64_t max_output_tokens       = 4096;
    unsigned    provider_timeout_seconds = 120;
    std::string reasoning_effort;
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
