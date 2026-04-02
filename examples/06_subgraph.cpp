// NeoGraph Example 06: Subgraph (계층적 그래프 합성)
//
// 서브그래프 노드를 사용하여 복잡한 워크플로를 계층적으로 구성하는 예제.
// 코드 변경 없이 JSON만으로 에이전트를 합성할 수 있습니다.
//
// 시나리오: Supervisor 패턴
//   메인 그래프: supervisor → inner_react_agent(서브그래프) → __end__
//   서브그래프 : llm → tools → llm (ReAct 루프)
//
// API 키 불필요 (Mock Provider 사용)
//
// Usage: ./example_subgraph

#include <neograph/neograph.h>

#include <iostream>

// Mock Provider
class SubgraphMockProvider : public neograph::Provider {
    int call_count_ = 0;
public:
    neograph::ChatCompletion complete(const neograph::CompletionParams&) override {
        neograph::ChatCompletion result;
        result.message.role = "assistant";

        if (call_count_++ == 0) {
            result.message.tool_calls = {{
                "call_sub_001", "lookup",
                R"({"query": "NeoGraph features"})"
            }};
        } else {
            result.message.content = "NeoGraph는 C++로 작성된 그래프 에이전트 엔진입니다. "
                "체크포인팅, HITL, 병렬 실행, 서브그래프를 지원합니다.";
        }
        return result;
    }

    neograph::ChatCompletion complete_stream(
        const neograph::CompletionParams& p, const neograph::StreamCallback& cb) override {
        auto r = complete(p);
        if (cb && !r.message.content.empty()) cb(r.message.content);
        return r;
    }
    std::string get_name() const override { return "subgraph_mock"; }
};

// Mock 도구
class LookupTool : public neograph::Tool {
public:
    neograph::ChatTool get_definition() const override {
        return {"lookup", "Look up information", neograph::json{{"type", "object"}}};
    }
    std::string execute(const neograph::json&) override {
        return R"({"result": "NeoGraph: C++ graph agent engine with checkpointing, HITL, parallel fan-out, subgraph composition."})";
    }
    std::string get_name() const override { return "lookup"; }
};

int main() {
    auto provider = std::make_shared<SubgraphMockProvider>();

    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<LookupTool>());

    std::vector<neograph::Tool*> tool_ptrs;
    for (auto& t : tools) tool_ptrs.push_back(t.get());

    neograph::graph::NodeContext ctx;
    ctx.provider = provider;
    ctx.tools = tool_ptrs;

    // JSON 기반 그래프 정의 — 서브그래프를 인라인으로 포함
    neograph::json definition = {
        {"name", "supervisor_graph"},
        {"channels", {
            {"messages", {{"reducer", "append"}}}
        }},
        {"nodes", {
            // 서브그래프 노드: 내부에 ReAct 루프를 포함
            {"inner_agent", {
                {"type", "subgraph"},
                {"definition", {
                    {"name", "inner_react"},
                    {"channels", {
                        {"messages", {{"reducer", "append"}}}
                    }},
                    {"nodes", {
                        {"llm",   {{"type", "llm_call"}}},
                        {"tools", {{"type", "tool_dispatch"}}}
                    }},
                    {"edges", neograph::json::array({
                        {{"from", "__start__"}, {"to", "llm"}},
                        {{"from", "llm"}, {"condition", "has_tool_calls"},
                         {"routes", {{"true", "tools"}, {"false", "__end__"}}}},
                        {{"from", "tools"}, {"to", "llm"}}
                    })}
                }}
                // input_map/output_map 생략 → 동명 채널 자동 매핑
            }}
        }},
        {"edges", neograph::json::array({
            {{"from", "__start__"}, {"to", "inner_agent"}},
            {{"from", "inner_agent"}, {"to", "__end__"}}
        })}
    };

    auto engine = neograph::graph::GraphEngine::compile(definition, ctx);
    engine->own_tools(std::move(tools));

    // 실행
    std::cout << "=== Subgraph (Supervisor 패턴) ===\n\n";

    neograph::graph::RunConfig config;
    config.input = {{"messages", neograph::json::array({
        {{"role", "user"}, {"content", "NeoGraph가 뭐야?"}}
    })}};

    auto result = engine->run_stream(config,
        [](const neograph::graph::GraphEvent& event) {
            switch (event.type) {
                case neograph::graph::GraphEvent::Type::NODE_START:
                    std::cout << "[start] " << event.node_name << "\n";
                    break;
                case neograph::graph::GraphEvent::Type::NODE_END:
                    std::cout << "[done]  " << event.node_name << "\n";
                    break;
                case neograph::graph::GraphEvent::Type::LLM_TOKEN:
                    std::cout << event.data.get<std::string>() << std::flush;
                    break;
                default:
                    break;
            }
        });

    std::cout << "\n\n실행 추적 (외부 그래프): ";
    for (const auto& n : result.execution_trace) std::cout << n << " → ";
    std::cout << "END\n";

    if (result.output.contains("final_response")) {
        std::cout << "\n최종 응답: " << result.output["final_response"].get<std::string>() << "\n";
    }

    return 0;
}
