#pragma once

#include <neograph/types.h>
#include <functional>
#include <memory>
#include <string>

namespace neograph {

using StreamCallback = std::function<void(const std::string& chunk)>;

struct CompletionParams {
    std::string model;
    std::vector<ChatMessage> messages;
    std::vector<ChatTool> tools;
    float temperature = 0.7f;
    int max_tokens = -1; // -1 = provider default
};

// Abstract provider interface
class Provider {
  public:
    virtual ~Provider() = default;
    virtual ChatCompletion complete(const CompletionParams& params) = 0;
    // Streaming: calls on_chunk per token, returns full completion when done
    virtual ChatCompletion complete_stream(const CompletionParams& params,
                                           const StreamCallback& on_chunk) = 0;
    virtual std::string get_name() const = 0;
};

} // namespace neograph
