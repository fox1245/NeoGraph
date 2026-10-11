// Clay layout + Raylib rendering for example 11's chat window — compiled as C99.
// The layout is rebuilt every frame from the host's ChatViewState (clay_chat_ui.h).

#define CLAY_IMPLEMENTATION
#include <clay.h>
#include "../deps/clay_renderer_raylib.c"
#include "clay_chat_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =========================================================================
// Theme
// =========================================================================
static const Clay_Color kBackground   = {14, 16, 21, 255};
static const Clay_Color kSurface      = {22, 25, 34, 255};
static const Clay_Color kRaised       = {31, 35, 48, 255};
static const Clay_Color kBorder       = {42, 47, 63, 255};
static const Clay_Color kText         = {231, 233, 240, 255};
static const Clay_Color kMuted        = {142, 148, 168, 255};
static const Clay_Color kFaint        = {96, 102, 122, 255};
static const Clay_Color kAccent       = {109, 93, 252, 255};
static const Clay_Color kAccentHover  = {131, 118, 255, 255};
static const Clay_Color kOnAccent     = {255, 255, 255, 255};
static const Clay_Color kCodeBg       = {11, 13, 18, 255};
static const Clay_Color kCodeText     = {196, 208, 232, 255};
static const Clay_Color kLive         = {52, 211, 153, 255};
static const Clay_Color kLiveBg       = {17, 50, 41, 255};
static const Clay_Color kMock         = {251, 191, 36, 255};
static const Clay_Color kMockBg       = {56, 45, 16, 255};
static const Clay_Color kErrorText    = {255, 184, 194, 255};
static const Clay_Color kErrorBg      = {56, 25, 31, 255};
static const Clay_Color kErrorBorder  = {124, 43, 55, 255};

#define COLUMN_MAX 820  // conversation column width cap, in pixels

static const char* kSuggestions[] = {
    "What can NeoGraph do?",
    "Show me a tiny graph in C++",
    "한국어로 짧게 자기소개 해줘",
};
#define SUGGESTION_COUNT ((int)(sizeof kSuggestions / sizeof kSuggestions[0]))

// =========================================================================
// State
// =========================================================================
static Font s_fonts[CHAT_FONT_COUNT];
static Clay_Arena s_clay_arena;
static int s_stick_to_bottom = 1;
static int s_cursor = MOUSE_CURSOR_DEFAULT;

// Per-frame scratch for text derived from messages (Clay keeps string pointers
// until the frame is rendered).
static char s_scratch[1 << 20];
static int s_scratch_used;

static Clay_String scratch_copy(const char* chars, int len) {
    if (len < 0 || s_scratch_used + len > (int)sizeof s_scratch) len = 0;
    char* dst = s_scratch + s_scratch_used;
    memcpy(dst, chars, (size_t)len);
    s_scratch_used += len;
    return (Clay_String){ .isStaticallyAllocated = false, .length = len, .chars = dst };
}

static Clay_String view_string(const char* chars, int len) {
    return (Clay_String){ .isStaticallyAllocated = false, .length = len, .chars = chars };
}

static Clay_String c_string(const char* chars) {
    return view_string(chars, chars ? (int)strlen(chars) : 0);
}

// =========================================================================
// Text measurement
// =========================================================================
// The stock Raylib_MeasureText indexes glyphs by byte, which only works for
// ASCII. MeasureTextEx decodes UTF-8 and looks glyphs up by codepoint, matching
// what DrawTextEx renders.
static Clay_Dimensions measure_text(Clay_StringSlice text, Clay_TextElementConfig* config, void* user_data) {
    Font font = ((Font*)user_data)[config->fontId];
    if (!font.glyphs) font = GetFontDefault();
    static char line[4096];
    float width = 0;
    int start = 0;
    for (int i = 0; i <= text.length; ++i) {
        if (i < text.length && text.chars[i] != '\n') continue;
        int n = i - start;
        if (n > (int)sizeof line - 1) n = (int)sizeof line - 1;
        if (n > 0) {
            memcpy(line, text.chars + start, (size_t)n);
            line[n] = '\0';
            float w = MeasureTextEx(font, line, config->fontSize, config->letterSpacing).x;
            if (w > width) width = w;
        }
        start = i + 1;
    }
    return (Clay_Dimensions){ width, (float)config->fontSize };
}

static void on_clay_error(Clay_ErrorData error) {
    static int reported;
    if (reported++ < 5)
        fprintf(stderr, "[clay] %.*s\n", (int)error.errorText.length, error.errorText.chars);
}

// =========================================================================
// Init / fonts / shared strings
// =========================================================================
void clay_chat_init(int width, int height) {
    uint32_t size = Clay_MinMemorySize();
    s_clay_arena = Clay_CreateArenaWithCapacityAndMemory(size, malloc(size));
    Clay_Initialize(s_clay_arena, (Clay_Dimensions){ (float)width, (float)height },
                    (Clay_ErrorHandler){ on_clay_error, NULL });
    Clay_SetMeasureTextFunction(measure_text, s_fonts);
}

void clay_chat_shutdown(void) {
    free(s_clay_arena.memory);
    if (temp_render_buffer) free(temp_render_buffer);
}

void clay_chat_set_fonts(const Font fonts[CHAT_FONT_COUNT]) {
    memcpy(s_fonts, fonts, sizeof s_fonts);
    Clay_ResetMeasureTextCache();
}

const char* clay_chat_ui_text(void) {
    static char text[1024];
    if (!text[0]) {
        snprintf(text, sizeof text, "%s",
                 "NeoGraph Chat N You Send Stop Stopped LIVE MOCK Generating How can I help? "
                 "Ask anything. Replies stream in from a NeoGraph agent. "
                 "Message NeoGraph... Enter to send · Esc to clear · Ctrl+V to paste • ");
        for (int i = 0; i < SUGGESTION_COUNT; ++i) {
            strncat(text, kSuggestions[i], sizeof text - strlen(text) - 2);
            strncat(text, " ", sizeof text - strlen(text) - 1);
        }
    }
    return text;
}

int clay_chat_suggestion_count(void) { return SUGGESTION_COUNT; }
const char* clay_chat_suggestion(int index) {
    return index >= 0 && index < SUGGESTION_COUNT ? kSuggestions[index] : "";
}

void clay_chat_stick_to_bottom(void) { s_stick_to_bottom = 1; }

// =========================================================================
// Small building blocks
// =========================================================================
static void text(Clay_String s, int font, int size, int line_height, Clay_Color color) {
    CLAY_TEXT(s, { .textColor = color, .fontId = (uint16_t)font, .fontSize = (uint16_t)size,
                   .lineHeight = (uint16_t)line_height, .wrapMode = CLAY_TEXT_WRAP_WORDS });
}

static void body_text(Clay_String s, Clay_Color color) {
    text(s, CHAT_FONT_BODY, CHAT_PX_BODY, 25, color);
}

static void small_text(Clay_String s, Clay_Color color) {
    text(s, CHAT_FONT_SMALL, CHAT_PX_SMALL, 17, color);
}

static void dot(float size, Clay_Color color) {
    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(size), CLAY_SIZING_FIXED(size) } },
                   .backgroundColor = color, .cornerRadius = CLAY_CORNER_RADIUS(size / 2) }) {}
}

static void avatar(float size) {
    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(size), CLAY_SIZING_FIXED(size) },
                               .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } },
                   .backgroundColor = kAccent, .cornerRadius = CLAY_CORNER_RADIUS(size / 2) }) {
        if (size >= 48) text(CLAY_STRING("N"), CHAT_FONT_DISPLAY, CHAT_PX_DISPLAY, CHAT_PX_DISPLAY, kOnAccent);
        else text(CLAY_STRING("N"), CHAT_FONT_TITLE, CHAT_PX_TITLE, CHAT_PX_TITLE, kOnAccent);
    }
}

static void pill(Clay_String label, Clay_Color fg, Clay_Color bg, float dot_alpha) {
    CLAY_AUTO_ID({ .layout = { .padding = { 10, 11, 5, 5 }, .childGap = 7,
                               .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                   .backgroundColor = bg, .cornerRadius = CLAY_CORNER_RADIUS(12) }) {
        Clay_Color c = fg;
        c.a = 255 * dot_alpha;
        dot(7, c);
        small_text(label, fg);
    }
}

// Three dots fading in sequence while a reply is on its way.
static void typing_indicator(double t) {
    CLAY_AUTO_ID({ .layout = { .childGap = 6, .padding = { 2, 2, 7, 5 } } }) {
        for (int k = 0; k < 3; ++k) {
            float wave = (float)sin(t * 5.0 - k * 0.8);
            Clay_Color c = kMuted;
            c.a = 255 * (0.3f + 0.7f * (wave > 0 ? wave : 0));
            dot(7, c);
        }
    }
}

// =========================================================================
// Message body: plain text for the user, light Markdown for the model
// =========================================================================
static int starts_with(const char* s, int len, const char* prefix) {
    int n = (int)strlen(prefix);
    return len >= n && memcmp(s, prefix, (size_t)n) == 0;
}

static void code_block(Clay_String code) {
    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .padding = { 14, 14, 11, 11 } },
                   .backgroundColor = kCodeBg, .cornerRadius = CLAY_CORNER_RADIUS(10),
                   .border = { .color = kBorder, .width = { 1, 1, 1, 1 } } }) {
        text(code, CHAT_FONT_MONO, CHAT_PX_MONO, 21, kCodeText);
    }
}

// Blocks are assembled in place at the end of the scratch buffer, then committed.
typedef struct { char* chars; int len; } Block;

static Block block_begin(void) { return (Block){ s_scratch + s_scratch_used, 0 }; }

static void block_put(Block* b, char c) {
    if (s_scratch_used + b->len + 1 < (int)sizeof s_scratch) b->chars[b->len++] = c;
}

static Clay_String block_take(Block* b) {
    int len = b->len;
    while (len > 0 && b->chars[len - 1] == '\n') --len;
    Clay_String s = { .isStaticallyAllocated = false, .length = len, .chars = b->chars };
    s_scratch_used += b->len;
    *b = block_begin();
    return s;
}

static void flush_paragraph(Block* b, Clay_Color color) {
    Clay_String s = block_take(b);
    if (s.length) body_text(s, color);
}

static void flush_code(Block* b) {
    Clay_String s = block_take(b);
    if (s.length) code_block(s);
}

// Paragraphs, "#" headings, "-"/"*" bullets and ``` fences. Emphasis markers
// (** and `) are dropped rather than rendered; everything else is shown as-is.
static void markdown_body(const char* chars, int len, Clay_Color color) {
    const char* p = chars;
    const char* end = chars + len;
    int in_code = 0;
    Block b = block_begin();
    while (p < end) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        const char* line_end = nl ? nl : end;
        const char* s = p;
        while (s < line_end && (*s == ' ' || *s == '\t')) ++s;
        int n = (int)(line_end - s);

        if (starts_with(s, n, "```")) {
            if (in_code) flush_code(&b);
            else flush_paragraph(&b, color);
            in_code = !in_code;
        } else if (in_code) {
            for (const char* c = p; c < line_end; ++c) block_put(&b, *c);
            block_put(&b, '\n');
        } else if (n == 0) {
            flush_paragraph(&b, color);
        } else if (*s == '#') {
            flush_paragraph(&b, color);
            while (s < line_end && (*s == '#' || *s == ' ')) ++s;
            text(scratch_copy(s, (int)(line_end - s)), CHAT_FONT_TITLE, CHAT_PX_TITLE, 24, kText);
            b = block_begin();
        } else {
            if (n > 1 && (*s == '-' || *s == '*') && s[1] == ' ') {
                block_put(&b, '\xE2'); block_put(&b, '\x80'); block_put(&b, '\xA2');  // U+2022
                ++s;
            }
            for (const char* c = s; c < line_end; ++c) {
                if (*c == '`') continue;
                if (*c == '*' && c + 1 < line_end && c[1] == '*') { ++c; continue; }
                block_put(&b, *c);
            }
            block_put(&b, '\n');
        }
        p = nl ? nl + 1 : end;
    }
    if (in_code) flush_code(&b);  // a fence still streaming in
    else flush_paragraph(&b, color);
}

static void message_row(const ChatViewState* st, int i, float column, double t) {
    const ChatMessageView* m = &st->messages[i];
    float bubble_max = m->is_user ? column * 0.78f : column - 44;
    Clay_Color bubble_bg = m->failed ? kErrorBg : m->is_user ? kAccent : kSurface;
    Clay_Color bubble_border = m->failed ? kErrorBorder : m->is_user ? kAccent : kBorder;
    Clay_Color fg = m->failed ? kErrorText : m->is_user ? kOnAccent : kText;

    CLAY(CLAY_IDI("Message", (uint32_t)i), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0, COLUMN_MAX), CLAY_SIZING_FIT(0) }, .childGap = 12,
                    .childAlignment = { .x = m->is_user ? CLAY_ALIGN_X_RIGHT : CLAY_ALIGN_X_LEFT } } }) {
        if (!m->is_user) avatar(32);
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIT(0, bubble_max), CLAY_SIZING_FIT(0) },
                                   .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 6,
                                   .childAlignment = { .x = m->is_user ? CLAY_ALIGN_X_RIGHT : CLAY_ALIGN_X_LEFT } } }) {
            CLAY_AUTO_ID({ .layout = { .childGap = 8, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                small_text(m->is_user ? CLAY_STRING("You") : CLAY_STRING("NeoGraph"), kMuted);
                small_text(c_string(m->time_label), kFaint);
            }
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIT(0, bubble_max), CLAY_SIZING_FIT(0) },
                                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = { 16, 16, 11, 12 },
                                       .childGap = 10 },
                           .backgroundColor = bubble_bg, .cornerRadius = CLAY_CORNER_RADIUS(14),
                           .border = { .color = bubble_border, .width = { 1, 1, 1, 1 } } }) {
                if (m->content_len == 0 && m->streaming) {
                    typing_indicator(t);
                } else if (m->content_len == 0 && m->stopped) {
                    small_text(CLAY_STRING("Stopped"), kMuted);
                } else {
                    if (m->is_user) body_text(view_string(m->content, m->content_len), fg);
                    else markdown_body(m->content, m->content_len, fg);
                    if (m->streaming) typing_indicator(t);
                    if (m->stopped) small_text(CLAY_STRING("Stopped"), kMuted);
                }
            }
        }
    }
}

// =========================================================================
// Screen sections
// =========================================================================
static void header(const ChatViewState* st) {
    CLAY(CLAY_ID("Header"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(64) }, .padding = { 20, 20, 0, 0 },
                    .childGap = 12, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
        .backgroundColor = kSurface, .border = { .color = kBorder, .width = { .bottom = 1 } } }) {
        avatar(36);
        CLAY_AUTO_ID({ .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 3 } }) {
            text(CLAY_STRING("NeoGraph Chat"), CHAT_FONT_TITLE, CHAT_PX_TITLE, 20, kText);
            small_text(c_string(st->model), kMuted);
        }
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}
        if (st->busy)
            pill(CLAY_STRING("Generating"), kAccentHover, kRaised, 0.4f + 0.6f * (float)fabs(sin(st->time * 3.0)));
        int live = st->mode && strcmp(st->mode, "LIVE") == 0;
        pill(c_string(st->mode), live ? kLive : kMock, live ? kLiveBg : kMockBg, 1.0f);
    }
}

static void welcome(void) {
    CLAY(CLAY_ID("Welcome"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0, 540), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .padding = { 28, 28, 30, 26 }, .childGap = 12, .childAlignment = { .x = CLAY_ALIGN_X_CENTER } },
        .backgroundColor = kSurface, .cornerRadius = CLAY_CORNER_RADIUS(18),
        .border = { .color = kBorder, .width = { 1, 1, 1, 1 } } }) {
        avatar(52);
        text(CLAY_STRING("How can I help?"), CHAT_FONT_DISPLAY, CHAT_PX_DISPLAY, 34, kText);
        small_text(CLAY_STRING("Ask anything. Replies stream in from a NeoGraph agent."), kMuted);
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                   .childGap = 8, .padding = { 0, 0, 10, 0 } } }) {
            for (int k = 0; k < SUGGESTION_COUNT; ++k) {
                CLAY(CLAY_IDI("Suggestion", (uint32_t)k), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0) }, .padding = { 16, 16, 11, 11 } },
                    .backgroundColor = Clay_Hovered() ? kRaised : kBackground,
                    .cornerRadius = CLAY_CORNER_RADIUS(12),
                    .border = { .color = Clay_Hovered() ? kAccent : kBorder, .width = { 1, 1, 1, 1 } } }) {
                    body_text(c_string(kSuggestions[k]), kText);
                }
            }
        }
    }
}

// Drawn as a floating overlay so the thumb never takes part in sizing the
// conversation (its size derives from the previous frame's layout).
static void scrollbar(void) {
    Clay_ScrollContainerData d = Clay_GetScrollContainerData(CLAY_ID("Messages"));
    float view = d.scrollContainerDimensions.height;
    float content = d.contentDimensions.height;
    if (!d.found || content <= view + 1) return;
    float track = view - 16;
    float thumb = fmaxf(36, track * view / content);
    float offset = -d.scrollPosition->y / (content - view) * (track - thumb);
    CLAY(CLAY_ID("ScrollThumb"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(5), CLAY_SIZING_FIXED(thumb) } },
        .backgroundColor = kBorder, .cornerRadius = CLAY_CORNER_RADIUS(2.5f),
        .floating = { .attachTo = CLAY_ATTACH_TO_PARENT, .offset = { -5, 8 + fmaxf(0, offset) },
                      .attachPoints = { .element = CLAY_ATTACH_POINT_RIGHT_TOP, .parent = CLAY_ATTACH_POINT_RIGHT_TOP },
                      .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH } }) {}
}

static void conversation(const ChatViewState* st) {
    float column = fminf(COLUMN_MAX, (float)GetScreenWidth() - 48);
    CLAY(CLAY_ID("Conversation"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {
        CLAY(CLAY_ID("Messages"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .padding = { 24, 24, 24, 24 }, .childGap = 20,
                        .childAlignment = { CLAY_ALIGN_X_CENTER,
                                            st->message_count ? CLAY_ALIGN_Y_TOP : CLAY_ALIGN_Y_CENTER } },
            .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
            if (st->message_count == 0) welcome();
            for (int i = 0; i < st->message_count; ++i) message_row(st, i, column, st->time);
        }
        scrollbar();
    }
}

static void composer(const ChatViewState* st) {
    int can_send = st->input_len > 0 && !st->busy;
    int caret_on = fmod(st->time, 1.0) < 0.55;
    CLAY(CLAY_ID("Composer"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .padding = { 24, 24, 14, 12 }, .childGap = 8, .childAlignment = { .x = CLAY_ALIGN_X_CENTER } },
        .backgroundColor = kBackground, .border = { .color = kBorder, .width = { .top = 1 } } }) {
        CLAY(CLAY_ID("Field"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0, COLUMN_MAX), CLAY_SIZING_FIT(54) },
                        .padding = { 18, 8, 9, 9 }, .childGap = 12, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
            .backgroundColor = kSurface, .cornerRadius = CLAY_CORNER_RADIUS(16),
            .border = { .color = st->input_len ? kAccent : kBorder, .width = { 1, 1, 1, 1 } } }) {
            CLAY(CLAY_ID("FieldText"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                                                     .childGap = 2, .childAlignment = { .y = CLAY_ALIGN_Y_BOTTOM } } }) {
                if (st->input_len > 0) body_text(view_string(st->input, st->input_len), kText);
                CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(2), CLAY_SIZING_FIXED(20) } },
                               .backgroundColor = caret_on ? kAccentHover : kSurface }) {}
                if (st->input_len == 0) body_text(CLAY_STRING("Message NeoGraph..."), kFaint);
            }
            // While a reply streams in, the button stops it instead.
            CLAY(CLAY_ID("Send"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(76), CLAY_SIZING_FIXED(36) },
                            .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } },
                .backgroundColor = st->busy ? (Clay_Hovered() ? kBorder : kRaised)
                                 : !can_send ? kRaised : Clay_Hovered() ? kAccentHover : kAccent,
                .cornerRadius = CLAY_CORNER_RADIUS(10) }) {
                if (st->busy) text(CLAY_STRING("Stop"), CHAT_FONT_TITLE, CHAT_PX_SMALL + 2, 18, kText);
                else text(CLAY_STRING("Send"), CHAT_FONT_TITLE, CHAT_PX_SMALL + 2, 18, can_send ? kOnAccent : kFaint);
            }
        }
        small_text(CLAY_STRING("Enter to send · Esc to clear · Ctrl+V to paste"), kFaint);
    }
}

// =========================================================================
// Per-frame entry points
// =========================================================================
int clay_chat_update(const ChatViewState* st, int* suggestion) {
    Clay_SetLayoutDimensions((Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() });
    Clay_SetPointerState((Clay_Vector2){ (float)GetMouseX(), (float)GetMouseY() },
                         IsMouseButtonDown(MOUSE_BUTTON_LEFT));
    float wheel = GetMouseWheelMove();
    Clay_UpdateScrollContainers(false, (Clay_Vector2){ 0, wheel * 60 }, GetFrameTime());

    // Follow new output unless the user has scrolled up to read.
    Clay_ScrollContainerData d = Clay_GetScrollContainerData(CLAY_ID("Messages"));
    if (d.found) {
        float max = fmaxf(0, d.contentDimensions.height - d.scrollContainerDimensions.height);
        if (wheel > 0) s_stick_to_bottom = 0;
        if (-d.scrollPosition->y >= max - 2) s_stick_to_bottom = 1;
        if (s_stick_to_bottom) d.scrollPosition->y = -max;
    }

    // Hit-testing uses the previous frame's layout, which is what the user sees.
    int action = CHAT_ACTION_NONE;
    int hand = Clay_PointerOver(CLAY_ID("Send"));
    if (hand && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) action = st->busy ? CHAT_ACTION_STOP : CHAT_ACTION_SEND;
    for (int k = 0; k < SUGGESTION_COUNT && st->message_count == 0; ++k) {
        if (!Clay_PointerOver(CLAY_IDI("Suggestion", (uint32_t)k))) continue;
        hand = 1;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { action = CHAT_ACTION_SUGGESTION; *suggestion = k; }
    }
    int cursor = hand ? MOUSE_CURSOR_POINTING_HAND
               : Clay_PointerOver(CLAY_ID("Field")) ? MOUSE_CURSOR_IBEAM : MOUSE_CURSOR_DEFAULT;
    if (cursor != s_cursor) SetMouseCursor(s_cursor = cursor);
    return action;
}

void clay_chat_layout(const ChatViewState* st) {
    s_scratch_used = 0;
    Clay_BeginLayout();
    CLAY(CLAY_ID("Root"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                                        .layoutDirection = CLAY_TOP_TO_BOTTOM },
                            .backgroundColor = kBackground }) {
        header(st);
        conversation(st);
        composer(st);
    }
}

void clay_chat_render(void) {
    Clay_RenderCommandArray commands = Clay_EndLayout(GetFrameTime());
    Clay_Raylib_Render(commands, s_fonts);
}
