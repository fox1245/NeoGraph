// Unit tests for neograph::async::SseEventParser (Stage 3 Semester 1.4).
//
// Pure state-machine tests — no network, no coroutines. Feed bytes
// in various splits and assert the parser emits the right events.
// Covers the SSE field shapes that OpenAI and Anthropic actually
// send, plus the line-boundary edge cases where partial data can
// be split across multiple transport chunks.

#include <neograph/async/sse_parser.h>

#include <gtest/gtest.h>

#include <string>

namespace async = neograph::async;

TEST(SseEventParser, SingleDataEvent) {
    async::SseEventParser p;
    p.feed("data: hello\n\n");
    auto events = p.drain();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "hello");
    EXPECT_TRUE(events[0].event.empty());
    EXPECT_TRUE(events[0].id.empty());

    // Second drain is empty — events don't replay.
    EXPECT_TRUE(p.drain().empty());
}

TEST(SseEventParser, MultiDataLinesJoined) {
    async::SseEventParser p;
    p.feed("data: line1\ndata: line2\ndata: line3\n\n");
    auto events = p.drain();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "line1\nline2\nline3");
}

TEST(SseEventParser, EventAndIdFields) {
    async::SseEventParser p;
    p.feed("event: chat\nid: 42\ndata: hi\n\n");
    auto events = p.drain();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].event, "chat");
    EXPECT_EQ(events[0].id, "42");
    EXPECT_EQ(events[0].data, "hi");
}

TEST(SseEventParser, PartialAcrossFeeds) {
    async::SseEventParser p;
    p.feed("data: par");
    EXPECT_TRUE(p.drain().empty());
    p.feed("tial");
    EXPECT_TRUE(p.drain().empty());
    p.feed("\n\n");
    auto events = p.drain();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "partial");
}

TEST(SseEventParser, BoundarySplitMidNewline) {
    async::SseEventParser p;
    p.feed("data: one\n");
    EXPECT_TRUE(p.drain().empty());   // boundary not yet seen
    p.feed("\n");                     // complete the \n\n
    auto events = p.drain();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "one");
}

TEST(SseEventParser, CommentsIgnored) {
    async::SseEventParser p;
    p.feed(": keepalive\n: another comment\ndata: real\n\n");
    auto events = p.drain();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "real");
}

TEST(SseEventParser, CommentOnlyEventDoesNotFire) {
    async::SseEventParser p;
    // A comment-only block between events must not emit a phantom event.
    p.feed(": hb\n\ndata: after\n\n");
    auto events = p.drain();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "after");
}

TEST(SseEventParser, CRLFLineEndings) {
    async::SseEventParser p;
    p.feed("data: with-crlf\r\n\r\n");
    auto events = p.drain();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "with-crlf");
}

TEST(SseEventParser, TwoEventsInOneFeed) {
    async::SseEventParser p;
    p.feed("data: one\n\ndata: two\n\n");
    auto events = p.drain();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].data, "one");
    EXPECT_EQ(events[1].data, "two");
}

TEST(SseEventParser, FieldWithoutLeadingSpace) {
    // "data:value" (no space after colon) is valid per spec.
    async::SseEventParser p;
    p.feed("data:nospace\n\n");
    auto events = p.drain();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "nospace");
}

TEST(SseEventParser, RealWorldAnthropicShape) {
    // Abbreviated Anthropic `messages` streaming wire.
    async::SseEventParser p;
    p.feed(
        "event: message_start\n"
        "data: {\"type\":\"message_start\"}\n"
        "\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"delta\":{\"text\":\"Hi\"}}\n"
        "\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n"
        "\n"
    );
    auto events = p.drain();
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].event, "message_start");
    EXPECT_EQ(events[1].event, "content_block_delta");
    EXPECT_EQ(events[2].event, "message_stop");
    EXPECT_NE(events[1].data.find("\"Hi\""), std::string::npos);
}

TEST(SseEventParser, ResetClearsPartial) {
    async::SseEventParser p;
    p.feed("data: partial-will-be-discarded");
    p.reset();
    p.feed("data: fresh\n\n");
    auto events = p.drain();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "fresh");
}
