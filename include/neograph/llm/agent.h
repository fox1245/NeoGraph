#pragma once

#include <neograph/provider.h>
#include <neograph/tool.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace neograph::llm {

class Agent {
  public:
    Agent(std::shared_ptr<Provider> provider,
          std::vector<std::unique_ptr<Tool>> tools,
          const std::string& instructions = "",
          const std::string& model = "");

    // Run the agent loop: LLM generates -> tool calls -> feed results -> repeat
    // Returns the final text response
    std::string run(std::vector<ChatMessage>& messages,
                    int max_iterations = 10);

    // Streaming variant: streams final response tokens via callback
    std::string run_stream(std::vector<ChatMessage>& messages,
                           const StreamCallback& on_chunk,
                           int max_iterations = 10);

    // Single completion (no tool loop)
    ChatCompletion complete(const std::vector<ChatMessage>& messages);

  private:
    std::shared_ptr<Provider> provider_;
    std::vector<std::unique_ptr<Tool>> tools_;
    std::string instructions_;
    std::string model_;

    void ensure_system_message(std::vector<ChatMessage>& messages);
    std::vector<ChatTool> get_tool_definitions() const;
};

} // namespace neograph::llm
