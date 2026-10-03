#include <gtest/gtest.h>
#include "fixtures/typed_wire_peer.h"
#include <core/native.h>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <algorithm>
#include <type_traits>

using namespace neograph;
namespace wire = neograph::test::wire;
using namespace std::chrono_literals;

TEST(SchemaProviderStreamAsyncOuterIo, KoreanResponseStreamsViaOuterCoSpawn) {
    const std::string expected = "서울의 날씨를 확인했습니다.";
    wire::Peer peer(wire::responses_sse(expected), true);
    auto provider = wire::provider("openai.responses", peer.origin());
    struct Capture { std::string text; std::thread::id executor; bool wrong_executor = false; };
    auto capture = std::make_shared<Capture>(); capture->executor = std::this_thread::get_id();
    auto request = wire::request("openai.responses", ProviderMode::Stream);
    request.on_event = [owned = capture](const sp::Event& event) {
        owned->wrong_executor |= std::this_thread::get_id() != owned->executor;
        if (const auto* delta = std::get_if<sp::PartDelta>(&event); delta && delta->payload.kind == sp::PartKind::Text)
            owned->text.append(delta->payload.bytes);
    };
    asio::io_context io;
    auto result = asio::co_spawn(io, provider->invoke_async(std::move(request)), asio::use_future);
    io.run();
    const auto outcome = result.get();
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*outcome));
    EXPECT_EQ(test::text(outcome), expected); EXPECT_EQ(capture->text, expected);
    EXPECT_FALSE(capture->wrong_executor);
}

TEST(SchemaProviderStreamAsyncOuterIo, OuterIoStaysResponsiveWhilePeerHoldsResponse) {
    wire::Peer peer(wire::responses_sse(), true); peer.state->hold = true;
    auto provider = wire::provider("openai.responses", peer.origin());
    asio::io_context io;
    auto result = asio::co_spawn(io, provider->invoke_async(wire::request("openai.responses", ProviderMode::Stream)), asio::use_future);
    auto ticks = std::make_shared<std::atomic<int>>(0);
    asio::steady_timer timer(io, 20ms);
    timer.async_wait([owned = ticks, state = peer.state](const asio::error_code& ec) {
        if (ec) return;
        ++*owned;
        std::lock_guard lock(state->mutex); state->released = true; state->cv.notify_all();
    });
    io.run();
    EXPECT_EQ(ticks->load(), 1);
    EXPECT_EQ(test::text(result.get()), "pong");
}

TEST(SchemaProviderStreamAsyncOuterIo, ConcurrentOuterCoroutinesOwnSeparateResults) {
    wire::Peer peer(wire::responses_sse("동시 응답"), true);
    auto provider = wire::provider("openai.responses", peer.origin());
    asio::io_context io;
    std::vector<std::future<sp::runtime::Result>> results;
    for (int i = 0; i < 6; ++i) results.push_back(asio::co_spawn(io,
        provider->invoke_async(wire::request("openai.responses", ProviderMode::Stream, "caller " + std::to_string(i))), asio::use_future));
    io.run();
    std::vector<sp::runtime::Result> owned;
    for (auto& result : results) {
        owned.push_back(result.get());
        ASSERT_TRUE(owned.back());
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(*owned.back()));
        EXPECT_EQ(test::text(owned.back()), "동시 응답");
        const auto& usage = test::completion(owned.back()).usage;
        ASSERT_TRUE(usage.input_total); ASSERT_TRUE(usage.output_total);
        ASSERT_TRUE(usage.total); ASSERT_TRUE(usage.provider_reported_total);
        EXPECT_EQ(usage.input_total->value, 3u);
        EXPECT_EQ(usage.output_total->value, 2u);
        EXPECT_EQ(usage.total->value, 5u);
        EXPECT_EQ(usage.total->evidence, sp::Evidence::Derived);
        EXPECT_EQ(usage.provider_reported_total->value, 5u);
        EXPECT_EQ(usage.provider_reported_total->evidence, sp::Evidence::Reported);
        EXPECT_EQ(usage.stage, sp::UsageStage::Final);
        EXPECT_EQ(usage.quality, sp::UsageQuality::Consistent);
    }
    for (std::size_t i = 1; i < owned.size(); ++i) EXPECT_NE(owned[i], owned[i - 1]);
    EXPECT_EQ(peer.state->entered, 6u);
}

TEST(SchemaProviderStreamAsyncOuterIo, AbandonedHttpStreamNeverTouchesDestroyedIoContext) {
    wire::Peer peer(wire::responses_sse(), true); peer.state->hold = true;
    auto provider = wire::provider("openai.responses", peer.origin());
    auto callbacks = std::make_shared<std::atomic<int>>(0);
    auto request = wire::request("openai.responses", ProviderMode::Stream);
    request.on_event = [owned = callbacks](const sp::Event&) { ++*owned; };
    auto io = std::make_unique<asio::io_context>();
    auto result = asio::co_spawn(*io, provider->invoke_async(std::move(request)), asio::use_future);
    std::thread runner([context = io.get()] { context->run(); });
    const bool entered = peer.await_requests(1);
    io->stop(); runner.join();
    const auto begin = std::chrono::steady_clock::now();
    io.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 1s);
    peer.release(); provider.reset();
    ASSERT_TRUE(entered);
    EXPECT_EQ(callbacks->load(), 0);
}

TEST(SchemaProviderStreamAsyncOuterIo, StalledConsumerOverflowRetainsDrainedOwnedOutcome) {
    sp::runtime::Result retained;
    auto callbacks = std::make_shared<std::atomic<int>>(0);
    bool overflow_observed = false;
    {
        auto final = json::parse(wire::responses_body("owned overflow text"));
        const json image = {{"type", "image_generation_call"}, {"id", "overflow-image"},
            {"status", "completed"}, {"result", "UE5H"}, {"output_format", "png"}};
        const auto text_item = final.at("output").at(0);
        final["output"] = json::array({image, text_item});
        final["usage"] = {{"input_tokens", 0}};
        auto initial = final; initial["output"] = json::array(); initial["status"] = "in_progress";
        auto frame = [](std::string type, json body) {
            body["type"] = type;
            return "event: " + type + "\ndata: " + body.dump() + "\n\n";
        };
        const auto batch = frame("response.created", {{"response", initial}})
            + frame("response.output_item.added", {{"output_index", 0}, {"item", image}})
            + frame("response.output_item.done", {{"output_index", 0}, {"item", image}})
            + frame("response.output_item.added", {{"output_index", 1}, {"item", {
                {"id", "msg-fixture"}, {"type", "message"}, {"role", "assistant"},
                {"status", "in_progress"}, {"content", json::array()}}}})
            + frame("response.content_part.added", {{"output_index", 1}, {"item_id", "msg-fixture"},
                {"content_index", 0}, {"part", {{"type", "output_text"}, {"text", ""}, {"annotations", json::array()}}}})
            + frame("response.output_text.delta", {{"output_index", 1}, {"item_id", "msg-fixture"},
                {"content_index", 0}, {"delta", "owned overflow text"}})
            + frame("response.output_text.done", {{"output_index", 1}, {"item_id", "msg-fixture"},
                {"content_index", 0}, {"text", "owned overflow text"}})
            + frame("response.output_item.done", {{"output_index", 1}, {"item", text_item}})
            + frame("response.completed", {{"response", final}});
        wire::Peer peer(batch, true);
        peer.state->hold = true;
        sp::runtime::Options options;
        options.default_timeout = 5s;
        options.workers = 1;
        options.retry_tokens = 0;
        options.retry_tokens_per_second = 0;
        options.limits.max_operations = 1;
        options.limits.queued_body_chunks = 17;
        options.limits.queued_body_bytes = 64 * 1024;
        auto client = std::make_shared<sp::runtime::Client>(test::descriptor("openai.responses", peer.origin()), options);
        auto provider = std::make_unique<wire::RuntimeProvider>(client, "openai.responses");
        auto request = wire::request("openai.responses", ProviderMode::Stream);
        request.observer_limits.max_events = 17;
        request.on_event = [owned = callbacks](const sp::Event&) { ++*owned; };
        asio::io_context io;
        auto result = asio::co_spawn(io, provider->invoke_async(std::move(request)), asio::use_future);
        io.poll();
        const bool entered = peer.await_requests(1);
        peer.release();
        ASSERT_TRUE(entered);

        // No outer handlers run here. SDK deliver() calls on_outcome before
        // releasing admission, so reclaiming its sole slot fences the actual
        // producer callback rather than merely fencing peer socket writes.
        bool producer_finished = false;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!producer_finished && std::chrono::steady_clock::now() < deadline) {
            try {
                auto probe = client->prepare(wire::request("openai.responses").payload);
                producer_finished = probe.valid();
            } catch (const sp::runtime::AdmissionError& error) {
                EXPECT_EQ(std::get<sp::Failure>(*error.outcome()).error.kind, sp::ErrorKind::ResourceLimit);
            }
            if (!producer_finished) std::this_thread::sleep_for(1ms);
        }
        EXPECT_TRUE(producer_finished);
        EXPECT_EQ(callbacks->load(), 0);
        provider.reset();
        io.restart();
        io.run();
        try {
            retained = result.get();
            ADD_FAILURE() << "discarded provider events must not return success";
        } catch (const ProviderObserverError& error) {
            overflow_observed = true;
            EXPECT_EQ(error.error_kind(), sp::ErrorKind::ResourceLimit);
            retained = error.outcome();
        }
    }
    ASSERT_TRUE(overflow_observed);
    ASSERT_TRUE(retained);
    const auto& messages = outcome_messages(*retained);
    const auto& usage = outcome_usage(*retained);
    ASSERT_EQ(messages.size(), 1u);
    ASSERT_EQ(messages[0].parts.size(), 2u);
    const auto& image = std::get<sp::Opaque>(messages[0].parts[0]);
    ASSERT_TRUE(image.wire_metadata);
    EXPECT_EQ(image.wire_type, "image_generation_call");
    EXPECT_EQ(image.wire_metadata->root().get("result").as_string(), "UE5H");
    EXPECT_EQ(std::get<sp::Text>(messages[0].parts[1]).value, "owned overflow text");
    ASSERT_TRUE(usage.input_total);
    EXPECT_EQ(usage.input_total->value, 0u);
    EXPECT_EQ(usage.input_total->evidence, sp::Evidence::Reported);
    EXPECT_FALSE(usage.output_total);
    EXPECT_FALSE(usage.total);
    EXPECT_FALSE(usage.provider_reported_total);
    EXPECT_EQ(usage.quality, sp::UsageQuality::Consistent);
    const auto& raw = std::visit([](const auto& outcome) -> const std::vector<sp::RawWire>& {
        using T = std::decay_t<decltype(outcome)>;
        if constexpr (std::is_same_v<T, sp::Completion>) return outcome.raw_events;
        else return outcome.partial.raw_events;
    }, *retained);
    ASSERT_FALSE(raw.empty());
    EXPECT_EQ(raw.front().type, "response.created");
    ASSERT_TRUE(raw.front().payload);
    EXPECT_EQ(raw.front().payload->root().get("response").get("id").as_string(), "response-fixture");
    const auto image_event = std::find_if(raw.begin(), raw.end(), [](const sp::RawWire& event) {
        return event.type == "response.output_item.added" && event.payload
            && event.payload->root().get("item").get("id").as_string() == "overflow-image";
    });
    ASSERT_NE(image_event, raw.end());
    EXPECT_EQ(image_event->payload->root().get("item").get("result").as_string(), "UE5H");
    if (const auto* failure = std::get_if<sp::Failure>(retained.get())) {
        EXPECT_EQ(failure->error.kind, sp::ErrorKind::Cancelled);
        EXPECT_EQ(usage.stage, sp::UsageStage::Partial);
        EXPECT_TRUE(failure->error.attempt.request_may_have_left);
        EXPECT_FALSE(messages[0].native && messages[0].native->complete());
        ASSERT_TRUE(failure->partial.wire_envelope);
        EXPECT_EQ(failure->partial.wire_envelope->root().get("id").as_string(), "response-fixture");
    } else {
        EXPECT_EQ(usage.stage, sp::UsageStage::Final);
        EXPECT_EQ(raw.back().type, "response.completed");
        ASSERT_TRUE(messages[0].native);
        EXPECT_TRUE(messages[0].native->complete());
        ASSERT_TRUE(std::get<sp::Completion>(*retained).wire_envelope);
    }
}
