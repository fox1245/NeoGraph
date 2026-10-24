// jarvis/src/memory/conversation_store.h
//
// Two graph nodes: "memory_lookup" and "memory_commit".
//
// memory_lookup: at the start of each turn, reads the last N turns and user
//   preferences from the NeoGraph Store and writes them to memory_context for the router and synthesizer.
//
// memory_commit: at the end of each turn, appends (user_text, final_text, tools_used).
//
// User preferences ("tony.prefers.language", "tony.last_topic") live in the
// same namespace and guide the synthesizer's response tone.
//
// Registration (main.cpp): registers "memory_lookup" and "memory_commit".
#pragma once

#include <neograph/neograph.h>

namespace jarvis::memory {

class MemoryLookupNode : public neograph::graph::GraphNode {
  public:
    MemoryLookupNode(std::string name, const neograph::json& cfg);

    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override;

    std::string get_name() const override { return name_; }

  private:
    std::string name_;
    std::string namespace_;
    int recent_turns_;
};

class MemoryCommitNode : public neograph::graph::GraphNode {
  public:
    MemoryCommitNode(std::string name, const neograph::json& cfg);

    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override;

    std::string get_name() const override { return name_; }

  private:
    std::string name_;
    std::string namespace_;
};

}  // namespace jarvis::memory
