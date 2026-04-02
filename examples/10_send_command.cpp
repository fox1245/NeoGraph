// NeoGraph Example 10: Send & Command Engine Integration
//
// Send(동적 fan-out)와 Command(라우팅 오버라이드)가 엔진에서
// 실제로 동작하는 것을 보여주는 예제.
//
// 시나리오: 리서치 에이전트
//   1. planner 노드가 사용자 질문을 분석하고 Send로 동적 fan-out
//   2. researcher 노드가 각 토픽을 병렬 조사
//   3. evaluator 노드가 결과를 평가하고 Command로 분기
//      - 충분하면 → summarizer → END
//      - 부족하면 → planner로 돌아가서 추가 조사
//
// API 키 불필요 (커스텀 노드)
//
// Usage: ./example_send_command

#include <neograph/neograph.h>

#include <iostream>
#include <thread>
#include <chrono>

using namespace neograph;
using namespace neograph::graph;

// =========================================================================
// PlannerNode: 사용자 질문에서 토픽을 추출하고 Send로 동적 fan-out
// =========================================================================
class PlannerNode : public GraphNode {
    static int round_;
public:
    // execute_full을 오버라이드하여 Send를 반환
    NodeResult execute_full(const GraphState& state) override {
        round_++;
        auto query = state.get("query");
        std::string q = query.is_string() ? query.get<std::string>() : "general research";

        // 1라운드: 3개 토픽, 2라운드: 2개 추가 토픽
        std::vector<std::string> topics;
        if (round_ == 1) {
            topics = {"market_size", "key_players", "technology_trends"};
        } else {
            topics = {"risk_factors", "future_outlook"};
        }

        NodeResult nr;
        nr.writes.push_back(ChannelWrite{"plan", json({
            {"round", round_},
            {"query", q},
            {"topics", topics}
        })});

        // Send: researcher 노드를 토픽 수만큼 동적 실행
        // Send.input은 채널명→값 매핑 (apply_input으로 상태에 적용됨)
        for (const auto& topic : topics) {
            nr.sends.push_back(Send{"researcher", json({
                {"topic", topic}   // "topic" 채널에 토픽 문자열 주입
            })});
        }

        return nr;
    }

    std::vector<ChannelWrite> execute(const GraphState& state) override {
        return execute_full(state).writes;
    }

    std::string name() const override { return "planner"; }
};
int PlannerNode::round_ = 0;

// =========================================================================
// ResearcherNode: 단일 토픽 조사 (Send로 호출됨)
// =========================================================================
class ResearcherNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto topic_json = state.get("topic");
        std::string topic = topic_json.is_string() ? topic_json.get<std::string>() : "unknown";

        // 시뮬레이션: 조사에 50~100ms 소요
        int delay = 50 + (std::hash<std::string>{}(topic) % 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));

        json finding = {
            {"topic", topic},
            {"result", "Analysis of " + topic + " completed."},
            {"confidence", 0.7 + (std::hash<std::string>{}(topic) % 30) / 100.0},
            {"elapsed_ms", delay}
        };

        return {ChannelWrite{"findings", json::array({finding})}};
    }

    std::string name() const override { return "researcher"; }
};

// =========================================================================
// EvaluatorNode: 결과 평가 후 Command로 분기 결정
// =========================================================================
class EvaluatorNode : public GraphNode {
    static int eval_count_;
public:
    NodeResult execute_full(const GraphState& state) override {
        eval_count_++;
        auto findings = state.get("findings");
        int count = findings.is_array() ? (int)findings.size() : 0;

        NodeResult nr;

        if (count >= 5 || eval_count_ >= 2) {
            // 충분한 데이터 → summarizer로
            nr.command = Command{
                "summarizer",
                {ChannelWrite{"eval_status", json("sufficient: " + std::to_string(count) + " findings")}}
            };
        } else {
            // 부족 → planner로 돌아가서 추가 조사
            nr.command = Command{
                "planner",
                {ChannelWrite{"eval_status", json("insufficient: need more research (have " + std::to_string(count) + ")")}}
            };
        }

        return nr;
    }

    std::vector<ChannelWrite> execute(const GraphState& state) override {
        return execute_full(state).writes;
    }

    std::string name() const override { return "evaluator"; }
};
int EvaluatorNode::eval_count_ = 0;

// =========================================================================
// SummarizerNode: 모든 결과를 종합
// =========================================================================
class SummarizerNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto findings = state.get("findings");
        auto eval = state.get("eval_status");

        std::string summary = "=== Research Summary ===\n";
        summary += "Status: " + (eval.is_string() ? eval.get<std::string>() : "n/a") + "\n\n";

        if (findings.is_array()) {
            for (const auto& f : findings) {
                summary += "- [" + f.value("topic", "?") + "] "
                        + f.value("result", "") + " (confidence: "
                        + std::to_string(f.value("confidence", 0.0)).substr(0, 4) + ")\n";
            }
            summary += "\nTotal findings: " + std::to_string(findings.size());
        }

        return {ChannelWrite{"summary", json(summary)}};
    }

    std::string name() const override { return "summarizer"; }
};

// =========================================================================
// Main
// =========================================================================
int main() {
    // 커스텀 노드 등록
    auto& factory = NodeFactory::instance();

    factory.register_type("planner",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<PlannerNode>(); });

    factory.register_type("researcher",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<ResearcherNode>(); });

    factory.register_type("evaluator",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<EvaluatorNode>(); });

    factory.register_type("summarizer_node",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<SummarizerNode>(); });

    // 그래프 정의
    // planner → (Send로 researcher 동적 실행) → evaluator → Command로 분기
    json definition = {
        {"name", "research_agent"},
        {"channels", {
            {"query",       {{"reducer", "overwrite"}}},
            {"plan",        {{"reducer", "overwrite"}}},
            {"topic",       {{"reducer", "overwrite"}}},  // Send가 주입하는 채널
            {"findings",    {{"reducer", "append"}}},
            {"eval_status", {{"reducer", "overwrite"}}},
            {"summary",     {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"planner",    {{"type", "planner"}}},
            {"researcher", {{"type", "researcher"}}},
            {"evaluator",  {{"type", "evaluator"}}},
            {"summarizer", {{"type", "summarizer_node"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "planner"}},
            {{"from", "planner"},   {"to", "evaluator"}},
            // evaluator는 Command로 라우팅하므로 여기서 edge 불필요
            // (Command.goto가 "summarizer" 또는 "planner"로 직접 지정)
            {{"from", "summarizer"}, {"to", "__end__"}}
        })}
    };

    NodeContext ctx;
    auto engine = GraphEngine::compile(definition, ctx);

    // 실행
    std::cout << "=== Send & Command Integration Demo ===\n\n";

    auto start = std::chrono::steady_clock::now();

    RunConfig config;
    config.input = {{"query", "AI semiconductor market analysis"}};
    config.max_steps = 20;
    config.stream_mode = StreamMode::EVENTS | StreamMode::DEBUG;

    auto result = engine->run_stream(config,
        [](const GraphEvent& event) {
            switch (event.type) {
                case GraphEvent::Type::NODE_START:
                    if (event.node_name == "__send__") {
                        auto& sends = event.data["sends"];
                        std::cout << "  [send] Dynamic fan-out: " << sends.size() << " tasks\n";
                        for (const auto& s : sends)
                            std::cout << "         → " << s["target"] << "(" << s["input"]["topic"] << ")\n";
                    } else if (event.node_name == "__routing__") {
                        if (event.data.contains("command_goto"))
                            std::cout << "  [cmd]  Command goto → " << event.data["command_goto"] << "\n";
                        else if (event.data.contains("next_nodes"))
                            std::cout << "  [route] → " << event.data["next_nodes"].dump() << "\n";
                    } else {
                        std::cout << "  [start] " << event.node_name << "\n";
                    }
                    break;
                case GraphEvent::Type::NODE_END: {
                    std::string extra;
                    if (event.data.contains("command_goto"))
                        extra = " (cmd→" + event.data["command_goto"].get<std::string>() + ")";
                    if (event.data.contains("sends"))
                        extra = " (sends=" + std::to_string(event.data["sends"].get<int>()) + ")";
                    std::cout << "  [done]  " << event.node_name << extra << "\n";
                    break;
                }
                default: break;
            }
        });

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    // 결과 출력
    std::cout << "\n";
    std::cout << "Execution trace: ";
    for (size_t i = 0; i < result.execution_trace.size(); ++i) {
        std::cout << result.execution_trace[i];
        if (i + 1 < result.execution_trace.size()) std::cout << " → ";
    }
    std::cout << " → END\n\n";

    if (result.output.contains("channels") &&
        result.output["channels"].contains("summary") &&
        result.output["channels"]["summary"]["value"].is_string()) {
        std::cout << result.output["channels"]["summary"]["value"].get<std::string>() << "\n";
    } else {
        std::cout << "(No summary generated — check execution trace)\n";
    }

    std::cout << "\nTotal time: " << elapsed << "ms\n";
    std::cout << "(Researchers ran in parallel via Send + Taskflow)\n";

    return 0;
}
