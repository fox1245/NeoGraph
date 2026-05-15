// NeoGraph Example 56: Conversation history compaction
//
// Long-running chat sessions grow unbounded. neograph::history
// compacts them: when the estimated token count exceeds a budget,
// older turns are summarized by one LLM call and only the last N
// messages are kept verbatim. It also repairs tool-call/response pairs
// that a window/truncation can break (OpenAI 400s on a dangling pair).
//
// Offline: a MockProvider returns a canned summary — no API key.
//
//   ./example_history_compaction

#include <neograph/history.h>
#include <neograph/async/run_sync.h>

#include <cstdio>
#include <string>
#include <vector>

using neograph::ChatMessage;

// Canned-summary provider so the example runs with no network/key.
// A real run would pass an OpenAIProvider here unchanged.
class SummaryMock : public neograph::Provider {
public:
    neograph::ChatCompletion
    complete(const neograph::CompletionParams& params) override {
        neograph::ChatCompletion r;
        r.message.role = "assistant";
        // Echo how much we were asked to compress, to prove the old
        // span (not the recent window) was what got summarized.
        std::size_t span = params.messages.empty()
            ? 0 : params.messages.back().content.size();
        r.message.content =
            "User is planning a 5-day Kyoto trip in April, vegetarian, "
            "budget ~1500 USD, prefers temples over nightlife. "
            "(summarized " + std::to_string(span) + " chars of history)";
        return r;
    }
    std::string get_name() const override { return "summary-mock"; }
};

static void print_roles(const char* label,
                         const std::vector<ChatMessage>& v) {
    std::printf("  %s (%zu msgs, ~%d tok): ", label, v.size(),
                neograph::history::estimate_tokens(v));
    for (const auto& m : v) {
        std::printf("%s", m.role.c_str());
        if (!m.tool_calls.empty()) std::printf("[+call]");
        std::printf(" ");
    }
    std::printf("\n");
}

int main() {
    // ── 1. sanitize_tool_calls: a sliced history with broken pairs ──
    //
    // A truncation kept an assistant tool_call whose tool response was
    // cut off, AND a tool message whose call was cut off. Both are
    // API-invalid; sanitize drops exactly those.
    {
        std::vector<ChatMessage> sliced;
        ChatMessage tool_only;
        tool_only.role = "tool";
        tool_only.tool_call_id = "lost_call";  // no preceding call
        tool_only.content = "42";
        sliced.push_back(tool_only);

        ChatMessage u; u.role = "user"; u.content = "weather?";
        sliced.push_back(u);

        ChatMessage a; a.role = "assistant";
        a.tool_calls = {{"call_w", "get_weather", R"({"city":"Kyoto"})"}};
        sliced.push_back(a);            // call with no following response

        std::printf("=== 1. sanitize_tool_calls ===\n");
        print_roles("before", sliced);
        neograph::history::sanitize_tool_calls(sliced);
        print_roles("after ", sliced);
        std::printf("  (orphan tool response + unanswered call removed; "
                    "user kept)\n\n");
    }

    // ── 2. compact_history: long session over a tiny budget ─────────
    std::vector<ChatMessage> hist;
    {
        ChatMessage sys;
        sys.role = "system";
        sys.content = "You are a helpful travel assistant.";
        hist.push_back(sys);
        for (int i = 0; i < 14; ++i) {
            ChatMessage u;
            u.role = "user";
            u.content = "Turn " + std::to_string(i) +
                ": tell me more about Kyoto temples, food, and budget "
                "for my spring trip, in detail please.";
            hist.push_back(u);
            ChatMessage as;
            as.role = "assistant";
            as.content = "Turn " + std::to_string(i) +
                ": here is a fairly long answer about Kyoto with many "
                "specifics so the token estimate climbs over budget.";
            hist.push_back(as);
        }
    }

    SummaryMock mock;
    std::printf("=== 2. compact_history (max_tokens=200, keep=4) ===\n");
    print_roles("input ", hist);

    auto out = neograph::async::run_sync(
        neograph::history::compact_history(
            hist, mock, "gpt-4o-mini",
            /*max_tokens=*/200, /*recent_keep=*/4));

    std::printf("  compacted: %s\n", out.compacted ? "yes" : "no");
    std::printf("  summary  : %s\n", out.summary.c_str());
    print_roles("output", out.recent);
    std::printf("\n  Original list is untouched: ");
    print_roles("orig  ", hist);

    std::printf("\nTakeaway: one LLM call collapses N old turns into a "
                "system-summary;\nthe last N stay verbatim, and the "
                "result is tool-pair safe.\n");
    return 0;
}
