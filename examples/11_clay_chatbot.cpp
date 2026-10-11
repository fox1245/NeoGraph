// NeoGraph Example 11: Multi-turn Chatbot with Clay UI + Raylib
//
// NeoGraph Agent backend + Clay layout (C) + Raylib renderer (C).
// C++ code handles the NeoGraph agent, keyboard input and fonts; the window
// itself is laid out in clay_impl.c.
//
// Build: cmake .. -DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON && make example_clay_chatbot
// Run:   ./example_clay_chatbot          (Mock)
//        ./example_clay_chatbot --live   (OpenRouter / DeepSeek)
//
// Text uses a system TrueType/OpenType font (Hangul and other scripts
// included when the font has them). Set NEOGRAPH_CHAT_FONT, and optionally
// NEOGRAPH_CHAT_BOLD_FONT / NEOGRAPH_CHAT_MONO_FONT, to choose the files;
// "path.ttc#2" selects a face inside a font collection.
//
// Keys: Enter send · Esc clear · Ctrl+Backspace delete word · Ctrl+V paste ·
//       Ctrl+Q quit · mouse wheel scroll. While a reply streams, Send becomes Stop.

#include <neograph/neograph.h>
#include "provider_example_support.h"
#include <neograph/llm/agent.h>
#include <neograph/graph/cancel.h>

#include <cppdotenv/dotenv.hpp>

#include <raylib.h>
#include "clay_chat_ui.h"

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// =========================================================================
// State
// =========================================================================
struct ChatEntry {
    std::string content;
    bool is_user = false;
    bool streaming = false;
    bool failed = false;
    bool stopped = false;
    std::string time;
};

static std::vector<ChatEntry> g_messages;  // guarded by g_mutex
static std::mutex g_mutex;
static std::string g_input;                // main thread only
static std::atomic<bool> g_generating{false};
static neograph::llm::Agent* g_agent = nullptr;
static std::vector<sp::Message> g_history;
static std::thread g_worker;
static std::shared_ptr<neograph::graph::CancelToken> g_cancel;  // current reply
static bool g_live = false;

static constexpr std::size_t kInputLimit = 4000;

static std::string clock_label() {
    std::time_t now = std::time(nullptr);
    char buf[8];
    std::strftime(buf, sizeof buf, "%H:%M", std::localtime(&now));
    return buf;
}

// =========================================================================
// Mock provider
// =========================================================================
// Streams a canned reply chosen from the latest user message, a few
// characters at a time, so the UI can be tried without a key.
class ChatMockProvider : public neograph::Provider {
    struct Fixture {
        std::shared_ptr<sp::runtime::Client> client = examples::make_local_client();
    };
    std::shared_ptr<Fixture> fixture_ = std::make_shared<Fixture>();

    static std::string reply_for(const std::string& prompt) {
        auto has = [&](const char* word) { return prompt.find(word) != std::string::npos; };
        for (unsigned char c : prompt)
            if (c >= 0xE0) return "안녕하세요! 저는 NeoGraph 챗봇이에요. 이 창은 Clay와 Raylib로 그려지고, "
                                  "답변은 NeoGraph 에이전트에서 스트리밍됩니다.";
        if (has("C++") || has("graph") || has("code"))
            return "A graph is a definition plus a node context:\n\n"
                   "```cpp\n"
                   "auto engine = neograph::graph::GraphEngine::build(definition, config);\n"
                   "auto result = engine->run(run_config);\n"
                   "```\n\n"
                   "Nodes can be your own C++ callables or built-ins like `llm_call`.";
        if (has("NeoGraph") || has("do"))
            return "NeoGraph is a C++ graph agent engine. Highlights:\n\n"
                   "- **Checkpointing** with resume after a crash\n"
                   "- Human-in-the-loop interrupts\n"
                   "- Parallel fan-out with `Send`\n"
                   "- Streaming tokens straight into a UI like this one";
        return "Hello! I'm the NeoGraph chatbot. Ask me what NeoGraph can do, or for a tiny graph in C++.";
    }

    static std::string last_user_text(const neograph::ProviderRequest& request) {
        const auto& messages = examples::request_messages(request);
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->role != sp::Role::User) continue;
            std::string text;
            for (const auto& part : it->parts)
                if (const auto* t = std::get_if<sp::Text>(&part)) text += t->value;
            return text;
        }
        return {};
    }

public:
    neograph::PreparedProviderRequest prepare(neograph::ProviderRequest request) override {
        std::string text = reply_for(last_user_text(request));
        auto cancel = request.cancel_token;  // Stop and quit cancel the reply mid-stream
        return prepare_local(fixture_->client, std::move(request),
            [text = std::move(text), cancel](const neograph::PreparedProviderRequest& prepared,
                                     const std::function<void(const sp::Event&)>& on_event)
                -> asio::awaitable<sp::runtime::Result> {
                sp::Completion completion;
                completion.messages.push_back(examples::message(sp::Role::Assistant, text));
                completion.stop = {sp::StopKind::EndTurn, "stop"};
                if (prepared.mode() == neograph::ProviderMode::Stream && on_event) {
                    on_event(sp::Begin{"clay-local"});
                    on_event(sp::MessageBegin{sp::LocalId{0}});
                    on_event(sp::PartBegin{sp::LocalId{0}, sp::LocalId{0}, sp::PartKind::Text});
                    asio::steady_timer timer(co_await asio::this_coro::executor);
                    for (std::size_t i = 0; i < text.size();) {
                        if (cancel) cancel->throw_if_cancelled("reply stopped");
                        const unsigned char ch = text[i];
                        const std::size_t len = ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : ch >= 0xC0 ? 2 : 1;
                        on_event(sp::PartDelta{sp::LocalId{0},
                            {sp::PartKind::Text, std::string_view(text).substr(i, len)}});
                        i += len;
                        timer.expires_after(std::chrono::milliseconds(18));
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
// Fonts
// =========================================================================
// Raylib rasterizes only the codepoints it is given, so the atlases are rebuilt
// whenever text on screen needs a glyph they do not have yet.
class ChatFonts {
public:
    void discover() {
        regular_ = load_face("NEOGRAPH_CHAT_FONT",
            {"sans-serif:lang=ko", "sans-serif"},
            {"C:/Windows/Fonts/malgun.ttf", "C:/Windows/Fonts/segoeui.ttf",
             "/System/Library/Fonts/Supplemental/AppleGothic.ttf", "/Library/Fonts/Arial Unicode.ttf",
             "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
             "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"});
        bold_ = load_face("NEOGRAPH_CHAT_BOLD_FONT",
            {"sans-serif:lang=ko:weight=bold", "sans-serif:weight=bold"},
            {"C:/Windows/Fonts/malgunbd.ttf", "C:/Windows/Fonts/segoeuib.ttf",
             "/usr/share/fonts/truetype/nanum/NanumGothicBold.ttf",
             "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
             "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"});
        mono_ = load_face("NEOGRAPH_CHAT_MONO_FONT", {"monospace"},
            {"C:/Windows/Fonts/consola.ttf", "/System/Library/Fonts/Supplemental/Courier New.ttf",
             "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"});
        if (bold_.empty()) bold_ = regular_;
        if (mono_.empty()) mono_ = regular_;
        if (regular_.empty())
            std::fprintf(stderr, "No TrueType font found; using Raylib's built-in font (ASCII only). "
                                 "Set NEOGRAPH_CHAT_FONT=/path/to/font.ttf for other scripts.\n");
        for (int c = 32; c < 127; ++c) codepoints_.insert(c);
    }

    // Marks the atlases stale if `text` uses a codepoint they lack.
    void need(const char* text, std::size_t len) {
        for (std::size_t i = 0; i < len;) {
            int size = 1;
            int cp = GetCodepointNext(text + i, &size);
            if (cp >= 32 && codepoints_.insert(cp).second) dirty_ = true;
            i += size > 0 ? size : 1;
        }
    }

    // Rebuilds at most ten times a second while a reply streams in new glyphs.
    void refresh() {
        if (!dirty_ || (loaded_ && GetTime() - built_at_ < 0.1)) return;
        unload();
        std::vector<int> cps(codepoints_.begin(), codepoints_.end());
        fonts_[CHAT_FONT_BODY] = rasterize(regular_, CHAT_PX_BODY, cps);
        fonts_[CHAT_FONT_SMALL] = rasterize(regular_, CHAT_PX_SMALL, cps);
        fonts_[CHAT_FONT_TITLE] = rasterize(bold_, CHAT_PX_TITLE, cps);
        fonts_[CHAT_FONT_DISPLAY] = rasterize(bold_, CHAT_PX_DISPLAY, cps);
        fonts_[CHAT_FONT_MONO] = rasterize(mono_, CHAT_PX_MONO, cps);
        clay_chat_set_fonts(fonts_);
        dirty_ = false;
        loaded_ = true;
        built_at_ = GetTime();
    }

    void unload() {
        if (!loaded_) return;
        for (Font& font : fonts_)
            if (font.texture.id != GetFontDefault().texture.id) UnloadFont(font);
        loaded_ = false;
    }

private:
    static Font rasterize(const std::vector<unsigned char>& face, int px, std::vector<int>& cps) {
        if (face.empty()) return GetFontDefault();
        Font font = LoadFontFromMemory(".ttf", face.data(), (int)face.size(), px, cps.data(), (int)cps.size());
        if (font.glyphCount == 0 || font.texture.id == 0) return GetFontDefault();
        SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
        return font;
    }

    static std::uint32_t be32(const unsigned char* p) {
        return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
    }

    // Raylib reads a single font, not a collection. Table offsets in a
    // collection are file-absolute and the face directories precede all table
    // data, so moving the chosen face's directory to the front is enough.
    static bool select_collection_face(std::vector<unsigned char>& data, unsigned index) {
        if (data.size() < 12 || std::memcmp(data.data(), "ttcf", 4) != 0) return true;
        const std::uint32_t faces = be32(&data[8]);
        if (index >= faces || 12 + 4ull * faces > data.size()) return false;
        const std::uint32_t dir = be32(&data[12 + 4 * index]);
        if (dir + 12ull > data.size()) return false;
        const std::uint32_t tables = (std::uint32_t(data[dir + 4]) << 8) | data[dir + 5];
        const std::uint64_t dir_size = 12 + 16ull * tables;
        if (dir + dir_size > data.size()) return false;
        for (std::uint32_t t = 0; t < tables; ++t)
            if (be32(&data[dir + 12 + 16 * t + 8]) < dir_size) return false;
        std::memmove(data.data(), data.data() + dir, dir_size);
        return true;
    }

    // "file" or "file#index" -> font bytes ready for Raylib, or empty.
    static std::vector<unsigned char> read_face(const std::string& spec) {
        std::string path = spec;
        unsigned index = 0;
        if (auto hash = spec.rfind('#'); hash != std::string::npos) {
            path = spec.substr(0, hash);
            index = (unsigned)std::strtoul(spec.c_str() + hash + 1, nullptr, 10);
        }
        std::ifstream in(path, std::ios::binary);
        std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (data.empty() || !select_collection_face(data, index)) return {};
        return data;
    }

    static std::string fc_match(const char* pattern) {
#ifndef _WIN32
        std::string cmd = std::string("fc-match -f '%{file}#%{index}' '") + pattern + "' 2>/dev/null";
        if (FILE* pipe = popen(cmd.c_str(), "r")) {
            char buf[1024] = {};
            std::size_t n = std::fread(buf, 1, sizeof buf - 1, pipe);
            pclose(pipe);
            return std::string(buf, n);
        }
#else
        (void)pattern;
#endif
        return {};
    }

    static std::vector<unsigned char> load_face(const char* env, std::initializer_list<const char*> patterns,
                                                std::initializer_list<const char*> candidates) {
        if (const char* chosen = std::getenv(env); chosen && *chosen) {
            auto face = read_face(chosen);
            if (face.empty()) std::fprintf(stderr, "%s=%s could not be loaded\n", env, chosen);
            return face;
        }
        for (const char* pattern : patterns)
            if (auto face = read_face(fc_match(pattern)); !face.empty()) return face;
        for (const char* path : candidates)
            if (auto face = read_face(path); !face.empty()) return face;
        return {};
    }

    std::vector<unsigned char> regular_, bold_, mono_;
    std::set<int> codepoints_;
    Font fonts_[CHAT_FONT_COUNT] = {};
    bool dirty_ = true;
    bool loaded_ = false;
    double built_at_ = 0;
};

// =========================================================================
// Sending and input
// =========================================================================
static void send_message(std::string text) {
    if (text.empty() || g_generating) return;
    const std::string now = clock_label();
    { std::lock_guard lock(g_mutex);
      g_messages.push_back({text, true, false, false, false, now});
      g_messages.push_back({"", false, true, false, false, now}); }
    g_history.push_back(examples::message(sp::Role::User, text));
    g_generating = true;
    clay_chat_stick_to_bottom();

    if (g_worker.joinable()) g_worker.join();
    g_cancel = std::make_shared<neograph::graph::CancelToken>();
    g_worker = std::thread([token = g_cancel]() {
        neograph::ToolExecutionContext execution;
        execution.cancel_token = token;  // also aborts the in-flight provider call
        try {
            auto resp = g_agent->run_stream(g_history,
                [](const sp::Event& event) {
                    const auto* delta = std::get_if<sp::PartDelta>(&event);
                    if (!delta || delta->payload.kind != sp::PartKind::Text ||
                        delta->payload.channel != sp::DeltaChannel::Content) return;
                    std::lock_guard lock(g_mutex);
                    if (!g_messages.empty() && g_messages.back().streaming)
                        g_messages.back().content += delta->payload.bytes;
                }, 10, execution);
            auto outcome = examples::require_outcome(std::move(resp));
            std::lock_guard lock(g_mutex);
            if (!g_messages.empty() && g_messages.back().streaming) {
                g_messages.back().content = examples::visible_text(*outcome);
                g_messages.back().streaming = false;
            }
        } catch (const std::exception& error) {
            std::lock_guard lock(g_mutex);
            if (!g_messages.empty() && g_messages.back().streaming) {
                ChatEntry& reply = g_messages.back();
                reply.streaming = false;
                if (token->is_cancelled()) {
                    reply.stopped = true;  // keep what arrived
                } else {
                    reply.content = std::string("Error: ") + error.what();
                    reply.failed = true;
                }
            }
        }
        g_generating = false;
    });
}

static void stop_reply() {
    if (g_generating && g_cancel) g_cancel->cancel();
}

static void erase_codepoint() {
    while (!g_input.empty()) {
        const bool continuation = (static_cast<unsigned char>(g_input.back()) & 0xC0) == 0x80;
        g_input.pop_back();
        if (!continuation) break;
    }
}

static void append_text(const char* text) {
    for (const char* p = text; *p && g_input.size() < kInputLimit; ++p)
        g_input += (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
}

// Returns false when the user asked to quit.
static bool handle_keys() {
    for (int ch = GetCharPressed(); ch > 0; ch = GetCharPressed()) {
        if (ch < 32 || g_input.size() >= kInputLimit) continue;
        int size = 0;
        const char* utf8 = CodepointToUTF8(ch, &size);
        g_input.append(utf8, size);
    }
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (ctrl) {
            while (!g_input.empty() && g_input.back() == ' ') g_input.pop_back();
            while (!g_input.empty() && g_input.back() != ' ') erase_codepoint();
        } else {
            erase_codepoint();
        }
    }
    if (IsKeyPressed(KEY_ESCAPE)) g_input.clear();
    if (ctrl && IsKeyPressed(KEY_V)) if (const char* clip = GetClipboardText()) append_text(clip);
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        if (!g_generating && !g_input.empty()) send_message(std::exchange(g_input, {}));
    }
    return !(ctrl && IsKeyPressed(KEY_Q));
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
        if (!key || !*key) { std::fprintf(stderr, "OPENROUTER_API_KEY not set\n"); return 1; }
        provider = examples::make_openrouter_provider(key);
    } else {
        provider = std::make_shared<ChatMockProvider>();
    }

    std::vector<std::unique_ptr<neograph::Tool>> tools;
    neograph::llm::Agent agent(provider, std::move(tools),
        "You are a helpful assistant. Respond concisely in the user's language.",
        g_live ? examples::openrouter_model : "clay-local");
    g_agent = &agent;
    const std::string model_label = g_live ? std::string(examples::openrouter_model) + " via OpenRouter"
                                           : std::string("Local mock provider");

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(980, 760, g_live ? "NeoGraph Chat (Live)" : "NeoGraph Chat (Mock)");
    SetWindowMinSize(560, 440);
    SetExitKey(KEY_NULL);  // Esc clears the input instead of closing the window
    SetTargetFPS(60);

    clay_chat_init(GetScreenWidth(), GetScreenHeight());
    ChatFonts fonts;
    fonts.discover();
    const char* ui_text = clay_chat_ui_text();
    fonts.need(ui_text, std::strlen(ui_text));

    std::vector<ChatEntry> frame;
    std::vector<ChatMessageView> views;
    while (!WindowShouldClose()) {
        if (!handle_keys()) break;

        { std::lock_guard lock(g_mutex); frame = g_messages; }
        for (const ChatEntry& m : frame) fonts.need(m.content.data(), m.content.size());
        fonts.need(g_input.data(), g_input.size());
        fonts.refresh();

        views.clear();
        for (const ChatEntry& m : frame)
            views.push_back({m.content.data(), (int)m.content.size(), m.is_user, m.streaming, m.failed,
                             m.stopped, m.time.c_str()});
        ChatViewState state{views.data(), (int)views.size(), g_input.data(), (int)g_input.size(),
                            g_generating ? 1 : 0, g_live ? "LIVE" : "MOCK", model_label.c_str(), GetTime()};

        int suggestion = -1;
        switch (clay_chat_update(&state, &suggestion)) {
            case CHAT_ACTION_SEND:
                if (!g_generating && !g_input.empty()) send_message(std::exchange(g_input, {}));
                break;
            case CHAT_ACTION_STOP: stop_reply(); break;
            case CHAT_ACTION_SUGGESTION: send_message(clay_chat_suggestion(suggestion)); break;
            default: break;
        }
        clay_chat_layout(&state);

        BeginDrawing();
        ClearBackground((Color){14, 16, 21, 255});
        clay_chat_render();
        EndDrawing();
    }
    stop_reply();
    if (g_worker.joinable()) g_worker.join();

    fonts.unload();
    CloseWindow();
    clay_chat_shutdown();
    return 0;
}
