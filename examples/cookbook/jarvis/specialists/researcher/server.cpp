// jarvis/specialists/researcher/server.cpp
//
// 자비스 위임용 연구자 전문가 — A2A 서버로 노출.
// 자비스 본체의 라우터가 mode="delegate", delegate_to="researcher" 를 결정하면
// 이 서버 (기본 포트 8211) 가 받아서 답한다.
//
// 사용 (run_session.sh 가 자동 띄움):
//   ./cookbook_jarvis_specialist_researcher [port]
//
// 자비스의 agent_registry.json 에 url=http://127.0.0.1:8211 으로 등록되어
// 있어야 라우터가 "delegate_to=researcher" 를 고를 수 있음.
//
// 응답 끝에 [SUMMARY] <≤25 words> 한 줄 — persona.txt 의 specialist 규약.
// 자비스 본체의 response_synth 가 그 한 줄만 추출해서 음성으로 읽음.
//
// 그래프 구성 — 두 단계:
//   1. ResearchNode   : persona.txt [specialist_researcher] 프롬프트로 LLM 호출
//   2. SummaryEnforcer: 응답 끝에 [SUMMARY] 가 없으면 추가 LLM 호출로 강제
//
// 실제 웹 검색 도구(Tavily / Brave / SerpAPI) 는 // TODO 로 두고 mock 진행.
// 외부 API 키 추가 부담 없이 A2A 규약만 검증할 수 있도록.

#include <neograph/neograph.h>
#include <neograph/a2a/server.h>
#include <neograph/llm/openai_provider.h>

#include <cppdotenv/dotenv.hpp>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>

using namespace neograph;
using neograph::graph::ChannelWrite;
using neograph::graph::GraphEngine;
using neograph::graph::GraphNode;
using neograph::graph::GraphState;
using neograph::graph::NodeContext;
using neograph::graph::NodeFactory;
using neograph::graph::NodeInput;
using neograph::graph::NodeOutput;

namespace {

// ---------------------------------------------------------------------------
// 전역 종료 신호 — SIGINT / SIGTERM 잡아서 메인 루프 탈출
// ---------------------------------------------------------------------------
std::atomic<bool> g_shutdown{false};

void on_signal(int) { g_shutdown.store(true, std::memory_order_release); }

// ---------------------------------------------------------------------------
// 파일 전체 읽기 — persona.txt 파싱에 쓰임
// ---------------------------------------------------------------------------
std::string slurp_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("파일 읽기 실패: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// persona.txt 에서 특정 섹션 추출.
// "===<section>==="  라인 다음 줄부터 다음 "===" 헤더 또는 EOF 까지 반환.
// ---------------------------------------------------------------------------
std::string extract_section(const std::string& text, const std::string& section) {
    std::string header = "===" + section + "===";
    auto start = text.find(header);
    if (start == std::string::npos) return "";
    start = text.find('\n', start);
    if (start == std::string::npos) return "";
    ++start; // 헤더 다음 줄 시작

    auto end = text.find("\n===", start);
    if (end == std::string::npos) end = text.size();

    return text.substr(start, end - start);
}

// ---------------------------------------------------------------------------
// MockProvider — OPENAI_API_KEY 없을 때 사용.
// 연구 응답 + [SUMMARY] 규약을 흉내낸다.
// ---------------------------------------------------------------------------
class MockProvider : public Provider {
public:
    ChatCompletion complete(const CompletionParams& /*params*/) override {
        ChatCompletion result;
        result.message.role    = "assistant";
        result.message.content =
            "이건 mock 응답입니다. 실제 연구 결과가 여기에 들어갑니다.\n"
            "\n"
            "[SUMMARY] mock researcher reply — no real data available";
        return result;
    }

    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& /*on_chunk*/) override {
        return complete(params);
    }

    asio::awaitable<ChatCompletion>
    invoke(const CompletionParams& params, StreamCallback /*on_chunk*/) override {
        co_return complete(params);
    }

    std::string get_name() const override { return "mock"; }
};

// ---------------------------------------------------------------------------
// ResearchNode — LLM 한 번 호출. 연구 결과를 `response` 채널에 기록.
// 실제 웹 검색 도구는 TODO 로 비워 둠 (외부 API 키 없이도 동작하도록).
// ---------------------------------------------------------------------------
class ResearchNode : public GraphNode {
public:
    ResearchNode(std::string name,
                 std::shared_ptr<Provider> provider,
                 std::string system_prompt)
        : name_(std::move(name)),
          provider_(std::move(provider)),
          system_prompt_(std::move(system_prompt)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto raw = in.state.get("prompt");
        std::string user_text = raw.is_string() ? raw.get<std::string>() : raw.dump();

        // TODO: 웹 검색 도구 (Tavily / Brave / SerpAPI) 호출 후 결과를
        //       user_text 에 덧붙여 컨텍스트 보강.
        //   예시:
        //   auto search_result = web_search_tool_->execute({{"query", user_text}});
        //   user_text += "\n\n[검색 결과]\n" + search_result;

        CompletionParams p;
        p.model       = provider_->get_name() == "mock" ? "mock" : "gpt-4o-mini";
        p.temperature = 0.3f; // 연구 응답은 정확성 위주 — 온도 낮게
        p.messages.push_back({"system", system_prompt_});
        p.messages.push_back({"user",   user_text});

        auto reply = co_await provider_->invoke(p, nullptr);

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", json(reply.message.content)});
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string              name_;
    std::shared_ptr<Provider> provider_;
    std::string              system_prompt_;
};

// ---------------------------------------------------------------------------
// SummaryEnforcer — `response` 를 읽어서 [SUMMARY] 가 없으면 LLM 을 한 번 더
// 불러 추가한다. 있으면 그냥 통과.
// ---------------------------------------------------------------------------
class SummaryEnforcer : public GraphNode {
public:
    SummaryEnforcer(std::string name,
                    std::shared_ptr<Provider> provider)
        : name_(std::move(name)),
          provider_(std::move(provider)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto raw = in.state.get("response");
        std::string text = raw.is_string() ? raw.get<std::string>() : raw.dump();

        // [SUMMARY] 가 이미 있으면 그대로
        if (text.find("[SUMMARY]") != std::string::npos) {
            NodeOutput out;
            out.writes.push_back(ChannelWrite{"response", json(text)});
            co_return out;
        }

        // 없으면 LLM 에게 25 단어 이하 요약 한 줄 추가 요청
        CompletionParams p;
        p.model       = provider_->get_name() == "mock" ? "mock" : "gpt-4o-mini";
        p.temperature = 0.2f;
        p.messages.push_back({
            "system",
            "You are a summarizer. Given research text, append ONE line at the end: "
            "[SUMMARY] <≤25 words natural-language summary>. "
            "Output the original text unchanged, then the [SUMMARY] line."
        });
        p.messages.push_back({"user", text});

        auto reply = co_await provider_->invoke(p, nullptr);
        std::string enforced = reply.message.content;

        // mock 이 [SUMMARY] 를 또 안 붙였을 때를 대비한 최후 방어
        if (enforced.find("[SUMMARY]") == std::string::npos) {
            enforced += "\n[SUMMARY] research complete";
        }

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", json(enforced)});
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string              name_;
    std::shared_ptr<Provider> provider_;
};

// ---------------------------------------------------------------------------
// 그래프 조립 — research → summary_enforcer → __end__
// ---------------------------------------------------------------------------
std::shared_ptr<GraphEngine> build_engine(std::shared_ptr<Provider> provider,
                                          const std::string&         system_prompt) {
    // research 노드 등록
    NodeFactory::instance().register_type(
        "research",
        [provider, system_prompt](
            const std::string& n, const json&, const NodeContext&) {
            return std::make_unique<ResearchNode>(n, provider, system_prompt);
        });

    // summary_enforcer 노드 등록
    NodeFactory::instance().register_type(
        "summary_enforcer",
        [provider](const std::string& n, const json&, const NodeContext&) {
            return std::make_unique<SummaryEnforcer>(n, provider);
        });

    json def = {
        {"name", "researcher-agent"},
        {"channels", {
            {"prompt",   {{"reducer", "overwrite"}}},
            {"response", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"research",          {{"type", "research"}}},
            {"summary_enforcer",  {{"type", "summary_enforcer"}}},
        }},
        {"edges", json::array({
            json{{"from", "__start__"},        {"to", "research"}},
            json{{"from", "research"},         {"to", "summary_enforcer"}},
            json{{"from", "summary_enforcer"}, {"to", "__end__"}},
        })},
    };

    NodeContext ctx;
    auto        unique_engine =
        GraphEngine::build(def, neograph::graph::EngineConfig{.node_context = ctx});
    return std::shared_ptr<GraphEngine>(std::move(unique_engine));
}

} // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    cppdotenv::auto_load_dotenv();

    int port = (argc >= 2) ? std::stoi(argv[1]) : 8211;

    // persona.txt 경로 — 실행 파일 기준 상대 경로 대신 __FILE__ 기반으로 잡음.
    // 빌드 후 바이너리 위치가 달라져도 소스 옆에 있는 config 를 찾게 됨.
    // 실제 배포 환경에서는 환경변수 JARVIS_CONFIG_DIR 로 덮을 수 있게 해 둠.
    std::string config_dir;
    if (const char* env_dir = std::getenv("JARVIS_CONFIG_DIR")) {
        config_dir = env_dir;
    } else {
        // __FILE__ = .../specialists/researcher/server.cpp
        // config 는 두 단계 위 config/ 아래
        std::string src_path = __FILE__;
        auto slash = src_path.rfind('/');
        slash = (slash != std::string::npos)
                    ? src_path.rfind('/', slash - 1)
                    : std::string::npos;
        slash = (slash != std::string::npos)
                    ? src_path.rfind('/', slash - 1)
                    : std::string::npos;
        config_dir = (slash != std::string::npos)
                         ? src_path.substr(0, slash + 1) + "config"
                         : "config";
    }
    std::string persona_path = config_dir + "/persona.txt";

    // persona.txt 읽기 — 없으면 기본 프롬프트로 대체 (서버 시작은 계속)
    std::string system_prompt;
    try {
        std::string full_text = slurp_file(persona_path);
        system_prompt = extract_section(full_text, "specialist_researcher");
        if (system_prompt.empty()) {
            std::cerr << "[researcher-specialist] 경고: persona.txt 에서 "
                         "===specialist_researcher=== 섹션을 찾지 못함. 기본 프롬프트 사용.\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "[researcher-specialist] 경고: " << e.what()
                  << " — 기본 프롬프트 사용.\n";
    }
    if (system_prompt.empty()) {
        system_prompt =
            "You are a research specialist. Investigate the user's query and "
            "provide a thorough answer. End with [SUMMARY] <≤25 words>.";
    }

    // Provider 분기 — OPENAI_API_KEY 있으면 OpenAI, 없으면 Mock
    std::shared_ptr<Provider> provider;
    const char* api_key = std::getenv("OPENAI_API_KEY");
    if (api_key && *api_key) {
        llm::OpenAIProvider::Config cfg;
        cfg.api_key       = api_key;
        cfg.default_model = "gpt-4o-mini";
        provider = llm::OpenAIProvider::create_shared(cfg);
        std::cout << "[researcher-specialist] OpenAI provider (gpt-4o-mini)\n";
    } else {
        provider = std::make_shared<MockProvider>();
        std::cout << "[researcher-specialist] OPENAI_API_KEY 없음 — mock provider 사용\n";
    }

    // 그래프 조립
    auto engine = build_engine(provider, system_prompt);

    // AgentCard — A2A 클라이언트가 /.well-known/agent-card.json 으로 발견
    a2a::AgentCard card;
    card.name                = "researcher";
    card.description         = "JARVIS researcher specialist — web search + summary delegation target";
    card.url                 = "http://127.0.0.1:" + std::to_string(port) + "/";
    card.version             = "0.1.0";
    card.protocol_version    = "0.3.0";
    card.preferred_transport = "JSONRPC";
    card.default_input_modes  = {"text/plain"};
    card.default_output_modes = {"text/plain"};
    card.skill_names = {"research", "summarize", "compare", "analyze"};

    // A2AServer 시작
    a2a::A2AServer server(engine, card);
    if (!server.start_async("127.0.0.1", port)) {
        std::cerr << "[researcher-specialist] 포트 바인드 실패: 127.0.0.1:" << port << "\n";
        return 1;
    }

    std::cout << "[researcher-specialist] online @ 127.0.0.1:" << server.port() << "\n";

    // 종료 신호 등록
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    // 신호 올 때까지 대기
    while (!g_shutdown.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    std::cout << "[researcher-specialist] 종료\n";
    return 0;
}
