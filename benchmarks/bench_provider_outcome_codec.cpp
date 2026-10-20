// Provider outcome codec benchmark.
//
// Times encode_outcome and decode_outcome (neograph.provider-outcome/v3) on two
// fixed shapes, with no archive, provider, journal or I/O:
//
//   * chat  - one assistant message: cited text, usage, stop reason and a small
//             response envelope.
//   * agent - a turn with reasoning, three tool calls, a server tool result,
//             tool result messages, usage counters and raw wire events.
//
// Each shape is checked before timing: its decoded outcome must encode to the
// same record. Preparation and verification are not timed.
//
// Usage: bench_provider_outcome_codec [iterations] [warmup] [samples]

#include <neograph/provider_outcome_codec.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using neograph::json;
namespace codec = neograph::provider_codec;

std::shared_ptr<const sp::json::Document> document(std::string_view source) {
    auto parsed = sp::json::parse(source);
    if (auto* error = std::get_if<sp::json::ParseError>(&parsed))
        throw std::invalid_argument(error->message);
    return std::make_shared<const sp::json::Document>(
        std::get<sp::json::Document>(std::move(parsed)));
}

std::string filler(std::size_t bytes, char seed) {
    std::string value;
    value.reserve(bytes);
    for (std::size_t i = 0; i < bytes; ++i)
        value.push_back(static_cast<char>('a' + (static_cast<std::size_t>(seed) + i * 7) % 26));
    return value;
}

sp::Usage usage(std::uint64_t input, std::uint64_t output) {
    sp::Usage result;
    result.input_total = sp::Count{input, sp::Evidence::Reported};
    result.output_total = sp::Count{output, sp::Evidence::Reported};
    result.total = sp::Count{input + output, sp::Evidence::Derived};
    result.stage = sp::UsageStage::Final;
    return result;
}

sp::Outcome chat_outcome() {
    sp::Completion completion;
    sp::Message message;
    message.id = "msg-chat";
    message.role = sp::Role::Assistant;
    sp::Text answer{filler(900, 'c')};
    answer.citations.push_back(sp::Citation{
        "url_citation", "https://example.invalid/source", "Source", "", "", 12, 48,
        document(R"({"type":"url_citation","start_index":12,"end_index":48})")});
    message.parts.emplace_back(std::move(answer));
    completion.messages.push_back(std::move(message));
    completion.stop = sp::StopReason{sp::StopKind::EndTurn, "stop"};
    completion.usage = usage(312, 241);
    completion.usage.cache_read = sp::Count{128, sp::Evidence::Reported};
    completion.wire_envelope = document(
        R"({"id":"resp-chat","object":"chat.completion","created":1,"choices":[{"index":0,"finish_reason":"stop"}]})");
    completion.attempt = sp::AttemptEvidence{true, 2048, true, 0, 1, false};
    completion.model = sp::ServedModel{"fixture-model", "fixture-model-001", sp::ServedRelation::Snapshot};
    return completion;
}

sp::Outcome agent_outcome() {
    sp::Completion completion;
    sp::Message assistant;
    assistant.id = "msg-agent";
    assistant.role = sp::Role::Assistant;
    assistant.parts.emplace_back(sp::Reasoning{
        "reasoning-1", {"plan the lookups", "compare the results"}, filler(600, 'r'),
        std::string("completed"), {}});
    assistant.parts.emplace_back(sp::Thinking{filler(400, 't'), std::string("signature-0123456789")});
    sp::Text lead{"Looking these up. " + filler(300, 'l')};
    lead.citations.push_back(sp::Citation{
        "url_citation", "https://example.invalid/a", "A", "", "", 0, 18,
        document(R"({"type":"url_citation","start_index":0,"end_index":18})")});
    lead.citations.push_back(sp::Citation{
        "file_citation", "", "", "file-1", "notes.txt", std::nullopt, std::nullopt,
        document(R"({"type":"file_citation","file_id":"file-1"})")});
    assistant.parts.emplace_back(std::move(lead));
    for (int index = 0; index < 3; ++index) {
        const auto id = "call-" + std::to_string(index);
        assistant.parts.emplace_back(sp::ToolCall{
            id, "lookup_record", sp::ToolCallKind::ClientExecuted,
            document(R"({"record":)" + std::to_string(1000 + index) +
                     R"(,"fields":["name","status","owner","updated"],"include_history":true,"limit":25})")});
    }
    assistant.parts.emplace_back(sp::ServerToolResult{
        "server-call-0", "web_search_tool_result",
        document(R"({"results":[{"title":"One","url":"https://example.invalid/1"},)"
                 R"({"title":"Two","url":"https://example.invalid/2"}]})")});
    assistant.wire_output = document(
        R"([{"type":"reasoning","encrypted_content":"opaque"},{"type":"message"}])");

    sp::Message results;
    results.id = "msg-results";
    results.role = sp::Role::Tool;
    for (int index = 0; index < 3; ++index) {
        sp::ToolResult result{"call-" + std::to_string(index), filler(1200, static_cast<char>('x' + index)), false};
        result.host = sp::ToolResultHostMetadata{"lookup_record", "ok", false, false};
        results.parts.emplace_back(std::move(result));
    }

    completion.messages = {std::move(assistant), std::move(results)};
    completion.stop = sp::StopReason{sp::StopKind::ToolUse, "tool_calls"};
    completion.usage = usage(4180, 903);
    completion.usage.cache_read = sp::Count{3072, sp::Evidence::Reported};
    completion.usage.reasoning = sp::Count{412, sp::Evidence::Reported};
    completion.usage.extra["server_tool_use"] = sp::Count{1, sp::Evidence::Reported};
    completion.wire_envelope = document(
        R"({"id":"resp-agent","object":"response","status":"completed","output":[{"type":"reasoning"},)"
        R"({"type":"function_call"},{"type":"function_call"},{"type":"function_call"}]})");
    completion.attempt = sp::AttemptEvidence{true, 9216, true, 0, 1, false};
    completion.raw_events = {
        sp::RawWire{"response.created", document(R"({"type":"response.created","sequence":0})")},
        sp::RawWire{"response.completed", document(R"({"type":"response.completed","sequence":41})")}};
    completion.model = sp::ServedModel{"fixture-model", "fixture-model-001", sp::ServedRelation::Snapshot};
    return completion;
}

struct Shape {
    std::string name;
    sp::Outcome outcome;
    json encoded;
};

Shape prepare(std::string name, sp::Outcome outcome) {
    auto encoded = codec::encode_outcome(outcome);
    const auto decoded = codec::decode_outcome(encoded);
    if (!decoded || codec::encode_outcome(*decoded) != encoded)
        throw std::runtime_error(name + ": the decoded outcome encodes to a different record");
    return {std::move(name), std::move(outcome), std::move(encoded)};
}

std::size_t parse_positive(std::string_view text, const char* name) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0)
        throw std::invalid_argument(std::string(name) + " must be a positive integer");
    return value;
}

// Keeps each result observable so the optimizer cannot drop the call.
std::uint64_t sink = 0;

template <class Operation>
double median_per_op_us(std::size_t iterations, std::size_t warmup, std::size_t samples,
                        Operation&& operation) {
    for (std::size_t i = 0; i < warmup; ++i) operation();
    std::vector<std::int64_t> timings;
    timings.reserve(samples);
    for (std::size_t sample = 0; sample < samples; ++sample) {
        const auto started = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < iterations; ++i) operation();
        timings.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
    }
    std::sort(timings.begin(), timings.end());
    return static_cast<double>(timings[timings.size() / 2]) / 1000.0 /
           static_cast<double>(iterations);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 4)
            throw std::invalid_argument("usage: bench_provider_outcome_codec [iterations] [warmup] [samples]");
        const auto iterations = argc > 1 ? parse_positive(argv[1], "iterations") : std::size_t{500};
        const auto warmup = argc > 2 ? parse_positive(argv[2], "warmup") : std::size_t{20};
        const auto samples = argc > 3 ? parse_positive(argv[3], "samples") : std::size_t{5};

        std::vector<Shape> shapes;
        shapes.push_back(prepare("chat", chat_outcome()));
        shapes.push_back(prepare("agent", agent_outcome()));

        std::cout << "config\titerations\t" << iterations << '\n'
                  << "config\twarmup\t" << warmup << '\n'
                  << "config\tsamples\t" << samples << '\n'
                  << "runtime\tos\tLinux\n"
                  << "runtime\tcompiler\t" << __VERSION__ << '\n'
#ifdef NEOGRAPH_BENCH_BUILD_TYPE
                  << "runtime\tbuild_type\t" << NEOGRAPH_BENCH_BUILD_TYPE << '\n'
#else
                  << "runtime\tbuild_type\tunspecified\n"
#endif
#if defined(NDEBUG)
                  << "runtime\toptimization\toptimized\n"
#else
                  << "runtime\toptimization\tunoptimized\n"
#endif
                  << "header\tworkload\titerations\tmedian_us_per_op\n";
        for (const auto& shape : shapes) {
            const auto encode_us = median_per_op_us(iterations, warmup, samples, [&] {
                sink += codec::encode_outcome(shape.outcome).size();
            });
            const auto decode_us = median_per_op_us(iterations, warmup, samples, [&] {
                sink += codec::decode_outcome(shape.encoded)->index() + 1;
            });
            std::cout << "result\tencode_" << shape.name << '\t' << iterations << '\t' << encode_us << '\n'
                      << "result\tdecode_" << shape.name << '\t' << iterations << '\t' << decode_us << '\n';
        }
        for (const auto& shape : shapes)
            std::cout << "metric\tencoded_bytes_" << shape.name << '\t' << shape.encoded.dump().size() << '\n';
        return sink == 0 ? 3 : 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
