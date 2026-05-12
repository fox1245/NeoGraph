// NeoGraph Example 41: Multi-turn chat via `resume_if_exists`
//
// Demonstrates the LangGraph-style multi-turn pattern called out in the
// README "Differences from LangGraph" section but not yet shown as a
// runnable C++ example:
//
//     cfg.thread_id        = "session-1";
//     cfg.input            = {{"messages", [new user turn]}};
//     cfg.resume_if_exists = true;   // <-- loads prior checkpoint,
//                                    //     applies input on top
//
// Run() with the same thread_id does *not* auto-load the previous
// turn's state by default (see RunConfig docstring), so without this
// flag the LangGraph port silently loses history. The example proves:
//
//   1. Turn 1 — fresh run (no checkpoint yet).
//   2. Turn 2 — same thread_id with resume_if_exists=true sees turn 1's
//      messages plus the new user message, in order.
//   3. Turn 3 — even with resume_if_exists=false on the same thread_id,
//      history is NOT carried (verifies the documented default).
//
// No API key required — uses a mock provider that just echoes the
// running conversation length.
//
// Usage: ./example_resume_if_exists_chat

#include <neograph/neograph.h>

#include <iostream>
#include <string>

using namespace neograph;
using namespace neograph::graph;

// Mock provider — reply names every prior user message back so we can
// SEE whether history flowed through.
class EchoProvider : public Provider {
public:
    ChatCompletion complete(const CompletionParams& params) override {
        ChatCompletion r;
        r.message.role = "assistant";

        std::string reply = "Heard so far: ";
        int n = 0;
        for (const auto& m : params.messages) {
            if (m.role == "user") {
                if (n++ > 0) reply += " | ";
                reply += m.content;
            }
        }
        reply += " (total user turns: " + std::to_string(n) + ")";

        r.message.content = reply;
        return r;
    }
    ChatCompletion complete_stream(const CompletionParams& p,
                                   const StreamCallback&) override {
        return complete(p);
    }
    std::string get_name() const override { return "echo"; }
};

static void print_messages(const json& state, const std::string& label) {
    std::cout << "  [" << label << "] channel `messages`:\n";
    if (!state.contains("channels") || !state["channels"].contains("messages")) {
        std::cout << "    (no messages channel)\n";
        return;
    }
    auto msgs = state["channels"]["messages"]["value"];
    if (!msgs.is_array()) {
        std::cout << "    (not an array)\n";
        return;
    }
    for (const auto& m : msgs) {
        std::string role    = m.value("role", "?");
        std::string content = m.value("content", "");
        std::cout << "      [" << role << "] " << content << "\n";
    }
}

int main() {
    auto provider = std::make_shared<EchoProvider>();

    NodeContext ctx;
    ctx.provider     = provider;
    ctx.instructions = "Echo user turn count.";

    json definition = {
        {"name", "chatbot"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes",    {{"llm",       {{"type", "llm_call"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "llm"}},
            {{"from", "llm"},       {"to", "__end__"}}
        })}
    };

    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(definition, ctx, store);

    const std::string thread = "session-1";

    // ── Turn 1 ────────────────────────────────────────────────────────
    std::cout << "── Turn 1 (fresh) ──────────────────────────────\n";
    {
        RunConfig cfg;
        cfg.thread_id        = thread;
        cfg.resume_if_exists = true;   // No checkpoint exists yet: behaves
                                       // like a fresh run (per RunConfig docs).
        cfg.input = {{"messages", json::array({
            {{"role", "user"}, {"content", "First message"}}
        })}};

        auto r = engine->run(cfg);
        print_messages(r.output, "after turn 1");
    }

    // ── Turn 2 (with resume_if_exists) — should carry turn 1's state ─
    std::cout << "\n── Turn 2 (resume_if_exists=true) ──────────────\n";
    {
        RunConfig cfg;
        cfg.thread_id        = thread;
        cfg.resume_if_exists = true;
        cfg.input = {{"messages", json::array({
            {{"role", "user"}, {"content", "Second message"}}
        })}};

        auto r = engine->run(cfg);
        print_messages(r.output, "after turn 2");

        // Assertion: the assistant's last reply should mention BOTH
        // user messages because the prior checkpoint loaded their
        // history into params.messages.
        // (neograph::json has no .back() — use size()-1 indexing.)
        auto msgs = r.output["channels"]["messages"]["value"];
        std::string last = msgs.size() > 0
                               ? msgs[msgs.size() - 1].value("content", "")
                               : std::string{};
        bool sees_first  = last.find("First message") != std::string::npos;
        bool sees_second = last.find("Second message") != std::string::npos;
        std::cout << "  assistant saw both turns: "
                  << std::boolalpha << (sees_first && sees_second) << "\n";
    }

    // ── Turn 3 (resume_if_exists=false on same thread) — should NOT ─
    //                                                    carry history.
    std::cout << "\n── Turn 3 (resume_if_exists=false, same thread) ──\n";
    {
        RunConfig cfg;
        cfg.thread_id        = thread;
        cfg.resume_if_exists = false;   // Default — fresh start.
        cfg.input = {{"messages", json::array({
            {{"role", "user"}, {"content", "Third message"}}
        })}};

        auto r = engine->run(cfg);
        print_messages(r.output, "after turn 3");

        auto msgs = r.output["channels"]["messages"]["value"];
        std::string last = msgs.size() > 0
                               ? msgs[msgs.size() - 1].value("content", "")
                               : std::string{};
        bool sees_first   = last.find("First message")  != std::string::npos;
        bool sees_third   = last.find("Third message")  != std::string::npos;
        std::cout << "  assistant saw turn 1 history: "
                  << std::boolalpha << sees_first << " (expected false)\n";
        std::cout << "  assistant saw turn 3 input  : "
                  << std::boolalpha << sees_third << " (expected true)\n";
    }

    std::cout << "\nResume-if-exists is opt-in; without it, callers must thread\n"
              << "state through `input` themselves (the LangGraph port pitfall).\n";
    return 0;
}
