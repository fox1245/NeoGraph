// NeoGraph Example 08: State Management (get_state / update_state / fork)
//
// LangGraph의 Checkpointer API에 대응하는 상태 관리 기능 데모.
//
// 시나리오:
//   1. 그래프 실행 → interrupt → 상태 조회 (get_state)
//   2. 상태 수정 (update_state) — 메시지 편집 후 재개
//   3. 분기 (fork) — 기존 체크포인트에서 새 스레드로 포크
//   4. 시간 여행 — 과거 체크포인트에서 다시 실행
//
// API 키 불필요 (Mock Provider 사용)
//
// Usage: ./example_state_management

#include <neograph/neograph.h>
#include <neograph/graph/react_graph.h>

#include <iostream>
#include <iomanip>

// Mock Provider: 메시지 내용에 따라 응답이 달라짐
class StateMockProvider : public neograph::Provider {
public:
    neograph::ChatCompletion complete(const neograph::CompletionParams& params) override {
        neograph::ChatCompletion result;
        result.message.role = "assistant";

        // 마지막 user 메시지 확인
        std::string last_user;
        for (auto it = params.messages.rbegin(); it != params.messages.rend(); ++it) {
            if (it->role == "user") { last_user = it->content; break; }
        }

        if (last_user.find("서울") != std::string::npos) {
            result.message.content = "서울은 대한민국의 수도이며, 인구 약 950만명의 도시입니다.";
        } else if (last_user.find("부산") != std::string::npos) {
            result.message.content = "부산은 대한민국 제2의 도시이며, 해운대 해수욕장으로 유명합니다.";
        } else if (last_user.find("도쿄") != std::string::npos) {
            result.message.content = "도쿄는 일본의 수도이며, 세계 최대 도시권 중 하나입니다.";
        } else {
            result.message.content = "안녕하세요! 도시에 대해 물어봐 주세요.";
        }
        return result;
    }

    neograph::ChatCompletion complete_stream(
        const neograph::CompletionParams& p, const neograph::StreamCallback& cb) override {
        auto r = complete(p);
        if (cb && !r.message.content.empty()) cb(r.message.content);
        return r;
    }
    std::string get_name() const override { return "state_mock"; }
};

static void print_separator(const std::string& title) {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << " " << title << "\n";
    std::cout << std::string(60, '=') << "\n\n";
}

static void print_messages(const neograph::json& state) {
    if (!state.contains("channels") || !state["channels"].contains("messages")) return;
    auto& msgs = state["channels"]["messages"]["value"];
    if (!msgs.is_array()) return;

    for (const auto& m : msgs) {
        std::string role = m.value("role", "?");
        std::string content = m.value("content", "");
        if (!content.empty()) {
            std::cout << "  [" << role << "] " << content << "\n";
        }
    }
}

int main() {
    auto provider = std::make_shared<StateMockProvider>();

    // 2-노드 그래프: llm → reviewer (reviewer 전에 interrupt)
    // 사용자가 LLM 응답을 검토한 뒤 reviewer가 최종 확인하는 구조
    neograph::json definition = {
        {"name", "state_demo"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {
            {"llm",      {{"type", "llm_call"}}},
            {"reviewer", {{"type", "llm_call"}}}
        }},
        {"edges", neograph::json::array({
            {{"from", "__start__"}, {"to", "llm"}},
            {{"from", "llm"},      {"to", "reviewer"}},
            {{"from", "reviewer"}, {"to", "__end__"}}
        })},
        {"interrupt_before", neograph::json::array({"reviewer"})}
    };

    neograph::graph::NodeContext ctx;
    ctx.provider = provider;

    auto store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    auto engine = neograph::graph::GraphEngine::compile(definition, ctx, store);

    // ================================================================
    // 1. 실행 → interrupt → get_state
    // ================================================================
    print_separator("1. 실행 후 상태 조회 (get_state)");

    neograph::graph::RunConfig config;
    config.thread_id = "thread-001";
    config.input = {{"messages", neograph::json::array({
        {{"role", "user"}, {"content", "서울에 대해 알려줘"}}
    })}};

    auto result = engine->run(config);
    std::cout << "interrupted: " << std::boolalpha << result.interrupted << "\n\n";

    // get_state로 현재 상태 조회
    auto state = engine->get_state("thread-001");
    if (state) {
        std::cout << "현재 대화 내용:\n";
        print_messages(*state);
    }

    // ================================================================
    // 2. update_state — 메시지를 수정하고 재개
    // ================================================================
    print_separator("2. 상태 수정 후 재개 (update_state)");

    std::cout << "원래 질문: \"서울에 대해 알려줘\"\n";
    std::cout << "수정 후:   \"부산에 대해 알려줘\" (질문 변경)\n\n";

    // 새 user 메시지를 추가 (reducer=append이므로 기존에 추가됨)
    engine->update_state("thread-001", {
        {"messages", neograph::json::array({
            {{"role", "user"}, {"content", "부산에 대해 알려줘"}}
        })}
    });

    // 수정된 상태 확인
    auto updated_state = engine->get_state("thread-001");
    if (updated_state) {
        std::cout << "수정된 대화 내용:\n";
        print_messages(*updated_state);
    }

    // resume로 재개
    std::cout << "\n재개 실행...\n";
    auto resumed = engine->resume("thread-001");
    std::cout << "실행 추적: ";
    for (const auto& n : resumed.execution_trace) std::cout << n << " → ";
    std::cout << "END\n\n";

    auto final_state = engine->get_state("thread-001");
    if (final_state) {
        std::cout << "최종 대화 내용:\n";
        print_messages(*final_state);
    }

    // ================================================================
    // 3. fork — 분기 실행
    // ================================================================
    print_separator("3. 분기 (fork)");

    // thread-001의 현재 상태에서 새 스레드로 분기
    auto fork_cp_id = engine->fork("thread-001", "thread-001-tokyo");
    std::cout << "fork 완료: thread-001 → thread-001-tokyo\n";
    std::cout << "새 체크포인트 ID: " << fork_cp_id << "\n\n";

    // 분기된 스레드의 상태 수정
    engine->update_state("thread-001-tokyo", {
        {"messages", neograph::json::array({
            {{"role", "user"}, {"content", "도쿄에 대해 알려줘"}}
        })}
    });

    // 분기된 스레드에서 실행
    auto forked_result = engine->resume("thread-001-tokyo");
    std::cout << "분기 스레드 실행 추적: ";
    for (const auto& n : forked_result.execution_trace) std::cout << n << " → ";
    std::cout << "END\n\n";

    auto fork_state = engine->get_state("thread-001-tokyo");
    if (fork_state) {
        std::cout << "분기 스레드 대화 내용:\n";
        print_messages(*fork_state);
    }

    // ================================================================
    // 4. 상태 히스토리 (시간 여행)
    // ================================================================
    print_separator("4. 상태 히스토리 (Time Travel)");

    auto history = engine->get_state_history("thread-001");
    std::cout << "thread-001 체크포인트 히스토리 (" << history.size() << "개):\n\n";
    for (size_t i = 0; i < history.size(); ++i) {
        const auto& cp = history[i];
        std::cout << "  #" << (i + 1) << " [" << cp.interrupt_phase << "]"
                  << " step=" << cp.step
                  << " node=" << cp.current_node
                  << " → " << cp.next_node
                  << " id=" << cp.id.substr(0, 8) << "...\n";
    }

    auto fork_history = engine->get_state_history("thread-001-tokyo");
    std::cout << "\nthread-001-tokyo 체크포인트 히스토리 (" << fork_history.size() << "개):\n\n";
    for (size_t i = 0; i < fork_history.size(); ++i) {
        const auto& cp = fork_history[i];
        std::string meta_info;
        if (cp.metadata.contains("forked_from")) {
            meta_info = " (forked from " +
                cp.metadata["forked_from"].value("thread_id", "?") + ")";
        }
        std::cout << "  #" << (i + 1) << " [" << cp.interrupt_phase << "]"
                  << " step=" << cp.step
                  << " node=" << cp.current_node
                  << meta_info << "\n";
    }

    return 0;
}
