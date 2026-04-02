// NeoGraph Example 10: Multi-turn Chatbot with Clay UI + Raylib
//
// NeoGraph Agent 백엔드 + Clay 레이아웃(C) + Raylib 렌더러.
// Clay 레이아웃은 clay_impl.c (C99)에서 정의하고, C++에서 호출합니다.
//
// 빌드: cmake .. -DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON && make example_clay_chatbot
// 실행: ./example_clay_chatbot          (Mock)
//       ./example_clay_chatbot --live   (OpenAI API)

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/llm/agent.h>

#include <raylib.h>

// Clay types needed for rendering — forward-declared from clay.h
// (Full Clay header is C99 only; macros & layout live in clay_impl.c)
extern "C" {

typedef struct { float r, g, b, a; } Clay_Color;
typedef struct { float topLeft, topRight, bottomLeft, bottomRight; } Clay_CornerRadius;
typedef struct { bool isStaticallyAllocated; int32_t length; const char *chars; } Clay_String;
typedef struct { Clay_String stringContents; Clay_Color textColor; uint16_t fontId; uint16_t fontSize; uint16_t letterSpacing; uint16_t lineHeight; } Clay_TextRenderData;
typedef struct { Clay_Color backgroundColor; Clay_CornerRadius cornerRadius; } Clay_RectangleRenderData;
typedef struct { bool horizontal; bool vertical; } Clay_ClipRenderData;
typedef struct { Clay_Color color; } Clay_OverlayColorRenderData;
typedef struct { Clay_Color color; Clay_CornerRadius cornerRadius; struct { uint16_t left,right,top,bottom,betweenChildren; } width; } Clay_BorderRenderData;
typedef struct { Clay_Color backgroundColor; Clay_CornerRadius cornerRadius; void* imageData; } Clay_ImageRenderData;
typedef struct { Clay_Color backgroundColor; Clay_CornerRadius cornerRadius; void* customData; } Clay_CustomRenderData;
typedef union {
    Clay_RectangleRenderData rectangle;
    Clay_TextRenderData text;
    Clay_ImageRenderData image;
    Clay_CustomRenderData custom;
    Clay_BorderRenderData border;
    Clay_ClipRenderData clip;
    Clay_OverlayColorRenderData overlayColor;
} Clay_RenderData;
typedef struct { float x, y, width, height; } Clay_BoundingBox;
typedef struct { float x, y; } Clay_Vector2;
typedef struct { float width, height; } Clay_Dimensions;
typedef struct { uintptr_t nextAllocation; size_t capacity; char* memory; } Clay_Arena;
typedef struct { void (*errorHandlerFunction)(void*); void* userData; } Clay_ErrorHandler;

typedef enum {
    CLAY_RENDER_COMMAND_TYPE_NONE,
    CLAY_RENDER_COMMAND_TYPE_RECTANGLE,
    CLAY_RENDER_COMMAND_TYPE_BORDER,
    CLAY_RENDER_COMMAND_TYPE_TEXT,
    CLAY_RENDER_COMMAND_TYPE_IMAGE,
    CLAY_RENDER_COMMAND_TYPE_SCISSOR_START,
    CLAY_RENDER_COMMAND_TYPE_SCISSOR_END,
    CLAY_RENDER_COMMAND_TYPE_CUSTOM,
    CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_START,
    CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_END,
} Clay_RenderCommandType;

typedef struct { Clay_BoundingBox boundingBox; Clay_RenderData renderData; Clay_RenderCommandType commandType; uint32_t id; int16_t zIndex; } Clay_RenderCommand;
typedef struct { int32_t capacity; int32_t length; Clay_RenderCommand *internalArray; } Clay_RenderCommandArray;

typedef struct { int32_t length; const char* chars; const char* baseChars; } Clay_StringSlice;
typedef struct { Clay_Color textColor; uint16_t fontId; uint16_t fontSize; uint16_t letterSpacing; uint16_t lineHeight; int wrapMode; } Clay_TextElementConfig;

Clay_RenderCommand* Clay_RenderCommandArray_Get(Clay_RenderCommandArray* array, int32_t index);
typedef struct Clay_Context Clay_Context;
uint32_t Clay_MinMemorySize(void);
Clay_Arena Clay_CreateArenaWithCapacityAndMemory(size_t capacity, void *memory);
Clay_Context* Clay_Initialize(Clay_Arena arena, Clay_Dimensions dimensions, Clay_ErrorHandler errorHandler);
void Clay_SetMeasureTextFunction(Clay_Dimensions (*measureText)(Clay_StringSlice, Clay_TextElementConfig*, void*), void* userData);
void Clay_SetLayoutDimensions(Clay_Dimensions dimensions);
void Clay_SetPointerState(Clay_Vector2 position, bool pointerDown);
void Clay_UpdateScrollContainers(bool enableDragScrolling, Clay_Vector2 scrollDelta, float deltaTime);
Clay_RenderCommandArray Clay_EndLayout(float deltaTime);

} // extern "C"

#include <string>
#include <vector>
#include <fstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cstring>

// =========================================================================
// C functions defined in clay_impl.c
// =========================================================================
extern "C" {
    void clay_set_messages(const char* roles[], const char* contents[],
                           int content_lens[], int streaming[], int count);
    void clay_set_input(const char* text, int len);
    void clay_set_config(int is_live, float screen_width, double time);
    void clay_build_layout(void);
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
static Font g_font;
static bool g_live = false;

// =========================================================================
// Mock provider
// =========================================================================
class ChatMockProvider : public neograph::Provider {
    int turn_ = 0;
public:
    neograph::ChatCompletion complete(const neograph::CompletionParams&) override {
        const char* r[] = {
            "안녕하세요! NeoGraph 챗봇입니다.",
            "NeoGraph는 C++ 그래프 에이전트 엔진입니다.\n체크포인팅, HITL, 병렬 실행을 지원합니다.",
            "Clay + Raylib으로 이런 UI를 만들 수 있습니다!",
            "더 궁금한 점이 있으면 물어보세요."};
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

// =========================================================================
// Clay text measurement (called from C side via function pointer)
// =========================================================================
static Clay_Dimensions MeasureText(Clay_StringSlice text, Clay_TextElementConfig* cfg, void*) {
    float scale = (float)cfg->fontSize / (float)g_font.baseSize;
    float w = 0, maxW = 0; int lines = 1;
    for (int i = 0; i < (int)text.length; i++) {
        if (text.chars[i] == '\n') { maxW = std::max(maxW, w); w = 0; lines++; continue; }
        int idx = GetGlyphIndex(g_font, (unsigned char)text.chars[i]);
        float adv = g_font.glyphs[idx].advanceX ? (float)g_font.glyphs[idx].advanceX : g_font.recs[idx].width;
        w += adv * scale;
    }
    return {std::max(maxW, w), (float)(lines * cfg->fontSize * 1.25f)};
}

// =========================================================================
// Raylib renderer
// =========================================================================
static void RenderClay(Clay_RenderCommandArray cmds) {
    for (int i = 0; i < (int)cmds.length; i++) {
        auto* cmd = Clay_RenderCommandArray_Get(&cmds, i);
        auto& b = cmd->boundingBox;
        switch (cmd->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
            auto& d = cmd->renderData.rectangle;
            Color c = {(uint8_t)d.backgroundColor.r,(uint8_t)d.backgroundColor.g,
                       (uint8_t)d.backgroundColor.b,(uint8_t)d.backgroundColor.a};
            float r = d.cornerRadius.topLeft;
            if (r > 0.5f) DrawRectangleRounded({b.x,b.y,b.width,b.height}, r/(std::min(b.width,b.height)*0.5f), 6, c);
            else DrawRectangle((int)b.x,(int)b.y,(int)b.width,(int)b.height, c);
            break; }
        case CLAY_RENDER_COMMAND_TYPE_TEXT: {
            auto& d = cmd->renderData.text;
            Color c = {(uint8_t)d.textColor.r,(uint8_t)d.textColor.g,
                       (uint8_t)d.textColor.b,(uint8_t)d.textColor.a};
            std::string txt(d.stringContents.chars, d.stringContents.length);
            DrawTextEx(g_font, txt.c_str(), {b.x,b.y}, (float)d.fontSize, 1, c);
            break; }
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
            BeginScissorMode((int)b.x,(int)b.y,(int)b.width,(int)b.height); break;
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
            EndScissorMode(); break;
        default: break;
        }
    }
}

// =========================================================================
// Sync state from C++ to C layout
// =========================================================================
static void sync_to_clay() {
    std::lock_guard lock(g_mutex);
    int n = (int)g_messages.size();
    if (n == 0) {
        clay_set_messages(nullptr, nullptr, nullptr, nullptr, 0);
        clay_set_input(g_input, g_input_len);
        return;
    }

    // Build flat arrays for C
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
    clay_set_input(g_input, g_input_len);
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

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(800, 600, g_live ? "NeoGraph Chat (Live)" : "NeoGraph Chat (Mock)");
    SetTargetFPS(60);

    g_font = LoadFontEx("/usr/share/fonts/opentype/noto/NotoSansCJK-Light.ttc", 20, nullptr, 0);
    if (g_font.glyphCount == 0) g_font = GetFontDefault();
    SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);

    uint64_t memSz = Clay_MinMemorySize();
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(memSz, malloc(memSz));
    Clay_Initialize(arena, {(float)GetScreenWidth(), (float)GetScreenHeight()}, (Clay_ErrorHandler){});
    Clay_SetMeasureTextFunction(MeasureText, nullptr);

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        Clay_SetLayoutDimensions({(float)GetScreenWidth(), (float)GetScreenHeight()});
        Clay_SetPointerState({(float)GetMouseX(), (float)GetMouseY()}, IsMouseButtonDown(MOUSE_BUTTON_LEFT));
        Clay_UpdateScrollContainers(true, {0, GetMouseWheelMove() * 50}, dt);

        // Keyboard
        int ch = GetCharPressed();
        while (ch > 0) {
            if (ch >= 32 && g_input_len < 1000) {
                if (ch<0x80) g_input[g_input_len++]=(char)ch;
                else if (ch<0x800) { g_input[g_input_len++]=0xC0|(ch>>6); g_input[g_input_len++]=0x80|(ch&0x3F); }
                else { g_input[g_input_len++]=0xE0|(ch>>12); g_input[g_input_len++]=0x80|((ch>>6)&0x3F); g_input[g_input_len++]=0x80|(ch&0x3F); }
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

        // Sync → Layout → Render
        sync_to_clay();
        clay_set_config(g_live ? 1 : 0, (float)GetScreenWidth(), GetTime());
        clay_build_layout();
        Clay_RenderCommandArray cmds = Clay_EndLayout(dt);

        BeginDrawing();
        ClearBackground({25,25,35,255});
        RenderClay(cmds);
        EndDrawing();
    }

    UnloadFont(g_font);
    CloseWindow();
    free(arena.memory);
    return 0;
}
