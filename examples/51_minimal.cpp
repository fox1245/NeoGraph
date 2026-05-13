// NeoGraph Example 51: 가장 작은 동작하는 프로그램
//
// LLM 없음, mock provider 없음, tool 없음, API key 없음 — 노드 하나가
// 채널 하나의 값을 변환하고 끝나는 그래프. NeoGraph 가 어떤 모양으로
// 돌아가는지 5분 안에 이해하고 싶은 사람에게.
//
// 흐름:
//   1) 그래프를 JSON 으로 기술 — 채널 1개 + 노드 1개 + 엣지
//   2) 노드 타입을 등록
//   3) compile → run → 결과를 result.channel<T>(name) 으로 꺼냄
//
// 실행: ./example_minimal     출력: "HELLO"

#include <neograph/neograph.h>

#include <cctype>
#include <iostream>

using namespace neograph;
using namespace neograph::graph;

// 노드 한 개. 채널 "text" 를 읽어서 대문자로 바꿔 다시 같은 채널에 씀.
class UpperNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        std::string s = in.state.get("text").get<std::string>();
        for (auto& c : s) c = static_cast<char>(std::toupper(c));
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"text", json(s)});
        co_return out;
    }
    std::string get_name() const override { return "upper"; }
};

int main() {
    // (2) JSON 노드 type → 실제 GraphNode 만드는 함수 등록.
    NodeFactory::instance().register_type("upper",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<UpperNode>();
        });

    // (1) 그래프 정의 — 채널 1, 노드 1, 엣지 2 (start→upper→end).
    json def = {
        {"name", "minimal"},
        {"channels", {{"text", {{"reducer", "overwrite"}}}}},
        {"nodes",    {{"upper", {{"type", "upper"}}}}},
        {"edges", {
            {{"from", "__start__"}, {"to", "upper"}},
            {{"from", "upper"},     {"to", "__end__"}},
        }},
    };

    // (3) 컴파일 → 실행 → 결과 꺼냄.
    NodeContext ctx;
    auto engine = GraphEngine::compile(def, ctx);

    RunConfig cfg;
    cfg.input = {{"text", "hello"}};
    auto result = engine->run(cfg);

    std::cout << result.channel<std::string>("text") << "\n";   // → HELLO
    return 0;
}
