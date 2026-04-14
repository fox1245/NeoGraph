// NeoGraph Example 18: Multi-Agent Debate (Researcher / Skeptic / Judge)
//
// Pattern: three agents with distinct system prompts argue about a claim,
// each seeing the shared transcript. After N rounds, a neutral Judge reads
// the full debate and renders a verdict with reasoning.
//
// Why this beats a single agent: forcing one agent to argue FOR a position
// and another to argue AGAINST it tends to surface failure modes that a
// single "be balanced" prompt glosses over. The Judge then adjudicates with
// full context of both strongest cases.
//
// Each agent is just a separate system prompt + a shared message history.
// That's the entire implementation — the pattern is about prompt design,
// not framework machinery.
//
// Usage:
//   OPENAI_API_KEY=sk-... ./example_multi_agent_debate

#include <neograph/neograph.h>
#include <neograph/llm/schema_provider.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace neograph;

static std::string speak(Provider& p,
                         const std::string& role_system,
                         const std::string& transcript,
                         float temperature) {
    CompletionParams params;
    params.model = "claude-sonnet-4-6";
    params.temperature = temperature;
    params.messages.push_back({"system", role_system});
    params.messages.push_back({"user", transcript});
    return p.complete(params).message.content;
}

int main() {
    const char* api_key = std::getenv("ANTHROPIC_API_KEY");
    if (!api_key) {
        std::cerr << "Set ANTHROPIC_API_KEY environment variable\n";
        return 1;
    }

    llm::SchemaProvider::Config cfg;
    cfg.schema_path = "claude";
    cfg.api_key = api_key;
    cfg.default_model = "claude-sonnet-4-6";
    auto provider = llm::SchemaProvider::create(cfg);

    std::cout << "\n╔══════════════════════════════════════════════════════╗\n"
              <<   "║  NeoGraph Example 18: Multi-Agent Debate              ║\n"
              <<   "╚══════════════════════════════════════════════════════╝\n\n";

    const std::string topic =
        "A seed-stage startup (5 engineers, pre-revenue) should adopt a "
        "microservices architecture from day one, rather than starting with "
        "a monolith.";

    const std::string researcher_sys =
        "You are the Proponent in a structured debate. You argue FOR the "
        "given claim with concrete technical and organizational reasoning. "
        "Keep each turn under 120 words. Be specific, cite trade-offs, and "
        "directly rebut the opposing side's most recent point when present. "
        "Do not hedge.";

    const std::string skeptic_sys =
        "You are the Opponent in a structured debate. You argue AGAINST the "
        "given claim with concrete technical and organizational reasoning. "
        "Keep each turn under 120 words. Be specific, cite trade-offs, and "
        "directly rebut the opposing side's most recent point when present. "
        "Do not hedge.";

    const std::string judge_sys =
        "You are a neutral judge evaluating a debate. You have no prior "
        "opinion. Read the full transcript and decide which side made the "
        "stronger case. Output EXACTLY this format:\n\n"
        "VERDICT: <PROPONENT|OPPONENT|DRAW>\n"
        "REASONING: <2-4 sentences explaining the decision>\n"
        "KEY_POINT: <the single most decisive argument in the debate>";

    std::cout << "Claim: " << topic << "\n\n";

    std::string transcript = "Claim: " + topic + "\n\n";
    const int ROUNDS = 2;

    for (int r = 1; r <= ROUNDS; ++r) {
        std::cout << "── Round " << r << " ─────────────────────────────────\n\n";

        // Proponent speaks
        std::string pro = speak(*provider, researcher_sys,
            transcript + "It is your turn to argue FOR the claim. "
                         "Write one paragraph.",
            0.7f);
        std::cout << "[Proponent]\n" << pro << "\n\n";
        transcript += "Proponent (round " + std::to_string(r) + "):\n" + pro + "\n\n";

        // Opponent speaks, sees the proponent's turn
        std::string con = speak(*provider, skeptic_sys,
            transcript + "It is your turn to argue AGAINST the claim. "
                         "Rebut the Proponent's most recent point directly.",
            0.7f);
        std::cout << "[Opponent]\n" << con << "\n\n";
        transcript += "Opponent (round " + std::to_string(r) + "):\n" + con + "\n\n";
    }

    // Judge
    std::cout << "── Verdict ──────────────────────────────────────────\n\n";
    std::string verdict = speak(*provider, judge_sys,
        transcript + "\nRender your verdict now.",
        0.2f);
    std::cout << verdict << "\n\n";

    return 0;
}
