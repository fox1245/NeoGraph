// NeoGraph Example 15: Reflexion (self-correction loop)
//
// Pattern: Generator -> Critic -> (if REVISE) feed critique back -> Generator
// Repeats until the critic says ACCEPT or max_iterations is reached.
//
// Wired through SchemaProvider ("claude" schema) so the calls go to the
// Anthropic /v1/messages endpoint. Sonnet's syllable-counting and
// constraint-verification is reliable enough for the critic to actually
// do its job — with a weaker model this loop hallucinates violations and
// never converges.
//
// Demonstrates that Reflexion is not a NeoGraph feature per se — it's just
// two LLM calls with different system prompts in a loop, where the critic's
// output feeds back into the generator's next prompt.
//
// Task: write a haiku about C++ RAII with exact 5-7-5 syllable constraints.
// The critic is strict, so the first draft almost never passes.
//
// Usage:
//   OPENAI_API_KEY=sk-... ./example_reflexion

#include <neograph/neograph.h>
#include <neograph/llm/schema_provider.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace neograph;

// Stronger models sometimes "think out loud" even when told not to.
// Extract the last N non-empty lines from a draft — the haiku the model
// actually committed to is always at the very end of its reply.
static std::string last_n_nonempty_lines(const std::string& text, int n) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty()) lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    if ((int)lines.size() <= n) {
        std::string out;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) out += "\n";
            out += lines[i];
        }
        return out;
    }
    std::string out;
    for (int i = (int)lines.size() - n; i < (int)lines.size(); ++i) {
        if (i > (int)lines.size() - n) out += "\n";
        out += lines[i];
    }
    return out;
}

static ChatCompletion ask(Provider& p,
                          const std::string& system,
                          const std::string& user,
                          float temperature = 0.7f) {
    CompletionParams params;
    params.model = "claude-sonnet-4-6";
    params.temperature = temperature;
    params.messages.push_back({"system", system});
    params.messages.push_back({"user",   user});
    return p.complete(params);
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
              <<   "║  NeoGraph Example 15: Reflexion (self-correction)     ║\n"
              <<   "╚══════════════════════════════════════════════════════╝\n\n";

    // The constraints are chosen so that (a) Sonnet can verify them
    // mechanically but (b) the generator rarely hits every one on its
    // first attempt — Reflexion needs a task where the first draft is
    // plausibly wrong, otherwise the loop never fires.
    const std::string task =
        "Write a haiku about C++ RAII (Resource Acquisition Is Initialization). "
        "ALL of these constraints must hold:\n"
        "  (1) exactly 5-7-5 syllables across the three lines,\n"
        "  (2) the exact word 'scope' must appear as a standalone token "
        "(not 'scope's' or 'scoped' — 'scope' on its own),\n"
        "  (3) no word of 4+ letters may be repeated across the three lines,\n"
        "  (4) the first line must end with a comma,\n"
        "  (5) the last line must end with a period,\n"
        "  (6) the haiku must NOT contain any of these banned words: "
        "'memory', 'cleanup', 'pointer', 'object'.";

    const std::string generator_sys =
        "You are a meticulous poet. Output EXACTLY three lines — the haiku "
        "and nothing else. No preamble, no reasoning, no syllable counts, "
        "no bullet points, no commentary after. Your entire reply must be "
        "three lines of verse.";

    const std::string critic_sys =
        "You are a fair constraint checker. Do not be biased toward "
        "rejection — only reject when you can point to an ACTUAL violation. "
        "Work through every constraint in order and report PASS or FAIL for "
        "each with a one-phrase justification. Then give your verdict on "
        "the last line.\n\n"
        "Required reply format:\n"
        "  (1) PASS|FAIL — <reason>\n"
        "  (2) PASS|FAIL — <reason>\n"
        "  (3) PASS|FAIL — <reason>\n"
        "  (4) PASS|FAIL — <reason>\n"
        "  (5) PASS|FAIL — <reason>\n"
        "  (6) PASS|FAIL — <reason>\n"
        "  VERDICT: ACCEPT\n"
        "  — or —\n"
        "  VERDICT: REVISE: <which constraint numbers failed>\n\n"
        "If ALL six constraints show PASS, you MUST output VERDICT: ACCEPT. "
        "Do not invent violations.";

    std::string draft;
    std::string critique;
    const int MAX_ITER = 6;

    for (int i = 1; i <= MAX_ITER; ++i) {
        std::cout << "── Iteration " << i << " ──────────────────────────────────────\n";

        // 1. GENERATE
        std::string gen_user;
        if (i == 1) {
            gen_user = "Task:\n" + task;
        } else {
            gen_user = "Task:\n" + task +
                       "\n\nYour previous attempt:\n" + draft +
                       "\n\nCritic's feedback (READ CAREFULLY — the critic "
                       "is always right about syllable counts and "
                       "mechanical checks):\n" + critique +
                       "\n\nProduce an improved version that FIXES THE "
                       "SPECIFIC ISSUES the critic listed. If the critic "
                       "said a line had N syllables but needed M, you must "
                       "ACTUALLY change the word count or pick words with "
                       "different syllable counts — do not submit a line "
                       "with the same structure.";
        }

        auto gen = ask(*provider, generator_sys, gen_user, 0.8f);
        // Stronger models sometimes leak reasoning into the reply even when
        // told not to. Trust the last 3 non-empty lines — that's the haiku.
        draft = last_n_nonempty_lines(gen.message.content, 3);

        std::cout << "[generator]\n" << draft << "\n\n";

        // 2. CRITIQUE
        auto crit = ask(*provider, critic_sys,
            "Task constraints:\n" + task + "\n\nPoem to evaluate:\n" + draft,
            0.1f);
        critique = crit.message.content;

        std::cout << "[critic]\n" << critique << "\n\n";

        if (critique.find("VERDICT: ACCEPT") != std::string::npos) {
            std::cout << "✓ Accepted after " << i << " iteration(s).\n\n";
            std::cout << "── Final haiku ──────────────────────────────────────\n"
                      << draft << "\n\n";
            return 0;
        }
    }

    std::cout << "⚠ Reached max iterations (" << MAX_ITER
              << ") without acceptance. Last draft:\n\n"
              << draft << "\n\n";
    return 0;
}
