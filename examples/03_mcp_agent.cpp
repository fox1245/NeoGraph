// NeoGraph Example 03: Real MCP Agent
//
// 실제 LLM API + MCP 서버 도구를 사용하는 그래프 에이전트.
// .env 파일에서 API 키를 로드하고, MCP 서버에서 도구를 발견하여
// ReAct 그래프를 실행합니다.
//
// 사전 준비:
//   1. .env 파일에 OPENAI_API_KEY 설정
//   2. MCP 서버 실행 (예: python server.py --transport streamable-http --port 8000)
//
// 사용법:
//   ./example_mcp_agent <MCP_SERVER_URL> "<질문>"
//
// 예시:
//   ./example_mcp_agent http://localhost:8000 "강아지 사료 추천해줘"

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/graph/react_graph.h>
#include <neograph/mcp/client.h>

#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>

// .env 파일 로더 — KEY=VALUE 파싱 후 환경변수로 설정
static void load_dotenv(const std::string& path = ".env") {
    std::ifstream file(path);
    if (!file.is_open()) {
        // 상위 디렉토리도 시도 (build/ 에서 실행 시)
        file.open("../" + path);
        if (!file.is_open()) return;
    }

    std::string line;
    while (std::getline(file, line)) {
        // 빈 줄, 주석 건너뛰기
        if (line.empty() || line[0] == '#') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // 앞뒤 공백 제거
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());

        // 따옴표 제거
        if (val.size() >= 2 &&
            ((val.front() == '"' && val.back() == '"') ||
             (val.front() == '\'' && val.back() == '\''))) {
            val = val.substr(1, val.size() - 2);
        }

        setenv(key.c_str(), val.c_str(), 0);  // 0 = 기존 값 덮어쓰지 않음
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <MCP_SERVER_URL> \"<question>\"\n"
                  << "Example: " << argv[0] << " http://localhost:8000 \"강아지 사료 추천해줘\"\n";
        return 1;
    }

    std::string mcp_url = argv[1];
    std::string question = argv[2];

    // 1. .env 로드
    load_dotenv();

    const char* api_key = std::getenv("OPENAI_API_KEY");
    if (!api_key) {
        std::cerr << "Error: OPENAI_API_KEY not set (check .env file)\n";
        return 1;
    }

    // 2. LLM Provider 생성
    auto provider = std::shared_ptr<neograph::Provider>(
        neograph::llm::OpenAIProvider::create({
            .api_key = api_key,
            .default_model = "gpt-4o-mini"
        })
    );

    // 3. MCP 서버에서 도구 발견
    std::cout << "[*] Connecting to MCP server: " << mcp_url << "\n";

    neograph::mcp::MCPClient mcp_client(mcp_url);
    auto tools = mcp_client.get_tools();

    std::cout << "[*] Discovered " << tools.size() << " tools:\n";
    for (const auto& tool : tools) {
        auto def = tool->get_definition();
        std::cout << "    - " << def.name << ": " << def.description.substr(0, 60) << "...\n";
    }
    std::cout << "\n";

    // 4. ReAct 그래프 생성 (LLM + MCP 도구)
    auto engine = neograph::graph::create_react_graph(
        provider,
        std::move(tools),
        "You are a helpful assistant. Use the available tools to answer the user's question. "
        "Always respond in the same language as the user's question."
    );

    // 5. 실행
    neograph::graph::RunConfig config;
    config.input = {{"messages", neograph::json::array({
        {{"role", "user"}, {"content", question}}
    })}};
    config.max_steps = 20;

    std::cout << "User: " << question << "\n\n";
    std::cout << "Assistant: " << std::flush;

    auto result = engine->run_stream(config,
        [](const neograph::graph::GraphEvent& event) {
            switch (event.type) {
                case neograph::graph::GraphEvent::Type::NODE_START:
                    if (event.node_name == "tools") {
                        std::cerr << "\n[tool call in progress...]\n";
                    }
                    break;
                case neograph::graph::GraphEvent::Type::LLM_TOKEN:
                    std::cout << event.data.get<std::string>() << std::flush;
                    break;
                default:
                    break;
            }
        });

    std::cout << "\n\n";
    std::cout << "[*] Execution trace: ";
    for (size_t i = 0; i < result.execution_trace.size(); ++i) {
        std::cout << result.execution_trace[i];
        if (i + 1 < result.execution_trace.size()) std::cout << " → ";
    }
    std::cout << " → END\n";

    return 0;
}
