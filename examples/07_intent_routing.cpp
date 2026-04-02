// NeoGraph Example 07: Intent-based Dynamic Routing
//
// LLM이 사용자 의도를 분류하고, 결과에 따라
// 전문가 서브그래프로 동적 라우팅하는 예제.
//
// 시나리오: Panel of Experts
//   classifier → (math? → math_expert, translate? → translate_expert, else → general)
//
// API 키 불필요 (Mock Provider 사용)
//
// Usage: ./example_intent_routing

#include <neograph/neograph.h>

#include <iostream>

// Mock Provider: 의도 분류 + 전문가 응답
class RoutingMockProvider : public neograph::Provider {
    int call_count_ = 0;
public:
    neograph::ChatCompletion complete(const neograph::CompletionParams& params) override {
        neograph::ChatCompletion result;
        result.message.role = "assistant";

        // 첫 번째 호출: 의도 분류 (IntentClassifierNode가 호출)
        // → system prompt에 "Classify" 가 있으면 분류 응답
        bool is_classifier = false;
        for (const auto& msg : params.messages) {
            if (msg.role == "system" && msg.content.find("Classify") != std::string::npos) {
                is_classifier = true;
                break;
            }
        }

        if (is_classifier) {
            // 사용자 메시지에서 의도 판별
            std::string user_msg;
            for (const auto& msg : params.messages) {
                if (msg.role == "user") user_msg = msg.content;
            }

            if (user_msg.find("계산") != std::string::npos ||
                user_msg.find("더하기") != std::string::npos ||
                user_msg.find("+") != std::string::npos) {
                result.message.content = "math";
            } else if (user_msg.find("번역") != std::string::npos ||
                       user_msg.find("translate") != std::string::npos) {
                result.message.content = "translate";
            } else {
                result.message.content = "general";
            }
        } else {
            // 전문가 응답
            std::string instruction;
            for (const auto& msg : params.messages) {
                if (msg.role == "system") instruction = msg.content;
            }

            if (instruction.find("수학") != std::string::npos) {
                result.message.content = "수학 전문가입니다. 42 + 58 = 100입니다.";
            } else if (instruction.find("번역") != std::string::npos) {
                result.message.content = "번역 전문가입니다. 'Hello' → '안녕하세요'";
            } else {
                result.message.content = "범용 어시스턴트입니다. 무엇이든 도와드리겠습니다.";
            }
        }

        return result;
    }

    neograph::ChatCompletion complete_stream(
        const neograph::CompletionParams& p, const neograph::StreamCallback& cb) override {
        auto r = complete(p);
        if (cb && !r.message.content.empty()) cb(r.message.content);
        return r;
    }
    std::string get_name() const override { return "routing_mock"; }
};

int main() {
    auto provider = std::make_shared<RoutingMockProvider>();

    neograph::graph::NodeContext ctx;
    ctx.provider = provider;

    // 전문가별 서브그래프 컨텍스트 (다른 instructions)
    neograph::graph::NodeContext math_ctx = ctx;
    math_ctx.instructions = "당신은 수학 전문가입니다. 계산 문제를 풀어주세요.";

    neograph::graph::NodeContext translate_ctx = ctx;
    translate_ctx.instructions = "당신은 번역 전문가입니다. 번역을 해주세요.";

    neograph::graph::NodeContext general_ctx = ctx;
    general_ctx.instructions = "당신은 범용 어시스턴트입니다.";

    // 그래프 정의
    neograph::json definition = {
        {"name", "intent_router"},
        {"channels", {
            {"messages",  {{"reducer", "append"}}},
            {"__route__", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            // 의도 분류기
            {"classifier", {
                {"type", "intent_classifier"},
                {"routes", neograph::json::array({"math", "translate", "general"})},
                {"prompt", "Classify the user's intent. Respond with ONLY one of: math, translate, general"}
            }},
            // 전문가 서브그래프들
            {"math_expert", {
                {"type", "subgraph"},
                {"definition", {
                    {"name", "math_agent"},
                    {"channels", {{"messages", {{"reducer", "append"}}}}},
                    {"nodes", {{"llm", {{"type", "llm_call"}}}}},
                    {"edges", neograph::json::array({
                        {{"from", "__start__"}, {"to", "llm"}},
                        {{"from", "llm"}, {"to", "__end__"}}
                    })}
                }}
            }},
            {"translate_expert", {
                {"type", "subgraph"},
                {"definition", {
                    {"name", "translate_agent"},
                    {"channels", {{"messages", {{"reducer", "append"}}}}},
                    {"nodes", {{"llm", {{"type", "llm_call"}}}}},
                    {"edges", neograph::json::array({
                        {{"from", "__start__"}, {"to", "llm"}},
                        {{"from", "llm"}, {"to", "__end__"}}
                    })}
                }}
            }},
            {"general_expert", {
                {"type", "subgraph"},
                {"definition", {
                    {"name", "general_agent"},
                    {"channels", {{"messages", {{"reducer", "append"}}}}},
                    {"nodes", {{"llm", {{"type", "llm_call"}}}}},
                    {"edges", neograph::json::array({
                        {{"from", "__start__"}, {"to", "llm"}},
                        {{"from", "llm"}, {"to", "__end__"}}
                    })}
                }}
            }}
        }},
        {"edges", neograph::json::array({
            {{"from", "__start__"}, {"to", "classifier"}},
            // 의도에 따라 라우팅
            {{"from", "classifier"}, {"condition", "route_channel"},
             {"routes", {
                 {"math", "math_expert"},
                 {"translate", "translate_expert"},
                 {"general", "general_expert"}
             }}},
            {{"from", "math_expert"}, {"to", "__end__"}},
            {{"from", "translate_expert"}, {"to", "__end__"}},
            {{"from", "general_expert"}, {"to", "__end__"}}
        })}
    };

    auto engine = neograph::graph::GraphEngine::compile(definition, ctx);

    // 테스트 케이스 3개 실행
    struct TestCase {
        std::string question;
        std::string expected_route;
    };

    std::vector<TestCase> cases = {
        {"42 더하기 58은 뭐야?", "math"},
        {"Hello를 번역해줘", "translate"},
        {"오늘 뭐하지?", "general"}
    };

    for (const auto& tc : cases) {
        std::cout << "=== Q: " << tc.question << " ===\n";

        neograph::graph::RunConfig config;
        config.input = {{"messages", neograph::json::array({
            {{"role", "user"}, {"content", tc.question}}
        })}};

        auto result = engine->run_stream(config,
            [](const neograph::graph::GraphEvent& event) {
                if (event.type == neograph::graph::GraphEvent::Type::LLM_TOKEN)
                    std::cout << event.data.get<std::string>();
            });

        std::cout << "\n추적: ";
        for (size_t i = 0; i < result.execution_trace.size(); ++i) {
            std::cout << result.execution_trace[i];
            if (i + 1 < result.execution_trace.size()) std::cout << " → ";
        }
        std::cout << " → END\n\n";
    }

    return 0;
}
