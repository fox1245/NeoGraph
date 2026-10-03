// NeoGraph Example 56: Conversation history compaction
//
// Long-running sessions exceed a bounded token budget. Text-only old turns
// can be summarized while the full typed recent window remains verbatim.
// Tool/native groups are never flattened or silently discarded to repair
// an invalid slice; the canonical sanitizer rejects broken pairing.
//
// Offline: a MockProvider returns a canned summary — no API key.
//
//   ./example_history_compaction

#include <neograph/history.h>
#include <neograph/async/run_sync.h>
#include "provider_example_support.h"
#include <json/json.h>
#include <stdexcept>

#include <cstdio>
#include <string>
#include <vector>

using History = std::vector<sp::Message>;

// A full immutable outcome from a genuinely admitted local SDK request.
class SummaryMock : public neograph::Provider {
    std::shared_ptr<sp::runtime::Client> client_ = examples::make_local_client();
public:
    neograph::PreparedProviderRequest prepare(neograph::ProviderRequest request) override {
        auto history = std::make_shared<const History>(examples::request_messages(request));
        return prepare_local(client_, std::move(request),
            [history](const neograph::PreparedProviderRequest& prepared,
                      const std::function<void(const sp::Event&)>& observer)
                -> asio::awaitable<sp::runtime::Result> {
                std::size_t span = 0;
                for (const auto& message : *history)
                    if (message.role == sp::Role::User)
                        for (const auto& part : message.parts)
                            if (const auto* text = std::get_if<sp::Text>(&part))
                                span += text->value.size();
                sp::Completion completion;
                completion.messages.push_back(examples::message(sp::Role::Assistant,
                    "User is planning a 5-day Kyoto trip in April, vegetarian, "
                    "budget ~1500 USD, prefers temples over nightlife. "
                    "(summarized " + std::to_string(span) + " chars of history)"));
                completion.stop.kind = sp::StopKind::EndTurn;
                if (prepared.mode() == neograph::ProviderMode::Stream)
                    examples::emit_local_events(completion, observer);
                co_return std::make_shared<const sp::Outcome>(std::move(completion));
            });
    }
    std::string_view family() const noexcept override { return "openai.chat"; }
    std::string get_name() const override { return "summary-mock"; }
};

static void print_roles(const char* label,
                        const History& v) {
    std::printf("  %s (%zu msgs, ~%d tok): ", label, v.size(),
                neograph::history::estimate_tokens(v));
    for (const auto& m : v) {
        const char* role = "assistant";
        switch (m.role) {
            case sp::Role::System: role = "system"; break;
            case sp::Role::Developer: role = "developer"; break;
            case sp::Role::User: role = "user"; break;
            case sp::Role::Assistant: break;
            case sp::Role::Tool: role = "tool"; break;
        }
        std::printf("%s", role);
        for (const auto& part : m.parts)
            if (std::holds_alternative<sp::ToolCall>(part)) std::printf("[+call]");
        std::printf(" ");
    }
    std::printf("\n");
}

int main() {
    // ── 1. sanitize_tool_calls: a sliced history with broken pairs ──
    //
    // A user-controlled slice cannot reconstruct missing tool/native authority.
    // Show explicit rejection instead of deleting the original typed parts.
    {
        History sliced;
        sp::Message tool_only;
        tool_only.role = sp::Role::Tool;
        tool_only.parts.emplace_back(sp::ToolResult{"lost_call", "42"});
        sliced.push_back(std::move(tool_only));
        sliced.push_back(examples::message(sp::Role::User, "weather?"));
        auto parsed = sp::json::parse(R"({"city":"Kyoto"})");
        sp::Message assistant;
        assistant.parts.emplace_back(sp::ToolCall{
            "call_w", "get_weather", sp::ToolCallKind::ClientExecuted,
            std::make_shared<const sp::json::Document>(
                std::get<sp::json::Document>(std::move(parsed)))});
        sliced.push_back(std::move(assistant));

        std::printf("=== 1. sanitize_tool_calls ===\n");
        print_roles("before", sliced);
        try {
            neograph::history::sanitize_tool_calls(sliced);
            std::fprintf(stderr, "Broken pairing was unexpectedly admitted.\n");
            return 1;
        } catch (const std::invalid_argument& error) {
            std::printf("  rejected: %s\n", error.what());
        }
        print_roles("intact", sliced);
        std::printf("  (invalid slice retained; no tool parts fabricated or dropped)\n\n");
    }

    // ── 2. compact_history: long session over a tiny budget ─────────
    History hist;
    {
        hist.push_back(examples::message(
            sp::Role::System, "You are a helpful travel assistant."));
        for (int i = 0; i < 14; ++i) {
            hist.push_back(examples::message(sp::Role::User,
                "Turn " + std::to_string(i) +
                ": tell me more about Kyoto temples, food, and budget "
                "for my spring trip, in detail please."));
            hist.push_back(examples::message(sp::Role::Assistant,
                "Turn " + std::to_string(i) +
                ": here is a fairly long answer about Kyoto with many "
                "specifics so the token estimate climbs over budget."));
        }
    }

    SummaryMock mock;
    std::printf("=== 2. compact_history (max_tokens=200, keep=4) ===\n");
    print_roles("input ", hist);

    auto out = neograph::async::run_sync(
        neograph::history::compact_history(
            hist, mock, "~deepseek/deepseek-v4-flash-latest",
            /*max_tokens=*/200, /*recent_keep=*/4));

    // summary_outcome remains owned in out; printing summary is not a
    // substitute for its ordered parts, stop reason, or nullable usage.
    std::printf("  compacted: %s\n", out.compacted ? "yes" : "no");
    std::printf("  summary  : %s\n", out.summary.c_str());
    print_roles("output", out.recent);
    std::printf("\n  Original list is untouched: ");
    print_roles("orig  ", hist);

    std::printf("\nTakeaway: one LLM call summarizes eligible old text turns;\n"
                "the full recent typed history and summary outcome stay owned, "
                "and tool/native groups are never silently repaired.\n");
    return 0;
}
