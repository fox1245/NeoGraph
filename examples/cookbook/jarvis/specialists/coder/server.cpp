// jarvis/specialists/coder/server.cpp
//
// 자비스 위임용 코더 전문가 — A2A 서버로 노출.
//
// 자비스 라우터가 mode="delegate", delegate_to="coder" 를 결정하면
// A2AClient 가 이 서버(기본 port 8210)에 user_text 를 통째로 던진다.
// PersonaNode 하나짜리 그래프가 LLM 응답을 만들고, 그 뒤
// SummaryEnforcerNode 가 마지막 줄에 [SUMMARY] 한 줄을 강제 삽입한다.
// 자비스 본체의 response_synth 는 그 [SUMMARY] 줄만 음성으로 읽는다.
//
// 사용:
//   ./cookbook_jarvis_specialist_coder [port]    (기본값 8210)
//
// .env 또는 환경변수에 OPENAI_API_KEY 가 없으면 MockProvider 로 대체.
//
// ai-assembly/member_server.cpp 와 동일한 패턴으로 구성.

#include <neograph/neograph.h>
#include <neograph/a2a/server.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/node.h>
#include <neograph/graph/loader.h>

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
#include <vector>

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

std::atomic<bool> g_shutdown{false};

void on_signal(int) { g_shutdown.store(true, std::memory_order_release); }

// ──────────────────────────────────────────────────────────────────────────
// API 키 없을 때 쓰는 간단한 Mock Provider
// ──────────────────────────────────────────────────────────────────────────
class MockProvider : public Provider {
public:
    ChatCompletion complete(const CompletionParams& params) override {
        // 사용자 메시지를 그대로 echo 하고 [SUMMARY] 를 붙여 돌려줌
        std::string user_text;
        for (auto& m : params.messages) {
            if (m.role == "user") { user_text = m.content; break; }
        }
        ChatCompletion result;
        result.message.role    = "assistant";
        result.message.content = "[mock coder] " + user_text
                                 + "\n[SUMMARY] mock coder reply";
        return result;
    }

    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override {
        auto result = complete(params);
        if (on_chunk && !result.message.content.empty())
            on_chunk(result.message.content);
        return result;
    }

    std::string get_name() const override { return "mock"; }
};

// ──────────────────────────────────────────────────────────────────────────
// persona.txt 에서 ===<section>=== 블록을 꺼내는 함수
// ──────────────────────────────────────────────────────────────────────────
std::string extract_persona_section(const std::string& file_path,
                                    const std::string& section_name) {
    std::ifstream f(file_path);
    if (!f) return "";          // 파일 없으면 빈 문자열 — 에러로 죽이지 않음

    const std::string marker = "===" + section_name + "===";
    std::string line;
    bool in_section = false;
    std::ostringstream buf;

    while (std::getline(f, line)) {
        if (!in_section) {
            if (line == marker) { in_section = true; }
            continue;
        }
        // 다음 === 헤더를 만나면 종료
        if (line.size() >= 3 && line[0] == '=' && line[1] == '=' && line[2] == '=')
            break;
        buf << line << '\n';
    }
    return buf.str();
}

// ──────────────────────────────────────────────────────────────────────────
// PersonaNode — LLM 에 system prompt(코더 페르소나) 를 입히고
//               user prompt(= "prompt" 채널 값) 를 던져 응답을 받음
// ──────────────────────────────────────────────────────────────────────────
class PersonaNode : public GraphNode {
public:
    PersonaNode(std::string node_name,
                std::shared_ptr<Provider> provider,
                std::string system_prompt)
        : name_(std::move(node_name)),
          provider_(std::move(provider)),
          system_prompt_(std::move(system_prompt)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto raw = in.state.get("prompt");
        std::string user_text = raw.is_string() ? raw.get<std::string>() : raw.dump();

        CompletionParams p;
        p.model       = "gpt-4o-mini";
        p.temperature = 0.7f;
        p.messages.push_back({"system", system_prompt_});
        p.messages.push_back({"user",   user_text});

        auto reply = co_await provider_->invoke(p, nullptr);

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", json(reply.message.content)});
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string             name_;
    std::shared_ptr<Provider> provider_;
    std::string             system_prompt_;
};

// ──────────────────────────────────────────────────────────────────────────
// SummaryEnforcerNode — LLM 응답 끝에 [SUMMARY] 줄이 없으면 강제 삽입
//   - 있으면: 그대로 통과
//   - 없으면: 응답 본문의 앞 25 단어를 요약으로 붙임
// ──────────────────────────────────────────────────────────────────────────
class SummaryEnforcerNode : public GraphNode {
public:
    explicit SummaryEnforcerNode(std::string node_name)
        : name_(std::move(node_name)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto raw = in.state.get("response");
        std::string text = raw.is_string() ? raw.get<std::string>() : raw.dump();

        // 마지막 줄이 [SUMMARY] 로 시작하는지 확인
        if (text.find("[SUMMARY]") == std::string::npos) {
            // 앞 25 단어 뽑아서 요약 줄로 붙임
            std::istringstream iss(text);
            std::string word;
            std::vector<std::string> words;
            while (iss >> word && words.size() < 25)
                words.push_back(word);

            std::string summary;
            for (size_t i = 0; i < words.size(); ++i) {
                if (i) summary += ' ';
                summary += words[i];
            }
            // 마침표로 끝나지 않으면 추가
            if (!summary.empty() && summary.back() != '.')
                summary += '.';

            text += "\n[SUMMARY] " + summary;
        }

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", json(text)});
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

}  // namespace

// ──────────────────────────────────────────────────────────────────────────
// main
// ──────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    // .env 로드 (OPENAI_API_KEY 등)
    cppdotenv::auto_load_dotenv();

    int port = (argc >= 2) ? std::atoi(argv[1]) : 8210;

    // persona.txt 에서 코더 전문가 시스템 프롬프트 꺼내기
    // 실행 파일 옆 ../../config/persona.txt 를 기본 경로로 시도
    const std::string persona_file =
        std::string(argv[0]).substr(0, std::string(argv[0]).rfind('/') + 1)
        + "../../config/persona.txt";

    std::string system_prompt = extract_persona_section(persona_file, "specialist_coder");
    if (system_prompt.empty()) {
        // 빌드 트리 위치와 다를 수 있으므로 소스 기준 경로도 시도.
        // 환경변수 JARVIS_CONFIG_DIR 가 있으면 우선, 없으면 __FILE__ 기준으로
        // 두 단계 위 config/ 를 찾는다 (researcher/server.cpp 와 동일 규약).
        std::string config_dir;
        if (const char* env_dir = std::getenv("JARVIS_CONFIG_DIR")) {
            config_dir = env_dir;
        } else {
            // __FILE__ = .../specialists/coder/server.cpp
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
        system_prompt = extract_persona_section(
            config_dir + "/persona.txt", "specialist_coder");
    }
    if (system_prompt.empty()) {
        // 끝까지 못 읽으면 최소 fallback
        system_prompt =
            "You are the coder specialist behind JARVIS. "
            "End your reply with a [SUMMARY] line of ≤25 words.";
        std::cerr << "[coder-specialist] persona.txt 를 찾지 못해 기본 프롬프트 사용\n";
    }

    // Provider 결정: API 키 있으면 OpenAI, 없으면 Mock
    std::shared_ptr<Provider> provider;
    const char* api_key = std::getenv("OPENAI_API_KEY");
    if (api_key && *api_key) {
        llm::OpenAIProvider::Config cfg;
        cfg.api_key       = api_key;
        cfg.default_model = "gpt-4o-mini";
        provider = llm::OpenAIProvider::create_shared(cfg);
    } else {
        std::cerr << "[coder-specialist] OPENAI_API_KEY 없음 — MockProvider 로 동작\n";
        provider = std::make_shared<MockProvider>();
    }

    // 노드 타입 등록 — PersonaNode
    NodeFactory::instance().register_type(
        "coder_persona",
        [provider, system_prompt](
            const std::string& n, const json&, const NodeContext&) {
            return std::make_unique<PersonaNode>(n, provider, system_prompt);
        });

    // 노드 타입 등록 — SummaryEnforcerNode
    NodeFactory::instance().register_type(
        "summary_enforcer",
        [](const std::string& n, const json&, const NodeContext&) {
            return std::make_unique<SummaryEnforcerNode>(n);
        });

    // 그래프 정의: persona → summary_enforcer 2단계
    json def = {
        {"name", "coder-specialist"},
        {"channels", {
            {"prompt",   {{"reducer", "overwrite"}}},
            {"response", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"persona",          {{"type", "coder_persona"}}},
            {"summary_enforcer", {{"type", "summary_enforcer"}}},
        }},
        {"edges", json::array({
            json{{"from", "__start__"},        {"to", "persona"}},
            json{{"from", "persona"},          {"to", "summary_enforcer"}},
            json{{"from", "summary_enforcer"}, {"to", "__end__"}},
        })},
    };

    NodeContext ctx;
    auto unique_engine = GraphEngine::compile(def, ctx);
    auto engine = std::shared_ptr<GraphEngine>(std::move(unique_engine));

    // AgentCard — 자비스 라우터가 "coder" 이름으로 찾는다
    a2a::AgentCard card;
    card.name                = "coder";
    card.description         = "JARVIS coder specialist — code writing/review/debug delegation target";
    card.url                 = "http://127.0.0.1:" + std::to_string(port) + "/";
    card.version             = "0.1.0";
    card.protocol_version    = "0.3.0";
    card.preferred_transport = "JSONRPC";
    card.default_input_modes  = {"text/plain"};
    card.default_output_modes = {"text/plain"};
    card.skill_names = {"code", "debug", "review", "refactor"};

    a2a::A2AServer server(engine, card);
    if (!server.start_async("127.0.0.1", port)) {
        std::cerr << "[coder-specialist] 바인드 실패 — 127.0.0.1:" << port << "\n";
        return 1;
    }

    std::cout << "[coder-specialist] online @ 127.0.0.1:" << server.port() << "\n";

    // SIGINT / SIGTERM 잡아서 graceful shutdown
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    while (!g_shutdown.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    std::cout << "[coder-specialist] 종료\n";
    return 0;
}
