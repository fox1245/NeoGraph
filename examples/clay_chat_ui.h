// Clay chat UI (clay_impl.c) <-> NeoGraph chat host (11_clay_chatbot.cpp).
// The host owns conversation state, input and fonts; the C side owns layout,
// rendering and hit-testing. Strings passed in must stay valid until
// clay_render() returns.
#pragma once

#include <raylib.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { CHAT_FONT_BODY, CHAT_FONT_SMALL, CHAT_FONT_TITLE, CHAT_FONT_DISPLAY, CHAT_FONT_MONO, CHAT_FONT_COUNT };

// Pixel sizes each font slot is rasterized at; text is drawn at exactly these sizes.
enum { CHAT_PX_BODY = 17, CHAT_PX_SMALL = 13, CHAT_PX_TITLE = 17, CHAT_PX_DISPLAY = 26, CHAT_PX_MONO = 15 };

typedef struct {
    const char* content;
    int content_len;
    int is_user;
    int streaming;  // reply still arriving
    int failed;     // content is an error description
    int stopped;    // the user stopped the reply; content is what arrived
    const char* time_label;  // e.g. "14:05"
} ChatMessageView;

typedef struct {
    const ChatMessageView* messages;
    int message_count;
    const char* input;
    int input_len;
    int busy;             // a reply is being generated
    const char* mode;     // "LIVE" or "MOCK"
    const char* model;    // shown under the title
    double time;          // seconds, drives animations
} ChatViewState;

enum { CHAT_ACTION_NONE, CHAT_ACTION_SEND, CHAT_ACTION_STOP, CHAT_ACTION_SUGGESTION };

void clay_chat_init(int width, int height);
void clay_chat_shutdown(void);
void clay_chat_set_fonts(const Font fonts[CHAT_FONT_COUNT]);  // also drops cached text sizes
const char* clay_chat_ui_text(void);  // every built-in label, so the host can rasterize its glyphs
int clay_chat_suggestion_count(void);
const char* clay_chat_suggestion(int index);

// Per frame: update (input + scrolling), layout, render. update() returns the
// action clicked in the previous frame's layout; *suggestion receives its index.
int clay_chat_update(const ChatViewState* state, int* suggestion);
void clay_chat_layout(const ChatViewState* state);
void clay_chat_render(void);
void clay_chat_stick_to_bottom(void);  // follow new output again (after sending)

#ifdef __cplusplus
}
#endif
