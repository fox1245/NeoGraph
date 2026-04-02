// Clay implementation + layout — compiled as C99
// The layout is defined here because Clay macros work correctly in C.
// C++ code (NeoGraph agent, threads) is in 10_clay_chatbot.cpp.

#define CLAY_IMPLEMENTATION
#include <clay.h>
#include <string.h>

// =========================================================================
// Shared state (written by C++ side, read by C layout)
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

// Called from C++ to sync state before layout
void clay_set_messages(const char* roles[], const char* contents[], int content_lens[], int streaming[], int count) {
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

void clay_set_config(int is_live, float screen_width, double time) {
    s_is_live = is_live;
    s_screen_width = screen_width;
    s_time = time;
}

// =========================================================================
// Build Clay layout
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

                        // Content — use dynamic Clay_String
                        static char display_buf[4096];
                        int len = s_messages[i].content_len;
                        if (len > 4090) len = 4090;
                        memcpy(display_buf, s_messages[i].content, len);
                        int dlen = len;

                        if (s_messages[i].streaming && ((int)(s_time * 3.0) % 2)) {
                            display_buf[dlen++] = '_';
                        }
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
            Clay_TextElementConfig icfg;
            Clay_String ics;

            if (s_input_len > 0) {
                icfg = (Clay_TextElementConfig){.textColor = {240,240,245,255}, .fontSize = 14};
                ics = (Clay_String){.isStaticallyAllocated = false,
                                    .length = (int32_t)s_input_len,
                                    .chars = s_input_text};
            } else {
                icfg = (Clay_TextElementConfig){.textColor = {100,105,120,255}, .fontSize = 14};
                // UTF-8 placeholder
                static const char placeholder[] = "\xEB\xA9\x94\xEC\x8B\x9C\xEC\xA7\x80\xEB\xA5\xBC"
                    " \xEC\x9E\x85\xEB\xA0\xA5\xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94...";
                ics = (Clay_String){.isStaticallyAllocated = true,
                                    .length = (int32_t)sizeof(placeholder) - 1,
                                    .chars = placeholder};
            }
            CLAY_TEXT(ics, &icfg);
        }
    }
}
