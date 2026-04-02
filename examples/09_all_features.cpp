// NeoGraph Example 09: All New Features Demo
//
// 하나의 예제에서 6가지 신규 기능을 모두 사용합니다:
//
//   1. NodeInterrupt  — 동적 breakpoint (노드 내부에서 throw)
//   2. RetryPolicy    — 실패 시 exponential backoff 재시도
//   3. StreamMode     — EVENTS | DEBUG 모드로 내부 동작 관찰
//   4. Send           — 동적 fan-out (map-reduce 패턴)
//   5. Command        — 라우팅 + 상태 수정 동시 반환
//   6. Store          — cross-thread 공유 메모리
//
// API 키 불필요 (커스텀 노드 사용)
//
// Usage: ./example_all_features

#include <neograph/neograph.h>
#include <neograph/graph/react_graph.h>

#include <iostream>
#include <iomanip>
#include <thread>
#include <atomic>

using namespace neograph;
using namespace neograph::graph;

// =========================================================================
// 1. NodeInterrupt — 위험 금액 감지 시 동적 중단
// =========================================================================
class PaymentNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto amount_json = state.get("amount");
        int amount = amount_json.is_number() ? amount_json.get<int>() : 0;

        // 100만원 이상이면 동적 breakpoint
        if (amount >= 1000000) {
            throw NodeInterrupt(
                "고액 결제 감지: " + std::to_string(amount) + "원. 관리자 승인 필요.");
        }

        return {ChannelWrite{"result", json("결제 완료: " + std::to_string(amount) + "원")}};
    }
    std::string name() const override { return "payment"; }
};

// =========================================================================
// 2. RetryPolicy — 간헐적 실패 노드 (3번째 시도에 성공)
// =========================================================================
class UnstableAPINode : public GraphNode {
    static std::atomic<int> call_count_;
public:
    std::vector<ChannelWrite> execute(const GraphState&) override {
        int count = ++call_count_;
        if (count < 3) {
            throw std::runtime_error(
                "API 일시 장애 (attempt " + std::to_string(count) + ")");
        }
        return {ChannelWrite{"api_result", json("외부 API 응답 성공 (attempt " + std::to_string(count) + ")")}};
    }
    std::string name() const override { return "unstable_api"; }
};
std::atomic<int> UnstableAPINode::call_count_{0};

// =========================================================================
// 5. Command — 점수에 따라 라우팅 + 상태 동시 수정
// =========================================================================
// (Command 구조체 자체를 보여주는 데모. 현재 엔진은 ChannelWrite 기반이므로
//  Command의 updates를 직접 적용하고, goto는 __route__ 채널로 표현합니다.)
class ScoreRouterNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto score = state.get("score");
        int s = score.is_number() ? score.get<int>() : 0;

        // Command 패턴: 라우팅 결정 + 상태 수정을 동시에
        Command cmd;
        if (s >= 90) {
            cmd.goto_node = "premium";
            cmd.updates = {ChannelWrite{"tier", json("PREMIUM")}};
        } else if (s >= 60) {
            cmd.goto_node = "standard";
            cmd.updates = {ChannelWrite{"tier", json("STANDARD")}};
        } else {
            cmd.goto_node = "basic";
            cmd.updates = {ChannelWrite{"tier", json("BASIC")}};
        }

        // 라우팅은 __route__ 채널로, 상태는 직접 write
        std::vector<ChannelWrite> writes;
        writes.push_back(ChannelWrite{"__route__", json(cmd.goto_node)});
        for (auto& u : cmd.updates) writes.push_back(std::move(u));

        return writes;
    }
    std::string name() const override { return "score_router"; }
};

// 등급별 처리 노드
class TierNode : public GraphNode {
    std::string name_;
    std::string message_;
public:
    TierNode(const std::string& n, const std::string& msg) : name_(n), message_(msg) {}
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto tier = state.get("tier");
        return {ChannelWrite{"result", json(message_ + " (등급: " + tier.get<std::string>() + ")")}};
    }
    std::string name() const override { return name_; }
};

// =========================================================================
// Helper: 구분선 출력
// =========================================================================
static void section(const std::string& title) {
    std::cout << "\n" << std::string(60, '=') << "\n"
              << " " << title << "\n"
              << std::string(60, '=') << "\n\n";
}

// =========================================================================
// Stream callback — StreamMode에 따라 선택적 출력
// =========================================================================
static void stream_handler(const GraphEvent& event) {
    switch (event.type) {
        case GraphEvent::Type::NODE_START:
            if (event.node_name == "__routing__") {
                // DEBUG 모드 라우팅 정보
                std::cout << "  [debug] routing → " << event.data.dump() << "\n";
            } else {
                std::string extra;
                if (event.data.contains("retry_attempt"))
                    extra = " (retry #" + std::to_string(event.data["retry_attempt"].get<int>()) + ")";
                std::cout << "  [start] " << event.node_name << extra << "\n";
            }
            break;
        case GraphEvent::Type::NODE_END:
            std::cout << "  [done]  " << event.node_name << "\n";
            break;
        case GraphEvent::Type::CHANNEL_WRITE:
            if (event.node_name == "__state__") {
                std::cout << "  [values] full state snapshot emitted\n";
            } else if (event.data.contains("value")) {
                std::cout << "  [update] " << event.node_name << "."
                          << event.data.value("channel", "?") << " = "
                          << event.data["value"].dump().substr(0, 60) << "\n";
            }
            break;
        case GraphEvent::Type::INTERRUPT:
            std::cout << "  [INTERRUPT] " << event.node_name
                      << " — " << event.data.dump() << "\n";
            break;
        case GraphEvent::Type::ERROR:
            std::cout << "  [error] " << event.node_name
                      << " — " << event.data.dump() << "\n";
            break;
        default:
            break;
    }
}

int main() {
    // ================================================================
    // Demo 1: NodeInterrupt (동적 breakpoint)
    // ================================================================
    section("1. NodeInterrupt — 동적 breakpoint");
    {
        NodeFactory::instance().register_type("payment",
            [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<PaymentNode>();
            });

        json def = {
            {"name", "payment_graph"},
            {"channels", {
                {"amount", {{"reducer", "overwrite"}}},
                {"result", {{"reducer", "overwrite"}}}
            }},
            {"nodes", {{"payment", {{"type", "payment"}}}}},
            {"edges", json::array({
                {{"from", "__start__"}, {"to", "payment"}},
                {{"from", "payment"}, {"to", "__end__"}}
            })}
        };

        NodeContext ctx;
        auto store = std::make_shared<InMemoryCheckpointStore>();
        auto engine = GraphEngine::compile(def, ctx, store);

        // 소액 결제: 정상 통과
        RunConfig cfg;
        cfg.thread_id = "pay-001";
        cfg.input = {{"amount", 50000}};
        auto r1 = engine->run_stream(cfg, stream_handler);
        std::cout << "  결과: " << r1.output["channels"]["result"]["value"] << "\n\n";

        // 고액 결제: NodeInterrupt 발동
        cfg.thread_id = "pay-002";
        cfg.input = {{"amount", 2500000}};
        cfg.stream_mode = StreamMode::EVENTS | StreamMode::DEBUG;
        auto r2 = engine->run_stream(cfg, stream_handler);
        std::cout << "  interrupted: " << std::boolalpha << r2.interrupted << "\n";
        std::cout << "  사유: " << r2.interrupt_value.value("reason", "") << "\n";

        // 승인 후 resume
        std::cout << "\n  >>> 관리자 승인 → resume <<<\n";
        auto r3 = engine->resume("pay-002", json(), stream_handler);
        std::cout << "  결과: " << r3.output["channels"]["result"]["value"] << "\n";
    }

    // ================================================================
    // Demo 2: RetryPolicy (자동 재시도)
    // ================================================================
    section("2. RetryPolicy — exponential backoff 재시도");
    {
        NodeFactory::instance().register_type("unstable_api",
            [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<UnstableAPINode>();
            });

        json def = {
            {"name", "retry_graph"},
            {"channels", {{"api_result", {{"reducer", "overwrite"}}}}},
            {"nodes", {{"unstable_api", {{"type", "unstable_api"}}}}},
            {"edges", json::array({
                {{"from", "__start__"}, {"to", "unstable_api"}},
                {{"from", "unstable_api"}, {"to", "__end__"}}
            })}
        };

        NodeContext ctx;
        auto engine = GraphEngine::compile(def, ctx);

        // 노드별 retry 정책 설정
        engine->set_node_retry_policy("unstable_api", {
            .max_retries = 5,
            .initial_delay_ms = 10,   // 데모용으로 짧게
            .backoff_multiplier = 2.0f,
            .max_delay_ms = 100
        });

        RunConfig cfg;
        cfg.stream_mode = StreamMode::EVENTS | StreamMode::DEBUG;
        auto result = engine->run_stream(cfg, stream_handler);
        std::cout << "  결과: " << result.output["channels"]["api_result"]["value"] << "\n";
    }

    // ================================================================
    // Demo 3: StreamMode (선택적 스트리밍)
    // ================================================================
    section("3. StreamMode — VALUES + UPDATES 모드");
    {
        json def = {
            {"name", "stream_demo"},
            {"channels", {
                {"amount", {{"reducer", "overwrite"}}},
                {"result", {{"reducer", "overwrite"}}}
            }},
            {"nodes", {{"payment", {{"type", "payment"}}}}},
            {"edges", json::array({
                {{"from", "__start__"}, {"to", "payment"}},
                {{"from", "payment"}, {"to", "__end__"}}
            })}
        };

        NodeContext ctx;
        auto engine = GraphEngine::compile(def, ctx);

        RunConfig cfg;
        cfg.input = {{"amount", 30000}};
        // VALUES + UPDATES만 (EVENTS 없이)
        cfg.stream_mode = StreamMode::VALUES | StreamMode::UPDATES;

        std::cout << "  (EVENTS 없이 VALUES + UPDATES만 스트림)\n";
        auto result = engine->run_stream(cfg,
            [](const GraphEvent& ev) {
                if (ev.type == GraphEvent::Type::NODE_START ||
                    ev.type == GraphEvent::Type::NODE_END) {
                    // EVENTS 모드가 꺼져있으므로 이건 안 옴
                    std::cout << "  [!] 이건 보이면 안 됨\n";
                }
                if (ev.type == GraphEvent::Type::CHANNEL_WRITE) {
                    if (ev.node_name == "__state__")
                        std::cout << "  [values] state snapshot received\n";
                    else
                        std::cout << "  [update] " << ev.data.dump().substr(0, 80) << "\n";
                }
            });
        std::cout << "  결과: " << result.output["channels"]["result"]["value"] << "\n";
    }

    // ================================================================
    // Demo 4: Send (동적 fan-out 구조체)
    // ================================================================
    section("4. Send — 동적 fan-out 구조체");
    {
        // Send 구조체 사용법 데모 (구조체 자체가 데이터임)
        std::vector<Send> sends = {
            {"researcher", {{"topic", "AI"}}},
            {"researcher", {{"topic", "Quantum"}}},
            {"researcher", {{"topic", "Biotech"}}},
        };

        std::cout << "  Send 요청 " << sends.size() << "개 생성:\n";
        for (const auto& s : sends) {
            std::cout << "    → " << s.target_node
                      << " (input: " << s.input.dump() << ")\n";
        }
        std::cout << "\n  이 Send들은 엔진의 동적 fan-out 스케줄러가\n"
                  << "  같은 노드를 서로 다른 입력으로 병렬 실행합니다.\n";
    }

    // ================================================================
    // Demo 5: Command (라우팅 + 상태 수정)
    // ================================================================
    section("5. Command — 라우팅 + 상태 동시 수정");
    {
        NodeFactory::instance().register_type("score_router",
            [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<ScoreRouterNode>();
            });
        NodeFactory::instance().register_type("tier_node",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<TierNode>(name, config.value("message", "처리 완료"));
            });

        json def = {
            {"name", "command_graph"},
            {"channels", {
                {"score",     {{"reducer", "overwrite"}}},
                {"tier",      {{"reducer", "overwrite"}}},
                {"result",    {{"reducer", "overwrite"}}},
                {"__route__", {{"reducer", "overwrite"}}}
            }},
            {"nodes", {
                {"score_router", {{"type", "score_router"}}},
                {"premium",  {{"type", "tier_node"}, {"message", "VIP 전용 서비스 제공"}}},
                {"standard", {{"type", "tier_node"}, {"message", "일반 서비스 제공"}}},
                {"basic",    {{"type", "tier_node"}, {"message", "기본 서비스 제공"}}}
            }},
            {"edges", json::array({
                {{"from", "__start__"}, {"to", "score_router"}},
                {{"from", "score_router"}, {"condition", "route_channel"},
                 {"routes", {{"premium", "premium"}, {"standard", "standard"}, {"basic", "basic"}}}},
                {{"from", "premium"}, {"to", "__end__"}},
                {{"from", "standard"}, {"to", "__end__"}},
                {{"from", "basic"}, {"to", "__end__"}}
            })}
        };

        NodeContext ctx;

        struct TestCase { int score; std::string expected; };
        std::vector<TestCase> cases = {{95, "premium"}, {72, "standard"}, {40, "basic"}};

        for (const auto& tc : cases) {
            auto engine = GraphEngine::compile(def, ctx);
            RunConfig cfg;
            cfg.input = {{"score", tc.score}};
            cfg.stream_mode = StreamMode::EVENTS | StreamMode::UPDATES;

            std::cout << "  점수 " << tc.score << ":\n";
            auto result = engine->run_stream(cfg, stream_handler);
            std::cout << "  → " << result.output["channels"]["result"]["value"]
                      << "  (tier: " << result.output["channels"]["tier"]["value"] << ")\n\n";
        }
    }

    // ================================================================
    // Demo 6: Store (cross-thread 공유 메모리)
    // ================================================================
    section("6. Store — cross-thread 공유 메모리");
    {
        auto store = std::make_shared<InMemoryStore>();

        // Thread A: 사용자 선호도 저장
        store->put({"users", "user123"}, "language", json("ko"));
        store->put({"users", "user123"}, "theme", json("dark"));
        store->put({"users", "user456"}, "language", json("en"));

        // Thread B: 선호도 조회
        auto lang = store->get({"users", "user123"}, "language");
        if (lang) {
            std::cout << "  user123 language: " << lang->value << "\n";
        }

        // Namespace 검색
        auto user123_prefs = store->search({"users", "user123"});
        std::cout << "  user123 preferences (" << user123_prefs.size() << "개):\n";
        for (const auto& item : user123_prefs) {
            std::cout << "    " << item.key << " = " << item.value << "\n";
        }

        // 전체 namespace 목록
        auto namespaces = store->list_namespaces({"users"});
        std::cout << "\n  Namespaces under 'users' (" << namespaces.size() << "개):\n";
        for (const auto& ns : namespaces) {
            std::string path;
            for (size_t i = 0; i < ns.size(); ++i) {
                if (i > 0) path += "/";
                path += ns[i];
            }
            std::cout << "    " << path << "\n";
        }

        // GraphEngine에 Store 연결
        json def = {
            {"name", "store_demo"},
            {"channels", {
                {"amount", {{"reducer", "overwrite"}}},
                {"result", {{"reducer", "overwrite"}}}
            }},
            {"nodes", {{"payment", {{"type", "payment"}}}}},
            {"edges", json::array({
                {{"from", "__start__"}, {"to", "payment"}},
                {{"from", "payment"}, {"to", "__end__"}}
            })}
        };
        NodeContext ctx;
        auto engine = GraphEngine::compile(def, ctx);
        engine->set_store(store);

        std::cout << "\n  Engine store 연결: "
                  << (engine->get_store() ? "OK" : "FAIL")
                  << " (size=" << store->size() << ")\n";
    }

    // ================================================================
    // Summary
    // ================================================================
    section("Summary");
    std::cout << "  All 6 features demonstrated successfully:\n\n"
              << "  1. NodeInterrupt  — 고액 결제 시 동적 중단 + resume\n"
              << "  2. RetryPolicy    — 2번 실패 후 3번째 성공 (backoff)\n"
              << "  3. StreamMode     — VALUES/UPDATES/EVENTS/DEBUG 선택적 스트림\n"
              << "  4. Send           — 동적 fan-out 요청 구조체\n"
              << "  5. Command        — 점수 기반 라우팅 + 등급 동시 설정\n"
              << "  6. Store          — 사용자 선호도 cross-thread 공유 메모리\n"
              << "\n";

    return 0;
}
