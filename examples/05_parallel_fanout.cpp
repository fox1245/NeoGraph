// NeoGraph Example 05: Parallel Fan-out / Fan-in
//
// 여러 노드를 Taskflow 기반으로 병렬 실행하고,
// 모든 결과를 수집(fan-in)한 후 다음 단계로 진행하는 예제.
//
// 시나리오: 병렬 리서치 에이전트
//   __start__ → [researcher_a, researcher_b, researcher_c] → summarizer → __end__
//   3명의 리서처가 동시에 작업하고, 결과를 요약기가 종합.
//
// API 키 불필요 (커스텀 노드 사용)
//
// Usage: ./example_parallel_fanout

#include <neograph/neograph.h>

#include <iostream>
#include <thread>
#include <chrono>

// 커스텀 노드: 리서처 (시뮬레이션 — 100ms 지연)
class ResearcherNode : public neograph::graph::GraphNode {
    std::string name_;
    std::string topic_;
    int delay_ms_;

public:
    ResearcherNode(const std::string& name, const std::string& topic, int delay_ms)
        : name_(name), topic_(topic), delay_ms_(delay_ms) {}

    std::vector<neograph::graph::ChannelWrite> execute(
        const neograph::graph::GraphState& state) override {

        auto start = std::chrono::steady_clock::now();

        // 실제로는 LLM 호출이나 웹 검색을 할 자리
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        neograph::json result = {
            {"researcher", name_},
            {"topic", topic_},
            {"finding", topic_ + "에 대한 조사 결과입니다. (" + std::to_string(elapsed) + "ms)"},
            {"elapsed_ms", elapsed}
        };

        // "findings" 채널에 결과를 append
        return {neograph::graph::ChannelWrite{"findings", neograph::json::array({result})}};
    }

    std::string name() const override { return name_; }
};

// 커스텀 노드: 요약기
class SummarizerNode : public neograph::graph::GraphNode {
public:
    std::vector<neograph::graph::ChannelWrite> execute(
        const neograph::graph::GraphState& state) override {

        auto findings = state.get("findings");
        std::string summary = "=== 리서치 요약 ===\n";

        if (findings.is_array()) {
            for (const auto& f : findings) {
                summary += "- [" + f.value("researcher", "?") + "] "
                        + f.value("finding", "") + "\n";
            }
            summary += "\n총 " + std::to_string(findings.size()) + "건의 조사 결과 수집 완료.";
        }

        return {neograph::graph::ChannelWrite{"summary", neograph::json(summary)}};
    }

    std::string name() const override { return "summarizer"; }
};

int main() {
    // 커스텀 노드 타입 등록
    auto& factory = neograph::graph::NodeFactory::instance();

    factory.register_type("researcher",
        [](const std::string& name, const neograph::json& config,
           const neograph::graph::NodeContext&) -> std::unique_ptr<neograph::graph::GraphNode> {
            return std::make_unique<ResearcherNode>(
                name,
                config.value("topic", "unknown"),
                config.value("delay_ms", 100)
            );
        });

    factory.register_type("summarizer",
        [](const std::string& name, const neograph::json&,
           const neograph::graph::NodeContext&) -> std::unique_ptr<neograph::graph::GraphNode> {
            return std::make_unique<SummarizerNode>();
        });

    // JSON 기반 그래프 정의
    neograph::json definition = {
        {"name", "parallel_research"},
        {"channels", {
            {"findings", {{"reducer", "append"}}},
            {"summary",  {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"researcher_a", {{"type", "researcher"}, {"topic", "AI 반도체 시장"}, {"delay_ms", 150}}},
            {"researcher_b", {{"type", "researcher"}, {"topic", "양자 컴퓨팅 동향"}, {"delay_ms", 100}}},
            {"researcher_c", {{"type", "researcher"}, {"topic", "자율주행 기술 현황"}, {"delay_ms", 120}}},
            {"summarizer",   {{"type", "summarizer"}}}
        }},
        {"edges", neograph::json::array({
            // fan-out: __start__ → 3개 리서처 (병렬 실행)
            {{"from", "__start__"}, {"to", "researcher_a"}},
            {{"from", "__start__"}, {"to", "researcher_b"}},
            {{"from", "__start__"}, {"to", "researcher_c"}},
            // fan-in: 3개 리서처 → 요약기 (모두 완료 후)
            {{"from", "researcher_a"}, {"to", "summarizer"}},
            {{"from", "researcher_b"}, {"to", "summarizer"}},
            {{"from", "researcher_c"}, {"to", "summarizer"}},
            // 요약 완료
            {{"from", "summarizer"}, {"to", "__end__"}}
        })}
    };

    neograph::graph::NodeContext ctx;  // 커스텀 노드라 Provider/Tool 불필요
    auto engine = neograph::graph::GraphEngine::compile(definition, ctx);

    // 실행
    std::cout << "=== Parallel Fan-out / Fan-in ===\n\n";

    auto total_start = std::chrono::steady_clock::now();

    neograph::graph::RunConfig config;
    auto result = engine->run_stream(config,
        [](const neograph::graph::GraphEvent& event) {
            if (event.type == neograph::graph::GraphEvent::Type::NODE_START)
                std::cout << "[start] " << event.node_name << "\n";
            if (event.type == neograph::graph::GraphEvent::Type::NODE_END)
                std::cout << "[done]  " << event.node_name << "\n";
        });

    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - total_start).count();

    std::cout << "\n실행 추적: ";
    for (size_t i = 0; i < result.execution_trace.size(); ++i) {
        std::cout << result.execution_trace[i];
        if (i + 1 < result.execution_trace.size()) std::cout << " → ";
    }
    std::cout << " → END\n";

    // 요약 출력
    if (result.output.contains("channels") &&
        result.output["channels"].contains("summary")) {
        std::cout << "\n" << result.output["channels"]["summary"]["value"].get<std::string>() << "\n";
    }

    std::cout << "\n총 소요시간: " << total_ms << "ms";
    std::cout << " (순차 실행이었으면 ~370ms, 병렬이므로 ~150ms)\n";

    return 0;
}
