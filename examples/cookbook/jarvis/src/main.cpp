// jarvis/src/main.cpp — 자비스 진입점
//
// 흐름:
//   1. .env / 환경변수 로드 (OPENAI_API_KEY 등)
//   2. config/mcp_catalog.json  → McpCatalog (MCP 서버 연결 + 도구 캐시)
//   3. config/agent_registry.json → AgentDispatcher (A2A 에이전트 카드 fetch)
//   4. LLM Provider 선택: OPENAI_API_KEY 있으면 OpenAIProvider, 없으면 MockProvider
//   5. NodeFactory 에 자비스 커스텀 노드 타입 전부 등록
//   6. config/jarvis_graph.json 컴파일 → GraphEngine
//   7. 자비스 자신을 A2A 서버로 노출 (agent_registry.json self.enabled=true 일 때)
//   8. 메인 루프: 한 턴 = engine->run() — voice_in 노드가 stdin 을 읽음
//      __shutdown__ 채널 true 또는 SIGINT/SIGTERM 이면 루프 종료

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/graph/store.h>
#include <cppdotenv/dotenv.hpp>

#include "audio/mic_input.h"
#include "audio/tts_output.h"
#include "stt/whisper_node.h"
#include "orchestrator/mcp_catalog.h"
#include "orchestrator/agent_dispatcher.h"
#include "orchestrator/intent_router_node.h"
#include "memory/conversation_store.h"

#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// 전역 종료 플래그 — SIGINT / SIGTERM 핸들러가 세팅
// ─────────────────────────────────────────────────────────────────────────────

namespace {

std::atomic<bool> g_shutdown_requested{false};

void install_signal_handlers() {
    std::signal(SIGINT,  [](int) { g_shutdown_requested.store(true); });
    std::signal(SIGTERM, [](int) { g_shutdown_requested.store(true); });
}

// ─────────────────────────────────────────────────────────────────────────────
// MockProvider — OPENAI_API_KEY 없을 때 사용하는 echo 제공자.
//
// IntentRouterNode 가 호출하면:
//   - 시스템 프롬프트 내용에 관계없이 항상 라우팅 mock JSON 을 반환.
//   - 그래프가 direct + skip_synthesis=true 경로로 흘러서 TTS 직행.
// ─────────────────────────────────────────────────────────────────────────────

class MockProvider : public neograph::Provider {
public:
    // invoke() — v0.4 단일 진입점
    asio::awaitable<neograph::ChatCompletion>
    invoke(const neograph::CompletionParams& params,
           neograph::StreamCallback on_chunk) override
    {
        neograph::ChatCompletion result;
        result.message.role = "assistant";

        // 시스템 프롬프트에 "IMPORTANT: respond with a single JSON" 문자열이 있으면
        // 라우터 호출로 판단 → mock 라우팅 JSON 반환
        bool is_router_call = false;
        for (const auto& msg : params.messages) {
            if (msg.role == "system" &&
                msg.content.find("IMPORTANT: respond with a single JSON") != std::string::npos) {
                is_router_call = true;
                break;
            }
        }

        if (is_router_call) {
            // 사용자 텍스트를 꺼내서 마지막 user 메시지 기준으로 echo 응답
            // direct + skip_synthesis=true → tool_results → synth_skip → final_text
            result.message.content =
                R"({"mode":"direct","tool_calls":[],"delegate_to":null,"skip_synthesis":true,"reasoning_short":"mock router"})";
        } else {
            // 일반 LLM 호출(response_synth 등) — 사용자 발화를 그대로 echo
            std::string user_text;
            for (const auto& msg : params.messages) {
                if (msg.role == "user") user_text = msg.content;
            }
            result.message.content = "[mock] " + user_text;
        }

        // 스트리밍 콜백이 있으면 전체 내용을 한 번에 전달
        if (on_chunk && !result.message.content.empty()) {
            on_chunk(result.message.content);
        }

        co_return result;
    }

    // 동기 버전 — Provider 기본 구현이 invoke 를 써서 처리하므로 여기서는 사용 안 됨
    // (neograph::Provider 가 complete() 기본 구현 제공)
    std::string get_name() const override { return "jarvis_mock"; }
};

// ─────────────────────────────────────────────────────────────────────────────
// register_custom_node_types() — 자비스 전용 노드 타입을 NodeFactory 에 등록.
//
// NeoGraph 내장 타입("llm_call", "intent_classifier" 등) 은 이미 등록되어 있음.
// 자비스 특화 "intent_classifier" 는 같은 이름으로 다시 등록해서 덮어쓴다.
// (NodeFactory::register_type 은 같은 이름 두 번 등록 시 마지막 등록이 이긴다.)
// ─────────────────────────────────────────────────────────────────────────────

void register_custom_node_types(
    const jarvis::orchestrator::McpCatalog&       catalog,
    const jarvis::orchestrator::AgentDispatcher&  dispatcher,
    std::shared_ptr<neograph::Provider>           router_provider)
{
    using namespace neograph::graph;
    auto& factory = NodeFactory::instance();

    // ── 0) 조건부 엣지(conditional edge) 라우팅 함수 두 개 등록 ─────────────
    //     - jarvis_graph.json 의 라우터 분기에서 사용한다.
    //     - 라우터는 route_decision 채널에 객체 한 덩이를 쓰는데,
    //       NeoGraph 기본 route_channel 은 1차원 값만 다루므로 nested JSON 의
    //       특정 필드를 꺼내는 함수가 따로 필요하다.
    auto& cond_reg = neograph::graph::ConditionRegistry::instance();
    cond_reg.register_condition("route_by_mode",
        [](const GraphState& state) -> std::string {
            auto rd = state.get("route_decision");
            if (rd.is_object() && rd.contains("mode") && rd["mode"].is_string())
                return rd["mode"].get<std::string>();
            return "direct";  // 안전 폴백
        });
    cond_reg.register_condition("route_by_skip_synth",
        [](const GraphState& state) -> std::string {
            auto rd = state.get("route_decision");
            if (rd.is_object() && rd.contains("skip_synthesis"))
                return rd["skip_synthesis"].get<bool>() ? "true" : "false";
            return "false";
        });

    // ── 1) 음성 입력 (mock: stdin) ──────────────────────────────────────────
    factory.register_type("voice_in",
        [](const std::string& name, const neograph::json& cfg, const NodeContext&) {
            return std::make_unique<jarvis::audio::MicInputNode>(name, cfg);
        });

    // ── 2) STT (mock: mock_text 패스스루 + 언어 감지) ────────────────────────
    factory.register_type("whisper_stt",
        [](const std::string& name, const neograph::json& cfg, const NodeContext&) {
            return std::make_unique<jarvis::stt::WhisperSttNode>(name, cfg);
        });

    // ── 3) TTS (mock: 콘솔 출력) ────────────────────────────────────────────
    factory.register_type("supertonic_tts",
        [](const std::string& name, const neograph::json& cfg, const NodeContext&) {
            return std::make_unique<jarvis::audio::SupertonicTtsNode>(name, cfg);
        });

    // ── 4) 메모리 조회 ───────────────────────────────────────────────────────
    factory.register_type("memory_lookup",
        [](const std::string& name, const neograph::json& cfg, const NodeContext&) {
            return std::make_unique<jarvis::memory::MemoryLookupNode>(name, cfg);
        });

    // ── 5) 메모리 저장 ───────────────────────────────────────────────────────
    factory.register_type("memory_commit",
        [](const std::string& name, const neograph::json& cfg, const NodeContext&) {
            return std::make_unique<jarvis::memory::MemoryCommitNode>(name, cfg);
        });

    // ── 6) 의도 라우터 — 자비스 특화 버전으로 내장 intent_classifier 덮어쓰기 ──
    factory.register_type("intent_classifier",
        [&catalog, &dispatcher, router_provider](
            const std::string& name, const neograph::json& cfg, const NodeContext&)
        {
            return std::make_unique<jarvis::orchestrator::IntentRouterNode>(
                name, cfg, catalog, dispatcher, router_provider);
        });

    // ── 7) MCP 도구 단건 호출 (direct 경로) ─────────────────────────────────
    factory.register_type("tool_dispatch",
        [&catalog](const std::string& name, const neograph::json& cfg, const NodeContext&)
        {
            // 인라인 GraphNode — catalog 캡처해서 tool_calls[0] 실행
            class ToolDispatchNode : public neograph::graph::GraphNode {
            public:
                ToolDispatchNode(std::string n,
                                 const neograph::json& c,
                                 const jarvis::orchestrator::McpCatalog& cat)
                    : name_(std::move(n)), cfg_(c), catalog_(cat) {}

                asio::awaitable<neograph::graph::NodeOutput>
                run(neograph::graph::NodeInput in) override {
                    neograph::graph::NodeOutput out;

                    // route_decision 채널에서 첫 번째 tool_call 꺼내기
                    const auto& rd = in.state.get("route_decision");
                    if (!rd.is_object() || !rd.contains("tool_calls") ||
                        !rd["tool_calls"].is_array() || rd["tool_calls"].empty()) {
                        // 도구 호출 0개. 단 — 라우터(IntentRouterNode)가 fallback
                        // 살리기(Fix E) 로 tool_results 를 이미 채워뒀다면 그걸 존중.
                        // append reducer 라서 여기서 또 push 하면 user_text echo 가
                        // 마지막 요소가 되어 salvage 텍스트가 묻힘.
                        const auto& existing = in.state.get("tool_results");
                        if (existing.is_array() && !existing.empty()) {
                            co_return out;  // writes 비움 — 라우터 salvage 그대로 유지
                        }
                        // 라우터도 비웠으면 mock 시연 유지용 user_text echo 로 폴백.
                        const auto& ut = in.state.get("user_text");
                        std::string user_text = ut.is_string() ? ut.get<std::string>() : "";
                        out.writes.push_back({"tool_results",
                            neograph::json::array({neograph::json{{"text", user_text}}})});
                        co_return out;
                    }

                    const auto& call = rd["tool_calls"][0];
                    std::string tool_name = call.value("tool", "");
                    neograph::json args   = call.value("args", neograph::json::object());

                    neograph::Tool* tool = catalog_.find_tool(tool_name);
                    if (!tool) {
                        out.writes.push_back({"tool_results",
                            neograph::json::array({neograph::json{
                                {"error", "tool not found: " + tool_name}}})});
                        co_return out;
                    }

                    neograph::json result;
                    try {
                        result = tool->execute(args);
                    } catch (const std::exception& e) {
                        result = neograph::json{{"error", std::string(e.what())}};
                    }

                    out.writes.push_back({"tool_results",
                        neograph::json::array({neograph::json{
                            {"tool",   tool_name},
                            {"result", result}}})});
                    co_return out;
                }

                std::string get_name() const override { return name_; }

            private:
                std::string name_;
                neograph::json cfg_;
                const jarvis::orchestrator::McpCatalog& catalog_;
            };

            return std::make_unique<ToolDispatchNode>(name, cfg, catalog);
        });

    // ── 8) MCP 도구 병렬 팬아웃 (parallel 경로) ─────────────────────────────
    // mock 구현은 순차 실행으로도 충분 — 실제 구현에서 make_parallel_group 사용
    factory.register_type("parallel_tool_fanout",
        [&catalog](const std::string& name, const neograph::json& cfg, const NodeContext&)
        {
            class ParallelFanoutNode : public neograph::graph::GraphNode {
            public:
                ParallelFanoutNode(std::string n,
                                   const neograph::json& c,
                                   const jarvis::orchestrator::McpCatalog& cat)
                    : name_(std::move(n)), cfg_(c), catalog_(cat) {}

                asio::awaitable<neograph::graph::NodeOutput>
                run(neograph::graph::NodeInput in) override {
                    neograph::graph::NodeOutput out;

                    const auto& rd = in.state.get("route_decision");
                    if (!rd.is_object() || !rd.contains("tool_calls") ||
                        !rd["tool_calls"].is_array()) {
                        out.writes.push_back({"tool_results", neograph::json::array()});
                        co_return out;
                    }

                    neograph::json results = neograph::json::array();

                    // TODO(실제 구현): asio::make_parallel_group 으로 동시 실행
                    for (const auto& call : rd["tool_calls"]) {
                        std::string tool_name = call.value("tool", "");
                        neograph::json args   = call.value("args", neograph::json::object());

                        neograph::Tool* tool = catalog_.find_tool(tool_name);
                        neograph::json result;
                        if (!tool) {
                            result = neograph::json{{"error", "tool not found: " + tool_name}};
                        } else {
                            try {
                                result = tool->execute(args);
                            } catch (const std::exception& e) {
                                result = neograph::json{{"error", std::string(e.what())}};
                            }
                        }
                        results.push_back(neograph::json{
                            {"tool",   tool_name},
                            {"result", result}});
                    }

                    out.writes.push_back({"tool_results", results});
                    co_return out;
                }

                std::string get_name() const override { return name_; }

            private:
                std::string name_;
                neograph::json cfg_;
                const jarvis::orchestrator::McpCatalog& catalog_;
            };

            return std::make_unique<ParallelFanoutNode>(name, cfg, catalog);
        });

    // ── 9) A2A 위임 (delegate 경로) ─────────────────────────────────────────
    factory.register_type("a2a_delegate",
        [&dispatcher](const std::string& name, const neograph::json& cfg, const NodeContext&)
        {
            class A2ADelegateNode : public neograph::graph::GraphNode {
            public:
                A2ADelegateNode(std::string n,
                                const neograph::json& c,
                                jarvis::orchestrator::AgentDispatcher& disp)
                    : name_(std::move(n)), cfg_(c), dispatcher_(disp) {}

                asio::awaitable<neograph::graph::NodeOutput>
                run(neograph::graph::NodeInput in) override {
                    neograph::graph::NodeOutput out;

                    // route_decision.delegate_to 에서 에이전트 이름 꺼내기
                    const auto& rd = in.state.get("route_decision");
                    std::string agent_name;
                    if (rd.is_object() && rd.contains("delegate_to") &&
                        rd["delegate_to"].is_string()) {
                        agent_name = rd["delegate_to"].get<std::string>();
                    }

                    // user_text 꺼내기
                    const auto& ut = in.state.get("user_text");
                    std::string user_text = ut.is_string() ? ut.get<std::string>() : "";

                    auto* entry = dispatcher_.find(agent_name);
                    if (!entry || !entry->client) {
                        // 에이전트 못 찾으면 mock 응답
                        out.writes.push_back({"delegated_reply",
                            "[mock-delegate] 에이전트 '" + agent_name +
                            "' 를 찾을 수 없음. 입력: " + user_text});
                        co_return out;
                    }

                    std::string reply;
                    try {
                        // TODO(실제 구현): co_await 비동기 A2A 호출로 교체
                        neograph::a2a::Task task =
                            entry->client->send_message_sync(user_text);

                        // Task 에서 텍스트 응답 꺼내기
                        // 우선순위: status.message.parts[0].text
                        //           → artifacts[0].parts[0].text
                        //           → "(응답 없음)"
                        if (task.status.message.has_value() &&
                            !task.status.message->parts.empty()) {
                            reply = task.status.message->parts[0].text;
                        } else if (!task.artifacts.empty() &&
                                   !task.artifacts[0].parts.empty()) {
                            reply = task.artifacts[0].parts[0].text;
                        } else {
                            reply = "(a2a 응답 없음)";
                        }
                    } catch (const std::exception& e) {
                        reply = std::string("[a2a 오류] ") + e.what();
                    }

                    out.writes.push_back({"delegated_reply", reply});
                    co_return out;
                }

                std::string get_name() const override { return name_; }

            private:
                std::string name_;
                neograph::json cfg_;
                jarvis::orchestrator::AgentDispatcher& dispatcher_;
            };

            // dispatcher 는 const & 로 받지만 find() 가 non-const 이므로
            // const_cast 사용 (find 가 내부 상태 바꾸지 않음 — 조회 전용)
            return std::make_unique<A2ADelegateNode>(
                name, cfg,
                const_cast<jarvis::orchestrator::AgentDispatcher&>(dispatcher));
        });

    // ── 10) channel_merge — 여러 입력 채널 중 첫 번째 비어있지 않은 값을 output 에 복사 ──
    factory.register_type("channel_merge",
        [](const std::string& name, const neograph::json& cfg, const NodeContext&)
        {
            class ChannelMergeNode : public neograph::graph::GraphNode {
            public:
                ChannelMergeNode(std::string n, const neograph::json& c)
                    : name_(std::move(n)), cfg_(c) {}

                asio::awaitable<neograph::graph::NodeOutput>
                run(neograph::graph::NodeInput in) override {
                    neograph::graph::NodeOutput out;

                    // cfg.inputs: 후보 채널 이름 배열
                    // cfg.output: 결과를 쓸 채널 이름
                    std::string output_ch = cfg_.value("output", std::string(""));
                    if (output_ch.empty() || !cfg_.contains("inputs") ||
                        !cfg_["inputs"].is_array()) {
                        co_return out;
                    }

                    for (const auto& ch_name_j : cfg_["inputs"]) {
                        if (!ch_name_j.is_string()) continue;
                        const std::string ch = ch_name_j.get<std::string>();
                        const auto& val = in.state.get(ch);
                        // null 이 아닌 첫 번째 값 채택
                        if (!val.is_null()) {
                            out.writes.push_back({output_ch, val});
                            co_return out;
                        }
                    }
                    // 모두 null — output 채널에 빈 문자열
                    out.writes.push_back({output_ch, std::string("")});
                    co_return out;
                }

                std::string get_name() const override { return name_; }

            private:
                std::string name_;
                neograph::json cfg_;
            };

            return std::make_unique<ChannelMergeNode>(name, cfg);
        });

    // ── 10.5) llm_call — NeoGraph 내장 덮어쓰기. 자비스 특화 합성기.
    //         내장 llm_call 은 messages 채널을 기대하는데 자비스 그래프에선
    //         아무도 안 채움 → empty messages 400. 직접 user_text + tool_results +
    //         delegated_reply 를 모아 system+user 메시지를 구성한다.
    factory.register_type("llm_call",
        [router_provider](const std::string& name, const neograph::json& cfg, const NodeContext&)
        {
            class JarvisSynthNode : public neograph::graph::GraphNode {
            public:
                JarvisSynthNode(std::string n, neograph::json c,
                                std::shared_ptr<neograph::Provider> p)
                    : name_(std::move(n)), cfg_(std::move(c)), provider_(std::move(p)) {}

                asio::awaitable<neograph::graph::NodeOutput>
                run(neograph::graph::NodeInput in) override {
                    neograph::graph::NodeOutput out;

                    auto get_str = [&](const char* ch, const std::string& dflt = "") {
                        auto v = in.state.get(ch);
                        return v.is_string() ? v.get<std::string>() : dflt;
                    };
                    std::string user_text = get_str("user_text");
                    std::string user_lang = get_str("user_lang", "en");
                    std::string delegated = get_str("delegated_reply");

                    std::string tool_summary;
                    auto tr = in.state.get("tool_results");
                    if (tr.is_array() && !tr.empty()) tool_summary = tr.dump();

                    std::string sys =
                        "You are JARVIS — Tony Stark's terse, witty AI butler. "
                        "Reply in the user's language code (" + user_lang + "). "
                        "One or two sentences max. No markdown, no JSON, plain speech. "
                        "If a tool result is provided, lead with the key fact from it.";
                    std::string usr = "User said: " + user_text + "\n";
                    if (!tool_summary.empty()) usr += "Tool result (JSON): " + tool_summary + "\n";
                    if (!delegated.empty())    usr += "Specialist replied: " + delegated + "\n";

                    neograph::CompletionParams p;
                    p.model       = cfg_.value("model", std::string("gpt-4o"));
                    p.temperature = 0.4f;
                    p.max_tokens  = 220;
                    p.messages.push_back({"system", sys});
                    p.messages.push_back({"user",   usr});

                    auto reply = co_await provider_->invoke(p, nullptr);
                    std::string out_ch = cfg_.value("output_channel",
                                                    std::string("final_text"));
                    out.writes.push_back({out_ch, reply.message.content});
                    co_return out;
                }

                std::string get_name() const override { return name_; }
            private:
                std::string name_;
                neograph::json cfg_;
                std::shared_ptr<neograph::Provider> provider_;
            };
            return std::make_unique<JarvisSynthNode>(name, cfg, router_provider);
        });

    // ── 11) passthrough — cfg.from 채널 값을 cfg.to 채널로 복사 ─────────────
    factory.register_type("passthrough",
        [](const std::string& name, const neograph::json& cfg, const NodeContext&)
        {
            class PassthroughNode : public neograph::graph::GraphNode {
            public:
                PassthroughNode(std::string n, const neograph::json& c)
                    : name_(std::move(n)), cfg_(c) {}

                asio::awaitable<neograph::graph::NodeOutput>
                run(neograph::graph::NodeInput in) override {
                    neograph::graph::NodeOutput out;

                    std::string from_ch = cfg_.value("from", std::string(""));
                    std::string to_ch   = cfg_.value("to",   std::string(""));

                    if (from_ch.empty() || to_ch.empty()) co_return out;

                    const auto& val = in.state.get(from_ch);
                    // 배열인 경우 마지막 요소 꺼내기, 그 외엔 그대로 복사
                    neograph::json elem = (val.is_array() && !val.empty())
                                         ? val[val.size() - 1]
                                         : val;

                    // tool_results 항목이 {"text": "..."} 형태면 문자열만 꺼낸다.
                    // tool_dispatch 가 tool_calls 없을 때 {"text": user_text} 로
                    // 채워주므로, 이 경로를 타면 사용자 발화가 그대로 TTS 로 간다.
                    if (elem.is_object() && elem.contains("text") && elem["text"].is_string()) {
                        elem = elem["text"];
                    }
                    // 마찬가지로 {"result": ...} 형태면 result 값을 꺼낸다.
                    // (실제 도구 결과도 문자열로 직렬화해서 TTS 에 전달)
                    else if (elem.is_object() && elem.contains("result")) {
                        const auto& res = elem["result"];
                        elem = res.is_string() ? res : res.dump();
                    }

                    out.writes.push_back({to_ch, elem});
                    co_return out;
                }

                std::string get_name() const override { return name_; }

            private:
                std::string name_;
                neograph::json cfg_;
            };

            return std::make_unique<PassthroughNode>(name, cfg);
        });
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON 파일 읽기 헬퍼
// ─────────────────────────────────────────────────────────────────────────────

neograph::json load_json_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("JSON 파일 열기 실패: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return neograph::json::parse(ss.str());
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    // .env 파일 자동 로드 (OPENAI_API_KEY 등)
    cppdotenv::auto_load_dotenv();
    install_signal_handlers();

    const std::string config_root =
        (argc >= 2) ? argv[1] : "examples/cookbook/jarvis/config";

    try {
        // ── 1) MCP 카탈로그 로드 ──────────────────────────────────────────────
        std::cerr << "[jarvis] MCP 카탈로그 로드 중...\n";
        auto catalog = jarvis::orchestrator::McpCatalog::load(
            config_root + "/mcp_catalog.json");

        // ── 2) A2A 에이전트 레지스트리 로드 ──────────────────────────────────
        std::cerr << "[jarvis] A2A 에이전트 레지스트리 로드 중...\n";
        auto dispatcher = jarvis::orchestrator::AgentDispatcher::load(
            config_root + "/agent_registry.json");

        // ── 3) LLM Provider 선택 ─────────────────────────────────────────────
        //    OPENAI_API_KEY 환경변수 있으면 실제 OpenAI, 없으면 mock
        std::shared_ptr<neograph::Provider> router_provider;
        std::shared_ptr<neograph::Provider> synth_provider;

        const char* api_key_env = std::getenv("OPENAI_API_KEY");
        if (api_key_env && std::string(api_key_env).size() > 0) {
            std::cerr << "[jarvis] OpenAI Provider 사용 (OPENAI_API_KEY 감지됨)\n";
            neograph::llm::OpenAIProvider::Config pcfg;
            pcfg.api_key      = api_key_env;
            pcfg.default_model = "gpt-4o-mini";
            router_provider = neograph::llm::OpenAIProvider::create_shared(pcfg);

            // 합성기용 — 더 큰 모델 사용
            neograph::llm::OpenAIProvider::Config synth_cfg;
            synth_cfg.api_key      = api_key_env;
            synth_cfg.default_model = "gpt-4o";
            synth_provider = neograph::llm::OpenAIProvider::create_shared(synth_cfg);
        } else {
            std::cerr << "[jarvis] Mock Provider 사용 (OPENAI_API_KEY 없음)\n";
            router_provider = std::make_shared<MockProvider>();
            synth_provider  = std::make_shared<MockProvider>();
        }

        // ── 4) 커스텀 노드 타입 등록 ────────────────────────────────────────
        std::cerr << "[jarvis] 노드 타입 등록 중...\n";
        register_custom_node_types(catalog, dispatcher, router_provider);

        // ── 5) jarvis_graph.json 컴파일 ──────────────────────────────────────
        std::cerr << "[jarvis] 그래프 컴파일 중: "
                  << config_root + "/jarvis_graph.json\n";
        neograph::json graph_def =
            load_json_file(config_root + "/jarvis_graph.json");

        neograph::graph::NodeContext ctx;
        ctx.provider = synth_provider;  // llm_call 노드(response_synth)가 사용

        auto engine_uptr =
            neograph::graph::GraphEngine::compile(graph_def, ctx);
        auto engine =
            std::shared_ptr<neograph::graph::GraphEngine>(engine_uptr.release());

        // ── 5.5) In-memory Store 생성 + 엔진에 주입 ─────────────────────────
        // MemoryLookupNode / MemoryCommitNode 가 ctx.store 를 통해 대화 기록을
        // 읽고 쓸 수 있게 된다. mock 시연이라 영구 저장 불필요 — 프로세스 내
        // 메모리만 사용 (InMemoryStore).
        auto jarvis_store = std::make_shared<neograph::graph::InMemoryStore>();
        engine->set_store(jarvis_store);
        std::cerr << "[jarvis] In-memory Store 주입 완료\n";

        // ── 6) 자비스 A2A self-server 기동 ──────────────────────────────────
        auto self_server = dispatcher.start_self_server(engine);
        if (self_server) {
            std::cerr << "[jarvis] A2A self-server 기동 완료\n";
        }

        // ── 7) (옵션) 백그라운드 트리거 그래프 — 구현 단계에서 추가 예정 ────

        std::cout << "[jarvis] 온라인. 텍스트를 입력하거나 ^C 로 종료.\n";
        std::cout.flush();

        // ── 8) 메인 루프 — 한 턴씩 그래프 실행 ─────────────────────────────
        while (!g_shutdown_requested.load()) {
            neograph::graph::RunConfig run_cfg;
            // thread_id 고정 — 단일 사용자 모드 (multi-user 는 다음 단계)
            run_cfg.thread_id = "jarvis.tony";

            auto result = engine->run(run_cfg);

            // __shutdown__ 채널 true 이면 루프 탈출 (MicInputNode 의 EOF 신호)
            const auto& shutdown_val =
                result.output["channels"]["__shutdown__"]["value"];
            if (shutdown_val.is_boolean() && shutdown_val.get<bool>()) {
                std::cerr << "[jarvis] __shutdown__ 신호 수신 — 루프 종료\n";
                break;
            }
        }

        std::cout << "[jarvis] 종료 중...\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "[jarvis] 치명적 오류: " << e.what() << "\n";
        return 1;
    }
}
