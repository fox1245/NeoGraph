// Clay implementation + layout + rendering — compiled as C99
// C++ side (10_clay_chatbot.cpp) only handles NeoGraph agent + input.

#define CLAY_IMPLEMENTATION
#include <clay.h>
#include <raylib.h>
#include <string.h>
#include <stdio.h>

// =========================================================================
// Shared state (set by C++ before each frame)
// =========================================================================
typedef struct {
    const char* role;
    const char* content;
    int content_len;
    int streaming;
} ChatBubbleC;

#define MAX_MESSAGES 256
static ChatBubbleC s_messages[MAX_MESSAGES];
static int s_message_count = 0;
static const char* s_input_text = "";
static int s_input_len = 0;
static int s_is_live = 0;
static float s_screen_width = 800;
static double s_time = 0;
static Font s_font;
static int s_font_loaded = 0;

// =========================================================================
// C API called from C++
// =========================================================================
void clay_set_messages(const char* roles[], const char* contents[],
                       int content_lens[], int streaming[], int count) {
    s_message_count = count < MAX_MESSAGES ? count : MAX_MESSAGES;
    for (int i = 0; i < s_message_count; i++) {
        s_messages[i].role = roles[i];
        s_messages[i].content = contents[i];
        s_messages[i].content_len = content_lens[i];
        s_messages[i].streaming = streaming[i];
    }
}

void clay_set_input(const char* text, int len) {
    s_input_text = text;
    s_input_len = len;
}

void clay_set_config(int is_live, float screen_width, double time_val) {
    s_is_live = is_live;
    s_screen_width = screen_width;
    s_time = time_val;
}

void clay_set_font(Font font) {
    s_font = font;
    s_font_loaded = 1;
}

// =========================================================================
// Clay text measurement callback
// =========================================================================
static Clay_Dimensions MeasureTextC(Clay_StringSlice text, Clay_TextElementConfig* config, void* userData) {
    (void)userData;
    if (!s_font_loaded) return (Clay_Dimensions){0, 0};

    float scale = (float)config->fontSize / (float)s_font.baseSize;
    float width = 0;
    float maxWidth = 0;
    int lines = 1;

    for (int i = 0; i < (int)text.length; i++) {
        if (text.chars[i] == '\n') {
            if (width > maxWidth) maxWidth = width;
            width = 0;
            lines++;
            continue;
        }
        int idx = GetGlyphIndex(s_font, (unsigned char)text.chars[i]);
        float adv = s_font.glyphs[idx].advanceX
            ? (float)s_font.glyphs[idx].advanceX
            : s_font.recs[idx].width;
        width += adv * scale;
    }
    if (width > maxWidth) maxWidth = width;

    return (Clay_Dimensions){maxWidth, (float)(lines * config->fontSize * 1.25f)};
}

// =========================================================================
// Clay initialization
// =========================================================================
static Clay_Arena s_clay_arena;

void clay_init(int screen_w, int screen_h) {
    uint32_t memSz = Clay_MinMemorySize();
    s_clay_arena = Clay_CreateArenaWithCapacityAndMemory(memSz, malloc(memSz));
    Clay_Initialize(s_clay_arena,
        (Clay_Dimensions){(float)screen_w, (float)screen_h},
        (Clay_ErrorHandler){0});
    Clay_SetMeasureTextFunction(MeasureTextC, NULL);
}

void clay_cleanup(void) {
    free(s_clay_arena.memory);
}

// =========================================================================
// Build layout
// =========================================================================
void clay_build_layout(void) {
    Clay_BeginLayout();

    // Root
    CLAY(CLAY_ID("Root"), {
        .layout = {
            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = CLAY_PADDING_ALL(12),
            .childGap = 8
        },
        .backgroundColor = {25, 25, 35, 255}
    }) {
        // Header
        CLAY(CLAY_ID("Header"), {
            .layout = {
                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(40)},
                .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}
            },
            .backgroundColor = {35, 35, 50, 255},
            .cornerRadius = CLAY_CORNER_RADIUS(8)
        }) {
            Clay_TextElementConfig hdr_cfg = {
                .textColor = {80, 140, 255, 255}, .fontSize = 16
            };
            if (s_is_live)
                CLAY_TEXT(CLAY_STRING("NeoGraph Chatbot (gpt-4o-mini)"), &hdr_cfg);
            else
                CLAY_TEXT(CLAY_STRING("NeoGraph Chatbot (Mock)"), &hdr_cfg);
        }

        // Messages (scrollable)
        CLAY(CLAY_ID("Messages"), {
            .layout = {
                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                .padding = CLAY_PADDING_ALL(8),
                .childGap = 8
            },
            .backgroundColor = {35, 35, 50, 255},
            .cornerRadius = CLAY_CORNER_RADIUS(8),
            .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}
        }) {
            for (int i = 0; i < s_message_count; i++) {
                int is_user = (strcmp(s_messages[i].role, "user") == 0);

                CLAY(CLAY_IDI("Row", (uint32_t)i), {
                    .layout = {
                        .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                        .childAlignment = {
                            is_user ? CLAY_ALIGN_X_RIGHT : CLAY_ALIGN_X_LEFT,
                            CLAY_ALIGN_Y_TOP
                        }
                    }
                }) {
                    CLAY(CLAY_IDI("Bub", (uint32_t)i), {
                        .layout = {
                            .sizing = {CLAY_SIZING_FIT(80, s_screen_width * 0.7f), CLAY_SIZING_FIT(0)},
                            .layoutDirection = CLAY_TOP_TO_BOTTOM,
                            .padding = {12, 12, 8, 10},
                            .childGap = 4
                        },
                        .backgroundColor = is_user
                            ? (Clay_Color){55, 90, 200, 255}
                            : (Clay_Color){50, 55, 65, 255},
                        .cornerRadius = CLAY_CORNER_RADIUS(12)
                    }) {
                        Clay_TextElementConfig role_cfg = {
                            .textColor = {160, 165, 180, 255}, .fontSize = 11
                        };
                        if (is_user)
                            CLAY_TEXT(CLAY_STRING("You"), &role_cfg);
                        else
                            CLAY_TEXT(CLAY_STRING("Assistant"), &role_cfg);

                        // Content
                        static char display_buf[4096];
                        int len = s_messages[i].content_len;
                        if (len > 4090) len = 4090;
                        memcpy(display_buf, s_messages[i].content, len);
                        int dlen = len;

                        if (s_messages[i].streaming && ((int)(s_time * 3.0) % 2))
                            display_buf[dlen++] = '_';
                        if (dlen == 0) {
                            memcpy(display_buf, "...", 3);
                            dlen = 3;
                        }
                        display_buf[dlen] = '\0';

                        Clay_TextElementConfig txt_cfg = {
                            .textColor = {240, 240, 245, 255},
                            .fontSize = 14,
                            .wrapMode = CLAY_TEXT_WRAP_WORDS
                        };
                        Clay_String cs = {
                            .isStaticallyAllocated = false,
                            .length = (int32_t)dlen,
                            .chars = display_buf
                        };
                        CLAY_TEXT(cs, &txt_cfg);
                    }
                }
            }
        }

        // Input bar
        CLAY(CLAY_ID("Input"), {
            .layout = {
                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(44)},
                .padding = {12, 12, 8, 8},
                .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}
            },
            .backgroundColor = {45, 45, 60, 255},
            .cornerRadius = CLAY_CORNER_RADIUS(8)
        }) {
            if (s_input_len > 0) {
                Clay_TextElementConfig icfg = {.textColor = {240,240,245,255}, .fontSize = 14};
                Clay_String ics = {.isStaticallyAllocated = false,
                                   .length = (int32_t)s_input_len,
                                   .chars = s_input_text};
                CLAY_TEXT(ics, &icfg);
            } else {
                Clay_TextElementConfig icfg = {.textColor = {100,105,120,255}, .fontSize = 14};
                CLAY_TEXT(CLAY_STRING("Type a message..."), &icfg);
            }
        }
    }
}

// =========================================================================
// Render Clay commands via Raylib (all in C)
// =========================================================================
void clay_render(void) {
    Clay_RenderCommandArray cmds = Clay_EndLayout(GetFrameTime());

    for (int i = 0; i < (int)cmds.length; i++) {
        Clay_RenderCommand* cmd = Clay_RenderCommandArray_Get(&cmds, i);
        Clay_BoundingBox b = cmd->boundingBox;

        switch (cmd->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
            Clay_RectangleRenderData d = cmd->renderData.rectangle;
            Color c = {(unsigned char)d.backgroundColor.r, (unsigned char)d.backgroundColor.g,
                       (unsigned char)d.backgroundColor.b, (unsigned char)d.backgroundColor.a};
            float r = d.cornerRadius.topLeft;
            if (r > 0.5f) {
                float minDim = b.width < b.height ? b.width : b.height;
                DrawRectangleRounded((Rectangle){b.x, b.y, b.width, b.height},
                    r / (minDim * 0.5f), 6, c);
            } else {
                DrawRectangle((int)b.x, (int)b.y, (int)b.width, (int)b.height, c);
            }
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_TEXT: {
            Clay_TextRenderData d = cmd->renderData.text;
            Color c = {(unsigned char)d.textColor.r, (unsigned char)d.textColor.g,
                       (unsigned char)d.textColor.b, (unsigned char)d.textColor.a};
            // Need null-terminated string for Raylib
            char buf[4096];
            int len = d.stringContents.length < 4095 ? d.stringContents.length : 4095;
            memcpy(buf, d.stringContents.chars, len);
            buf[len] = '\0';
            DrawTextEx(s_font, buf, (Vector2){b.x, b.y}, (float)d.fontSize, 1, c);
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
            BeginScissorMode((int)b.x, (int)b.y, (int)b.width, (int)b.height);
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
// Per-frame update (called from C++)
// =========================================================================
void clay_update(void) {
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)GetScreenWidth(), (float)GetScreenHeight()});
    Clay_SetPointerState(
        (Clay_Vector2){(float)GetMouseX(), (float)GetMouseY()},
        IsMouseButtonDown(MOUSE_BUTTON_LEFT));
    Clay_UpdateScrollContainers(true,
        (Clay_Vector2){0, GetMouseWheelMove() * 50},
        GetFrameTime());
}
