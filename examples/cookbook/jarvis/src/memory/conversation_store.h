// jarvis/src/memory/conversation_store.h
//
// "memory_lookup" + "memory_commit" 두 개의 그래프 노드.
//
// memory_lookup: 매 턴 시작 시 NeoGraph Store 에서 최근 N 턴 + 사용자 선호 끌어와
//   memory_context 채널에 넣음. 라우터/합성기가 둘 다 본다.
//
// memory_commit: 매 턴 끝에 (user_text, final_text, 사용된 도구 이름들) 을 push.
//
// 사용 선호 ("tony.prefers.language", "tony.last_topic") 도 같은 namespace 에 두고
// 합성기가 응답 어조 결정에 사용.
//
// 등록 (main.cpp): "memory_lookup", "memory_commit" 두 타입.
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
