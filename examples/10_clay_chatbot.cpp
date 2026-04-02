// NeoGraph Example 10: Multi-turn Chatbot with Clay UI + Raylib
//
// NeoGraph 에이전트를 백엔드로, Clay 레이아웃 + Raylib 렌더러로
// 시각적 멀티턴 챗봇을 구현합니다.
//
// 사전 준비:
//   Raylib은 CMake FetchContent로 자동 다운로드됩니다.
//   .env에 OPENAI_API_KEY 설정 (또는 Mock 모드로 실행)
//
// Usage:
//   ./example_clay_chatbot              # Mock 모드 (API 키 불필요)
//   ./example_clay_chatbot --live       # 실제 OpenAI API 사용
//
// 조작:
//   텍스트 입력 후 Enter → 메시지 전송
//   마우스 휠 → 스크롤
//   ESC → 종료

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/llm/agent.h>
#include <neograph/graph/react_graph.h>

#include <raylib.h>

#include <string>
#include <vector>
#include <cstring>
#include <fstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>

// =========================================================================
// Clay (C library) — compile as C via extern "C" wrapper
// =========================================================================
extern "C" {
#define CLAY_IMPLEMENTATION
#include <clay.h>
}

// =========================================================================
// Chat message structure
// =========================================================================
struct ChatBubble {
    std::string role;       // "user" or "assistant"
    std::string content;
    bool        streaming;  // true while assistant is still typing
};

// =========================================================================
// Global state
// =========================================================================
static std::vector<ChatBubble> g_messages;
static char g_input[1024] = {};
static int  g_input_len = 0;
static float g_scroll_y = 0.0f;
static std::mutex g_mutex;
static std::atomic<bool> g_generating{false};

// Agent (set in main)
static neograph::llm::Agent* g_agent = nullptr;
static std::vector<neograph::ChatMessage> g_history;

// =========================================================================
// Mock provider for offline mode
// =========================================================================
class ChatMockProvider : public neograph::Provider {
    int turn_ = 0;
    static constexpr const char* responses[] = {
        "안녕하세요! NeoGraph 챗봇입니다. 무엇이든 물어보세요.",
        "NeoGraph는 C++로 작성된 그래프 에이전트 엔진 라이브러리입니다.\n"
        "LangGraph의 C++ 대안으로, 체크포인팅, HITL, 병렬 실행을 지원합니다.",
        "네, Clay는 C 기반 즉시모드 UI 레이아웃 라이브러리이고,\n"
        "Raylib으로 렌더링하여 이런 챗봇 UI를 만들 수 있습니다!",
        "더 궁금한 점이 있으시면 편하게 물어보세요!",
    };
public:
    neograph::ChatCompletion complete(const neograph::CompletionParams&) override {
        neograph::ChatCompletion r;
        r.message.role = "assistant";
        r.message.content = responses[turn_ % 4];
        turn_++;
        return r;
    }
    neograph::ChatCompletion complete_stream(
        const neograph::CompletionParams& p,
        const neograph::StreamCallback& cb) override {
        auto r = complete(p);
        // Simulate token-by-token streaming
        for (size_t i = 0; i < r.message.content.size(); ) {
            // Handle UTF-8 multi-byte characters
            unsigned char c = r.message.content[i];
            int len = 1;
            if (c >= 0xF0) len = 4;
            else if (c >= 0xE0) len = 3;
            else if (c >= 0xC0) len = 2;

            std::string token = r.message.content.substr(i, len);
            if (cb) cb(token);

            // Update streaming content
            {
                std::lock_guard lock(g_mutex);
                if (!g_messages.empty() && g_messages.back().streaming) {
                    g_messages.back().content += token;
                }
            }

            i += len;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        return r;
    }
    std::string get_name() const override { return "mock"; }
};

// =========================================================================
// .env loader
// =========================================================================
static void load_dotenv(const std::string& path = ".env") {
    std::ifstream file(path);
    if (!file.is_open()) file.open("../" + path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        setenv(key.c_str(), val.c_str(), 0);
    }
}

// =========================================================================
// Send message (runs agent in background thread)
// =========================================================================
static void send_message() {
    if (g_input_len == 0 || g_generating) return;

    std::string user_msg(g_input, g_input_len);
    g_input[0] = '\0';
    g_input_len = 0;

    {
        std::lock_guard lock(g_mutex);
        g_messages.push_back({"user", user_msg, false});
        g_messages.push_back({"assistant", "", true});  // streaming placeholder
    }

    g_history.push_back({"user", user_msg});
    g_generating = true;

    std::thread([user_msg]() {
        try {
            auto response = g_agent->run_stream(g_history,
                [](const std::string& token) {
                    // Streaming is handled inside MockProvider or real provider
                    // For real provider, update here:
                    std::lock_guard lock(g_mutex);
                    if (!g_messages.empty() && g_messages.back().streaming) {
                        g_messages.back().content += token;
                    }
                });

            std::lock_guard lock(g_mutex);
            if (!g_messages.empty() && g_messages.back().streaming) {
                g_messages.back().content = response;
                g_messages.back().streaming = false;
            }
        } catch (const std::exception& e) {
            std::lock_guard lock(g_mutex);
            if (!g_messages.empty() && g_messages.back().streaming) {
                g_messages.back().content = std::string("[Error] ") + e.what();
                g_messages.back().streaming = false;
            }
        }
        g_generating = false;
    }).detach();
}

// =========================================================================
// Clay text measurement callback (required by Clay)
// =========================================================================
static Clay_Dimensions MeasureText(Clay_StringSlice text, Clay_TextElementConfig* config, void* userData) {
    int fontSize = config->fontSize;
    Font font = *(Font*)userData;

    float width = 0;
    float maxWidth = 0;
    int lines = 1;

    for (int i = 0; i < (int)text.length; i++) {
        unsigned char c = text.chars[i];
        if (c == '\n') {
            maxWidth = std::max(maxWidth, width);
            width = 0;
            lines++;
            continue;
        }
        int idx = GetGlyphIndex(font, c);
        if (font.glyphs[idx].advanceX)
            width += font.glyphs[idx].advanceX;
        else
            width += font.recs[idx].width;
    }
    maxWidth = std::max(maxWidth, width);

    return {maxWidth * fontSize / (float)font.baseSize,
            (float)(lines * fontSize * 1.2f)};
}

// =========================================================================
// Raylib renderer for Clay commands
// =========================================================================
static void RenderClay(Clay_RenderCommandArray commands, Font font) {
    for (int i = 0; i < (int)commands.length; i++) {
        auto* cmd = Clay_RenderCommandArray_Get(&commands, i);
        auto bb = cmd->boundingBox;

        switch (cmd->commandType) {
            case CYCLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                auto* cfg = cmd->renderData.rectangle;
                Color col = {(unsigned char)cfg->backgroundColor.r,
                             (unsigned char)cfg->backgroundColor.g,
                             (unsigned char)cfg->backgroundColor.b,
                             (unsigned char)cfg->backgroundColor.a};
                float radius = cfg->cornerRadius.topLeft;
                if (radius > 0) {
                    DrawRectangleRounded(
                        {bb.x, bb.y, bb.width, bb.height},
                        radius / (std::min(bb.width, bb.height) * 0.5f),
                        8, col);
                } else {
                    DrawRectangle((int)bb.x, (int)bb.y,
                                  (int)bb.width, (int)bb.height, col);
                }
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                auto* cfg = cmd->renderData.text;
                Color col = {(unsigned char)cfg->textColor.r,
                             (unsigned char)cfg->textColor.g,
                             (unsigned char)cfg->textColor.b,
                             (unsigned char)cfg->textColor.a};
                // Draw text with word wrap handled by Clay layout
                std::string text(cfg->stringContents.chars, cfg->stringContents.length);
                DrawTextEx(font, text.c_str(),
                           {bb.x, bb.y},
                           (float)cfg->fontSize,
                           1.0f, col);
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
                BeginScissorMode((int)bb.x, (int)bb.y,
                                 (int)bb.width, (int)bb.height);
                break;
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
                EndScissorMode();
                break;
            default:
                break;
        }
    }
}

// =========================================================================
// Colors
// =========================================================================
static constexpr Clay_Color BG_DARK   = {25, 25, 35, 255};
static constexpr Clay_Color BG_PANEL  = {35, 35, 50, 255};
static constexpr Clay_Color BG_USER   = {55, 90, 200, 255};
static constexpr Clay_Color BG_ASSIST = {50, 55, 65, 255};
static constexpr Clay_Color BG_INPUT  = {45, 45, 60, 255};
static constexpr Clay_Color TEXT_WHITE = {240, 240, 245, 255};
static constexpr Clay_Color TEXT_GRAY  = {160, 165, 180, 255};
static constexpr Clay_Color TEXT_HINT  = {100, 105, 120, 255};
static constexpr Clay_Color ACCENT    = {80, 140, 255, 255};

// =========================================================================
// Main
// =========================================================================
int main(int argc, char** argv) {
    bool live_mode = (argc > 1 && std::string(argv[1]) == "--live");

    load_dotenv();

    // Create provider
    std::shared_ptr<neograph::Provider> provider;
    if (live_mode) {
        const char* key = std::getenv("OPENAI_API_KEY");
        if (!key) {
            TraceLog(LOG_ERROR, "OPENAI_API_KEY not set");
            return 1;
        }
        provider = neograph::llm::OpenAIProvider::create({
            .api_key = key, .default_model = "gpt-4o-mini"
        });
    } else {
        provider = std::make_shared<ChatMockProvider>();
    }

    // Create agent
    std::vector<std::unique_ptr<neograph::Tool>> tools;
    neograph::llm::Agent agent(provider, std::move(tools),
        "You are a helpful AI assistant. Respond concisely in the same language as the user.");
    g_agent = &agent;

    // Raylib init
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(800, 600, "NeoGraph Chatbot — Clay UI");
    SetTargetFPS(60);

    // Font
    Font font = LoadFontEx("/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", 20, nullptr, 0);
    if (font.glyphCount == 0) font = GetFontDefault();
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);

    // Clay init
    uint64_t clayMemSize = Clay_MinMemorySize();
    Clay_Arena clayArena = Clay_CreateArenaWithCapacityAndMemory(
        clayMemSize, malloc(clayMemSize));
    Clay_Initialize(clayArena,
        (Clay_Dimensions){(float)GetScreenWidth(), (float)GetScreenHeight()},
        (Clay_ErrorHandler){0});
    Clay_SetMeasureTextFunction(MeasureText, &font);

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        int sw = GetScreenWidth();
        int sh = GetScreenHeight();

        // Input handling
        Clay_SetLayoutDimensions((Clay_Dimensions){(float)sw, (float)sh});
        Clay_SetPointerState(
            (Clay_Vector2){(float)GetMouseX(), (float)GetMouseY()},
            IsMouseButtonDown(MOUSE_BUTTON_LEFT));
        Clay_UpdateScrollContainers(true,
            (Clay_Vector2){0, GetMouseWheelMove() * 40}, dt);

        // Keyboard input
        int key = GetCharPressed();
        while (key > 0) {
            if (key >= 32 && g_input_len < 1000) {
                // UTF-8 encode
                if (key < 0x80) {
                    g_input[g_input_len++] = (char)key;
                } else if (key < 0x800) {
                    g_input[g_input_len++] = (char)(0xC0 | (key >> 6));
                    g_input[g_input_len++] = (char)(0x80 | (key & 0x3F));
                } else if (key < 0x10000) {
                    g_input[g_input_len++] = (char)(0xE0 | (key >> 12));
                    g_input[g_input_len++] = (char)(0x80 | ((key >> 6) & 0x3F));
                    g_input[g_input_len++] = (char)(0x80 | (key & 0x3F));
                }
                g_input[g_input_len] = '\0';
            }
            key = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && g_input_len > 0) {
            // Handle UTF-8 backspace
            g_input_len--;
            while (g_input_len > 0 && (g_input[g_input_len] & 0xC0) == 0x80)
                g_input_len--;
            g_input[g_input_len] = '\0';
        }
        if (IsKeyPressed(KEY_ENTER)) {
            send_message();
        }

        // ── Clay Layout ──
        Clay_BeginLayout();

        // Root container
        CLAY({
            .id = CLAY_ID("Root"),
            .layout = {
                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                .padding = CLAY_PADDING_ALL(12),
                .childGap = 8
            },
            .backgroundColor = BG_DARK
        }) {
            // Header
            CLAY({
                .id = CLAY_ID("Header"),
                .layout = {
                    .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(40)},
                    .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}
                },
                .backgroundColor = BG_PANEL,
                .cornerRadius = CLAY_CORNER_RADIUS(8)
            }) {
                std::string title = live_mode
                    ? "NeoGraph Chatbot (Live — gpt-4o-mini)"
                    : "NeoGraph Chatbot (Mock Mode)";
                CLAY_TEXT(CLAY_STRING(title.c_str()), CLAY_TEXT_CONFIG({
                    .textColor = ACCENT, .fontSize = 16
                }));
            }

            // Messages area (scrollable)
            CLAY({
                .id = CLAY_ID("Messages"),
                .layout = {
                    .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .padding = CLAY_PADDING_ALL(8),
                    .childGap = 8
                },
                .backgroundColor = BG_PANEL,
                .cornerRadius = CLAY_CORNER_RADIUS(8),
                .scroll = {.vertical = true}
            }) {
                std::lock_guard lock(g_mutex);
                for (size_t i = 0; i < g_messages.size(); i++) {
                    auto& msg = g_messages[i];
                    bool is_user = (msg.role == "user");

                    CLAY({
                        .id = CLAY_IDI("MsgRow", (uint32_t)i),
                        .layout = {
                            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                            .childAlignment = {
                                is_user ? CLAY_ALIGN_X_RIGHT : CLAY_ALIGN_X_LEFT,
                                CLAY_ALIGN_Y_TOP
                            }
                        }
                    }) {
                        CLAY({
                            .id = CLAY_IDI("Bubble", (uint32_t)i),
                            .layout = {
                                .sizing = {CLAY_SIZING_FIT(80, (float)sw * 0.7f), CLAY_SIZING_FIT(0)},
                                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                .padding = {12, 12, 8, 10},
                                .childGap = 4
                            },
                            .backgroundColor = is_user ? BG_USER : BG_ASSIST,
                            .cornerRadius = CLAY_CORNER_RADIUS(12)
                        }) {
                            // Role label
                            const char* label = is_user ? "You" : "Assistant";
                            CLAY_TEXT(CLAY_STRING(label), CLAY_TEXT_CONFIG({
                                .textColor = TEXT_GRAY, .fontSize = 11
                            }));

                            // Content
                            std::string display = msg.content;
                            if (msg.streaming && ((int)(GetTime() * 3) % 2))
                                display += "_";
                            if (display.empty()) display = "...";
                            CLAY_TEXT(CLAY_STRING(display.c_str()), CLAY_TEXT_CONFIG({
                                .textColor = TEXT_WHITE, .fontSize = 14,
                                .wrapMode = CLAY_TEXT_WRAP_WORDS
                            }));
                        }
                    }
                }
            }

            // Input bar
            CLAY({
                .id = CLAY_ID("InputBar"),
                .layout = {
                    .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(44)},
                    .padding = {12, 12, 8, 8},
                    .childGap = 8,
                    .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}
                },
                .backgroundColor = BG_INPUT,
                .cornerRadius = CLAY_CORNER_RADIUS(8)
            }) {
                std::string display_input = g_input_len > 0
                    ? std::string(g_input)
                    : "메시지를 입력하세요...";
                Clay_Color text_color = g_input_len > 0 ? TEXT_WHITE : TEXT_HINT;

                CLAY_TEXT(CLAY_STRING(display_input.c_str()), CLAY_TEXT_CONFIG({
                    .textColor = text_color, .fontSize = 14
                }));
            }
        }

        Clay_RenderCommandArray commands = Clay_EndLayout();

        // ── Render ──
        BeginDrawing();
        ClearBackground({25, 25, 35, 255});
        RenderClay(commands, font);
        EndDrawing();
    }

    UnloadFont(font);
    CloseWindow();
    free(clayArena.memory);

    return 0;
}
