// NeoGraph Example 10: Multi-turn Chatbot with Clay UI + Raylib
//
// NeoGraph Agent backend + Clay layout (C) + Raylib renderer (C).
// C++ code handles only the NeoGraph agent + input processing.
//
// Build: cmake .. -DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON && make example_clay_chatbot
// Run:   ./example_clay_chatbot          (Mock)
//        ./example_clay_chatbot --live   (OpenRouter / DeepSeek)

#include <neograph/neograph.h>
#include "provider_example_support.h"
#include <neograph/llm/agent.h>

#include <cppdotenv/dotenv.hpp>

#include <raylib.h>

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>

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
static std::vector<sp::Message> g_history;
static std::thread g_worker;
static bool g_live = false;

// =========================================================================
// Mock provider
// =========================================================================
class ChatMockProvider : public neograph::Provider {
    struct Fixture {
        std::atomic<std::size_t> turn{0};
        std::shared_ptr<sp::runtime::Client> client = examples::make_local_client();
    };
    std::shared_ptr<Fixture> fixture_ = std::make_shared<Fixture>();
public:
    neograph::PreparedProviderRequest prepare(neograph::ProviderRequest request) override {
        return prepare_local(fixture_->client, std::move(request),
            [fixture = fixture_](const neograph::PreparedProviderRequest& prepared,
                                const std::function<void(const sp::Event&)>& on_event)
                -> asio::awaitable<sp::runtime::Result> {
                static constexpr const char* replies[] = {
                    "Hello! I'm the NeoGraph chatbot.",
                    "NeoGraph is a C++ graph agent engine library.\nIt supports checkpointing, HITL, and parallel execution.",
                    "This UI is built with Clay + Raylib!",
                    "Feel free to ask me anything."};
                const std::string text = replies[fixture->turn.fetch_add(1) % 4];
                sp::Completion completion;
                completion.messages.push_back(examples::message(sp::Role::Assistant, text));
                completion.stop = {sp::StopKind::EndTurn, "stop"};
                if (prepared.mode() == neograph::ProviderMode::Stream && on_event) {
                    on_event(sp::Begin{"clay-local"});
                    on_event(sp::MessageBegin{sp::LocalId{0}});
                    on_event(sp::PartBegin{sp::LocalId{0}, sp::LocalId{0}, sp::PartKind::Text});
                    asio::steady_timer timer(co_await asio::this_coro::executor);
                    for (std::size_t i = 0; i < text.size();) {
                        const unsigned char ch = text[i];
                        const std::size_t len = ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : ch >= 0xC0 ? 2 : 1;
                        on_event(sp::PartDelta{sp::LocalId{0},
                            {sp::PartKind::Text, std::string_view(text).substr(i, len)}});
                        i += len;
                        timer.expires_after(std::chrono::milliseconds(20));
                        co_await timer.async_wait(asio::use_awaitable);
                    }
                    on_event(sp::PartSeal{sp::LocalId{0}, std::nullopt});
                    on_event(sp::MessageSeal{sp::LocalId{0}});
                    on_event(sp::Stop{completion.stop});
                    on_event(sp::Commit{"local-complete"});
                }
                co_return std::make_shared<const sp::Outcome>(std::move(completion));
            });
    }
    std::string get_name() const override { return "clay-local"; }
    std::string_view family() const noexcept override { return "openai.chat"; }
};

// =========================================================================
// Helpers
// =========================================================================
static void send_message() {
    if (g_input_len == 0 || g_generating) return;
    std::string msg(g_input, g_input_len);
    g_input[0] = '\0'; g_input_len = 0;
    { std::lock_guard lock(g_mutex);
      g_messages.push_back({"user", msg, false});
      g_messages.push_back({"assistant", "", true}); }
    g_history.push_back(examples::message(sp::Role::User, msg));
    g_generating = true;

    if (g_worker.joinable()) g_worker.join();
    g_worker = std::thread([]() {
        try {
            auto resp = g_agent->run_stream(g_history,
                [](const sp::Event& event) {
                    const auto* delta = std::get_if<sp::PartDelta>(&event);
                    if (!delta || delta->payload.kind != sp::PartKind::Text ||
                        delta->payload.channel != sp::DeltaChannel::Content) return;
                    std::lock_guard lock(g_mutex);
                    if (!g_messages.empty() && g_messages.back().streaming)
                        g_messages.back().content += delta->payload.bytes;
                });
            auto outcome = examples::require_outcome(std::move(resp));
            std::lock_guard lock(g_mutex);
            if (!g_messages.empty() && g_messages.back().streaming) {
                g_messages.back().content = examples::visible_text(*outcome);
                g_messages.back().streaming = false;
            }
        } catch (const std::exception& error) {
            std::lock_guard lock(g_mutex);
            if (!g_messages.empty() && g_messages.back().streaming) {
                g_messages.back().content = std::string("Error: ") + error.what();
                g_messages.back().streaming = false;
            }
        }
        g_generating = false;
    });
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
    cppdotenv::auto_load_dotenv();

    std::shared_ptr<neograph::Provider> provider;
    if (g_live) {
        const char* key = std::getenv("OPENROUTER_API_KEY");
        if (!key) { fprintf(stderr, "OPENROUTER_API_KEY not set\n"); return 1; }
        provider = examples::make_openrouter_provider(key);
    } else {
        provider = std::make_shared<ChatMockProvider>();
    }

    std::vector<std::unique_ptr<neograph::Tool>> tools;
    neograph::llm::Agent agent(provider, std::move(tools),
        "You are a helpful assistant. Respond concisely in the user's language.",
        g_live ? examples::openrouter_model : "clay-local");
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
    if (g_worker.joinable()) g_worker.join();

    UnloadFont(font);
    CloseWindow();
    clay_cleanup();
    return 0;
}
