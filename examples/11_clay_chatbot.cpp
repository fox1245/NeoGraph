// NeoGraph Example 10: Multi-turn Chatbot with Clay UI + Raylib
//
// NeoGraph Agent backend + Clay layout (C) + Raylib renderer (C).
// C++ code handles only the NeoGraph agent + input processing.
//
// Build: cmake .. -DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON && make example_clay_chatbot
// Run:   ./example_clay_chatbot          (Mock)
//        ./example_clay_chatbot --live   (OpenAI API)

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/llm/agent.h>

#include <raylib.h>

#include <string>
#include <vector>
#include <fstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>

// =========================================================================
// C functions from clay_impl.c
// =========================================================================
extern "C" {
    void clay_init(int screen_w, int screen_h);
    void clay_cleanup(void);
    void clay_set_font(Font font);
    void clay_set_messages(const char* roles[], const char* contents[],
                           int content_lens[], int streaming[], int count);
    void clay_set_input(const char* text, int len);
    void clay_set_config(int is_live, float screen_width, double time_val);
    void clay_update(void);
    void clay_build_layout(void);
    void clay_render(void);
}

// =========================================================================
// State
// =========================================================================
struct ChatBubble { std::string role, content; bool streaming; };

static std::vector<ChatBubble> g_messages;
static char g_input[1024] = {};
static int g_input_len = 0;
static std::mutex g_mutex;
static std::atomic<bool> g_generating{false};
static neograph::llm::Agent* g_agent = nullptr;
static std::vector<neograph::ChatMessage> g_history;
static bool g_live = false;

// =========================================================================
// Mock provider
// =========================================================================
class ChatMockProvider : public neograph::Provider {
    int turn_ = 0;
public:
    neograph::ChatCompletion complete(const neograph::CompletionParams&) override {
        const char* r[] = {
            "Hello! I'm the NeoGraph chatbot.",
            "NeoGraph is a C++ graph agent engine library.\nIt supports checkpointing, HITL, and parallel execution.",
            "This UI is built with Clay + Raylib!",
            "Feel free to ask me anything."};
        neograph::ChatCompletion c;
        c.message.role = "assistant";
        c.message.content = r[turn_++ % 4];
        return c;
    }
    neograph::ChatCompletion complete_stream(
        const neograph::CompletionParams& p, const neograph::StreamCallback& cb) override {
        auto c = complete(p);
        for (size_t i = 0; i < c.message.content.size(); ) {
            unsigned char ch = c.message.content[i];
            int len = (ch>=0xF0)?4:(ch>=0xE0)?3:(ch>=0xC0)?2:1;
            std::string tok = c.message.content.substr(i, len);
            { std::lock_guard lock(g_mutex);
              if (!g_messages.empty() && g_messages.back().streaming)
                  g_messages.back().content += tok; }
            i += len;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return c;
    }
    std::string get_name() const override { return "mock"; }
};

// =========================================================================
// Helpers
// =========================================================================
static void load_dotenv() {
    for (auto p : {"../.env", ".env"}) {
        std::ifstream f(p); if (!f) continue;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0]=='#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            setenv(line.substr(0,eq).c_str(), line.substr(eq+1).c_str(), 0);
        }
        break;
    }
}

static void send_message() {
    if (g_input_len == 0 || g_generating) return;
    std::string msg(g_input, g_input_len);
    g_input[0] = '\0'; g_input_len = 0;
    { std::lock_guard lock(g_mutex);
      g_messages.push_back({"user", msg, false});
      g_messages.push_back({"assistant", "", true}); }
    g_history.push_back({"user", msg});
    g_generating = true;

    std::thread([=]() {
        try {
            auto resp = g_agent->run_stream(g_history,
                [](const std::string& tok) {
                    std::lock_guard lock(g_mutex);
                    if (!g_messages.empty() && g_messages.back().streaming)
                        g_messages.back().content += tok;
                });
            std::lock_guard lock(g_mutex);
            if (!g_messages.empty() && g_messages.back().streaming) {
                g_messages.back().content = resp;
                g_messages.back().streaming = false;
            }
        } catch (...) {}
        g_generating = false;
    }).detach();
}

static void sync_to_clay() {
    std::lock_guard lock(g_mutex);
    int n = (int)g_messages.size();
    if (n == 0) {
        clay_set_messages(nullptr, nullptr, nullptr, nullptr, 0);
    } else {
        static std::vector<const char*> roles, contents;
        static std::vector<int> lens, streams;
        roles.resize(n); contents.resize(n); lens.resize(n); streams.resize(n);
        for (int i = 0; i < n; i++) {
            roles[i] = g_messages[i].role.c_str();
            contents[i] = g_messages[i].content.c_str();
            lens[i] = (int)g_messages[i].content.size();
            streams[i] = g_messages[i].streaming ? 1 : 0;
        }
        clay_set_messages(roles.data(), contents.data(), lens.data(), streams.data(), n);
    }
    clay_set_input(g_input, g_input_len);
    clay_set_config(g_live ? 1 : 0, (float)GetScreenWidth(), GetTime());
}

// =========================================================================
// Main
// =========================================================================
int main(int argc, char** argv) {
    g_live = (argc > 1 && std::string(argv[1]) == "--live");
    load_dotenv();

    std::shared_ptr<neograph::Provider> provider;
    if (g_live) {
        const char* key = std::getenv("OPENAI_API_KEY");
        if (!key) { fprintf(stderr, "OPENAI_API_KEY not set\n"); return 1; }
        provider = neograph::llm::OpenAIProvider::create({.api_key = key, .default_model = "gpt-4o-mini"});
    } else {
        provider = std::make_shared<ChatMockProvider>();
    }

    std::vector<std::unique_ptr<neograph::Tool>> tools;
    neograph::llm::Agent agent(provider, std::move(tools),
        "You are a helpful assistant. Respond concisely in the user's language.");
    g_agent = &agent;

    // Raylib
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(800, 600, g_live ? "NeoGraph Chat (Live)" : "NeoGraph Chat (Mock)");
    SetTargetFPS(60);

    // Use Raylib default font (external TTF has rendering issues with Raylib 5.5)
    // TODO: Switch to LoadFontEx when Raylib fixes GRAY_ALPHA texture rendering
    Font font = GetFontDefault();

    // Clay
    clay_init(GetScreenWidth(), GetScreenHeight());
    clay_set_font(font);

    while (!WindowShouldClose()) {
        // Input
        int ch = GetCharPressed();
        while (ch > 0) {
            if (ch >= 32 && g_input_len < 1000) {
                if (ch<0x80) g_input[g_input_len++]=(char)ch;
                else if (ch<0x800) {
                    g_input[g_input_len++]=0xC0|(ch>>6);
                    g_input[g_input_len++]=0x80|(ch&0x3F);
                } else {
                    g_input[g_input_len++]=0xE0|(ch>>12);
                    g_input[g_input_len++]=0x80|((ch>>6)&0x3F);
                    g_input[g_input_len++]=0x80|(ch&0x3F);
                }
                g_input[g_input_len] = '\0';
            }
            ch = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && g_input_len > 0) {
            g_input_len--;
            while (g_input_len > 0 && (g_input[g_input_len] & 0xC0) == 0x80) g_input_len--;
            g_input[g_input_len] = '\0';
        }
        if (IsKeyPressed(KEY_ENTER)) send_message();

        // Sync state → Clay update → Layout → Render
        sync_to_clay();
        clay_update();
        clay_build_layout();

        BeginDrawing();
        ClearBackground((Color){25, 25, 35, 255});
        clay_render();
        EndDrawing();
    }

    UnloadFont(font);
    CloseWindow();
    clay_cleanup();
    return 0;
}
