// NeoGraph Example 04: Checkpointing + Human-in-the-Loop (HITL)
//
// 그래프 실행 중 특정 노드 앞에서 중단(interrupt)하고,
// 사용자 승인 후 재개(resume)하는 HITL 워크플로 예제.
//
// 시나리오: 주문 처리 에이전트
//   1. LLM이 주문 내용을 분석
//   2. 결제 실행 전 사용자 승인 요청 (interrupt_before)
//   3. 사용자가 승인하면 결제 실행 후 완료
//
// API 키 불필요 (Mock Provider 사용)
//
// Usage: ./example_checkpoint_hitl

#include <neograph/neograph.h>
#include <neograph/graph/react_graph.h>

#include <iostream>
#include <string>

// Mock Provider: 단계별로 다른 응답 반환
class OrderProvider : public neograph::Provider {
    int call_count_ = 0;
public:
    neograph::ChatCompletion complete(const neograph::CompletionParams&) override {
        neograph::ChatCompletion result;
        result.message.role = "assistant";

        if (call_count_++ == 0) {
            // 1단계: 도구 호출 (주문 분석)
            result.message.tool_calls = {{
                "call_001", "analyze_order",
                R"({"item": "MacBook Pro", "quantity": 1, "price": 2500000})"
            }};
        } else {
            // 2단계: 최종 응답
            result.message.content =
                "주문이 확인되었습니다.\n"
                "- 상품: MacBook Pro\n"
                "- 수량: 1\n"
                "- 금액: 2,500,000원\n"
                "결제가 완료되었습니다. 감사합니다!";
        }
        return result;
    }

    neograph::ChatCompletion complete_stream(
        const neograph::CompletionParams& p, const neograph::StreamCallback& cb) override {
        auto r = complete(p);
        if (cb && !r.message.content.empty()) cb(r.message.content);
        return r;
    }

    std::string get_name() const override { return "order_mock"; }
};

// 주문 분석 도구
class AnalyzeOrderTool : public neograph::Tool {
public:
    neograph::ChatTool get_definition() const override {
        return {"analyze_order", "Analyze an order and return confirmation details",
                neograph::json{{"type", "object"}}};
    }
    std::string execute(const neograph::json& args) override {
        return R"({"status": "confirmed", "item": "MacBook Pro", "total": 2500000, "currency": "KRW"})";
    }
    std::string get_name() const override { return "analyze_order"; }
};

int main() {
    auto provider = std::make_shared<OrderProvider>();

    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<AnalyzeOrderTool>());

    // JSON으로 그래프 정의 — tools 노드 앞에서 interrupt
    neograph::json definition = {
        {"name", "order_workflow"},
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
        })},
        // 핵심: tools 실행 전에 중단
        {"interrupt_before", neograph::json::array({"tools"})}
    };

    // Tool 포인터 준비
    std::vector<neograph::Tool*> tool_ptrs;
    for (auto& t : tools) tool_ptrs.push_back(t.get());

    neograph::graph::NodeContext ctx;
    ctx.provider = provider;
    ctx.tools = tool_ptrs;

    // 체크포인트 스토어 (인메모리)
    auto store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    auto engine = neograph::graph::GraphEngine::compile(definition, ctx, store);
    engine->own_tools(std::move(tools));

    // === 1차 실행: 중단까지 ===
    std::cout << "=== Phase 1: 주문 분석 후 승인 대기 ===\n\n";

    neograph::graph::RunConfig config;
    config.thread_id = "order-001";
    config.input = {{"messages", neograph::json::array({
        {{"role", "user"}, {"content", "MacBook Pro 1대 주문해줘"}}
    })}};

    auto result = engine->run(config);

    if (result.interrupted) {
        std::cout << "중단됨! 노드: " << result.interrupt_node << "\n";
        std::cout << "체크포인트 ID: " << result.checkpoint_id << "\n";
        std::cout << "실행 추적: ";
        for (const auto& n : result.execution_trace) std::cout << n << " → ";
        std::cout << "PAUSED\n\n";

        // 사용자에게 승인 요청 시뮬레이션
        std::cout << ">>> 결제를 진행하시겠습니까? (시뮬레이션: 승인) <<<\n\n";
    }

    // === 2차 실행: 승인 후 재개 ===
    std::cout << "=== Phase 2: 승인 후 재개 ===\n\n";

    auto resumed = engine->resume(
        "order-001",
        neograph::json("승인합니다")
    );

    std::cout << "실행 추적: ";
    for (const auto& n : resumed.execution_trace) std::cout << n << " → ";
    std::cout << "END\n\n";

    if (resumed.output.contains("final_response")) {
        std::cout << "최종 응답:\n" << resumed.output["final_response"].get<std::string>() << "\n";
    }

    // 체크포인트 히스토리
    auto checkpoints = store->list("order-001");
    std::cout << "\n=== 체크포인트 히스토리 (" << checkpoints.size() << "개) ===\n";
    for (const auto& cp : checkpoints) {
        std::cout << "  [" << cp.interrupt_phase << "] step=" << cp.step
                  << " node=" << cp.current_node << " → " << cp.next_node << "\n";
    }

    return 0;
}
