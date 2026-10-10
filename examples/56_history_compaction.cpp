// NeoGraph Example 56: Conversation history compaction
//
// Long-running sessions exceed a bounded token budget. Text-only old turns
// can be summarized while the full typed recent window remains verbatim.
// Only a CLEAN summary (EndTurn/StopSequence, non-blank) replaces history; a truncated or
// refused summary keeps the input and reports why. The summary is model
// output, so it re-enters as labelled user-role context, never as system.
// Histories with native seals are kept whole: their seals bind the full prefix.
// Tool groups are never flattened or silently discarded to repair an invalid
// slice; the canonical sanitizer rejects broken pairing.
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
    sp::StopKind stop_;
public:
    explicit SummaryMock(sp::StopKind stop = sp::StopKind::EndTurn) : stop_(stop) {}
    neograph::PreparedProviderRequest prepare(neograph::ProviderRequest request) override {
        auto history = std::make_shared<const History>(examples::request_messages(request));
        return prepare_local(client_, std::move(request),
            [history, stop = stop_](const neograph::PreparedProviderRequest& prepared,
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
                completion.stop.kind = stop;
                if (prepared.mode() == neograph::ProviderMode::Stream)
                    examples::emit_local_events(completion, observer);
                co_return std::make_shared<const sp::Outcome>(std::move(completion));
            });
    }
    std::string_view family() const noexcept override { return "openai.chat"; }
    std::string get_name() const override { return "summary-mock"; }
};

static const char* status_name(neograph::history::CompactionStatus status) {
    using neograph::history::CompactionStatus;
    switch (status) {
        case CompactionStatus::UnderBudget: return "UnderBudget";
        case CompactionStatus::NothingEligible: return "NothingEligible";
        case CompactionStatus::Compacted: return "Compacted";
        case CompactionStatus::EmptySummary: return "EmptySummary";
        case CompactionStatus::UnusableSummary: return "UnusableSummary";
        case CompactionStatus::NativeReplayProtected: return "NativeReplayProtected";
    }
    return "?";
}

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

    // Explicit controls replace the historical 0.2 / 500 defaults. Leave
    // temperature and reasoning unset for models that reject those overrides.
    neograph::ProviderControls controls;
    controls.max_output_tokens = 1500;
    auto out = neograph::async::run_sync(
        neograph::history::compact_history(
            hist, mock, "~deepseek/deepseek-v4-flash-latest",
            /*max_tokens=*/200, /*recent_keep=*/4, controls));

    // summary_outcome remains owned in out; printing summary is not a
    // substitute for its ordered parts, stop reason, or nullable usage.
    std::printf("  status   : %s (compacted: %s)\n", status_name(out.status),
                out.compacted ? "yes" : "no");
    std::printf("  covered  : input messages [%zu, %zu) replaced by recent[%zu]\n",
                out.summarized_begin, out.summarized_end, out.summarized_begin);
    std::printf("  summary  : %s\n", out.summary.c_str());
    print_roles("output", out.recent);
    if (out.compacted && out.recent[out.summarized_begin].role == sp::Role::System) {
        std::fprintf(stderr, "Summary was promoted to system authority.\n");
        return 1;
    }
    std::printf("\n  Original list is untouched: ");
    print_roles("orig  ", hist);

    // ── 3. A truncated summary must not replace history ─────────────
    SummaryMock truncating(sp::StopKind::MaxTokens);
    std::printf("\n=== 3. truncated summary (summarizer stops at max_tokens) ===\n");
    auto kept = neograph::async::run_sync(
        neograph::history::compact_history(
            hist, truncating, "~deepseek/deepseek-v4-flash-latest",
            /*max_tokens=*/200, /*recent_keep=*/4, controls));
    std::printf("  status   : %s (compacted: %s)\n", status_name(kept.status),
                kept.compacted ? "yes" : "no");
    print_roles("output", kept.recent);
    if (kept.compacted || kept.recent.size() != hist.size()) {
        std::fprintf(stderr, "Truncated summary replaced the history.\n");
        return 1;
    }

    std::printf("\nTakeaway: one LLM call summarizes eligible old text turns;\n"
                "only a clean summary replaces them, as labelled user-role context;\n"
                "the full recent typed history and summary outcome stay owned, "
                "and tool/native groups are never silently repaired.\n");
    return 0;
}
