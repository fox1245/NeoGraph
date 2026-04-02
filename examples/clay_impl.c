// Clay implementation + layout — compiled as C99
// Uses the official Clay Raylib renderer for correct text rendering.

#define CLAY_IMPLEMENTATION
#include <clay.h>
#include "../deps/clay_renderer_raylib.c"
#include <string.h>

// =========================================================================
// Shared state
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

#define FONT_ID_BODY 0
static Font s_fonts[1];

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
    s_fonts[FONT_ID_BODY] = font;
}

// =========================================================================
// Init / cleanup
// =========================================================================
static Clay_Arena s_clay_arena;

void clay_init(int screen_w, int screen_h) {
    uint32_t memSz = Clay_MinMemorySize();
    s_clay_arena = Clay_CreateArenaWithCapacityAndMemory(memSz, malloc(memSz));
    Clay_Initialize(s_clay_arena,
        (Clay_Dimensions){(float)screen_w, (float)screen_h},
        (Clay_ErrorHandler){0});
    Clay_SetMeasureTextFunction(Raylib_MeasureText, s_fonts);
}

void clay_cleanup(void) { free(s_clay_arena.memory); }

// =========================================================================
// Layout
// =========================================================================
void clay_build_layout(void) {
    Clay_BeginLayout();

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
                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(48)},
                .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}
            },
            .backgroundColor = {35, 35, 50, 255},
            .cornerRadius = CLAY_CORNER_RADIUS(8)
        }) {
            if (s_is_live)
                CLAY_TEXT(CLAY_STRING("NeoGraph Chatbot (gpt-4o-mini)"),
                    CLAY_TEXT_CONFIG({ .textColor = {80, 140, 255, 255}, .fontId = FONT_ID_BODY, .fontSize = 24 }));
            else
                CLAY_TEXT(CLAY_STRING("NeoGraph Chatbot (Mock)"),
                    CLAY_TEXT_CONFIG({ .textColor = {80, 140, 255, 255}, .fontId = FONT_ID_BODY, .fontSize = 24 }));
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
                            CLAY_ALIGN_Y_TOP }
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
                        if (is_user)
                            CLAY_TEXT(CLAY_STRING("You"),
                                CLAY_TEXT_CONFIG({ .textColor = {160,165,180,255}, .fontId = FONT_ID_BODY, .fontSize = 16 }));
                        else
                            CLAY_TEXT(CLAY_STRING("Assistant"),
                                CLAY_TEXT_CONFIG({ .textColor = {160,165,180,255}, .fontId = FONT_ID_BODY, .fontSize = 16 }));

                        // Per-message buffer (static array so pointers survive until render)
                        static char display_bufs[MAX_MESSAGES][4096];
                        int len = s_messages[i].content_len;
                        if (len > 4090) len = 4090;
                        memcpy(display_bufs[i], s_messages[i].content, len);
                        int dlen = len;
                        if (s_messages[i].streaming && ((int)(s_time * 3.0) % 2))
                            display_bufs[i][dlen++] = '_';
                        if (dlen == 0) { memcpy(display_bufs[i], "...", 3); dlen = 3; }
                        display_bufs[i][dlen] = '\0';

                        Clay_String cs = {
                            .isStaticallyAllocated = false,
                            .length = (int32_t)dlen,
                            .chars = display_bufs[i]
                        };
                        CLAY_TEXT(cs,
                            CLAY_TEXT_CONFIG({ .textColor = {240,240,245,255}, .fontId = FONT_ID_BODY, .fontSize = 20,
                                .wrapMode = CLAY_TEXT_WRAP_WORDS }));
                    }
                }
            }
        }

        // Input bar
        CLAY(CLAY_ID("Input"), {
            .layout = {
                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(48)},
                .padding = {12, 12, 8, 8},
                .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}
            },
            .backgroundColor = {45, 45, 60, 255},
            .cornerRadius = CLAY_CORNER_RADIUS(8)
        }) {
            if (s_input_len > 0) {
                Clay_String ics = { .isStaticallyAllocated = false,
                                    .length = (int32_t)s_input_len,
                                    .chars = s_input_text };
                CLAY_TEXT(ics,
                    CLAY_TEXT_CONFIG({ .textColor = {240,240,245,255}, .fontId = FONT_ID_BODY, .fontSize = 20 }));
            } else {
                CLAY_TEXT(CLAY_STRING("Type a message..."),
                    CLAY_TEXT_CONFIG({ .textColor = {100,105,120,255}, .fontId = FONT_ID_BODY, .fontSize = 20 }));
            }
        }
    }
}

// =========================================================================
// Render + update
// =========================================================================
void clay_render(void) {
    Clay_RenderCommandArray cmds = Clay_EndLayout(GetFrameTime());
    Clay_Raylib_Render(cmds, s_fonts);
}

void clay_update(void) {
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)GetScreenWidth(), (float)GetScreenHeight()});
    Clay_SetPointerState(
        (Clay_Vector2){(float)GetMouseX(), (float)GetMouseY()},
        IsMouseButtonDown(MOUSE_BUTTON_LEFT));
    Clay_UpdateScrollContainers(true,
        (Clay_Vector2){0, GetMouseWheelMove() * 50},
        GetFrameTime());
}
