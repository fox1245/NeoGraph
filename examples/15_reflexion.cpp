// NeoGraph Example 15: Reflexion (self-correction loop)
//
// Implements the three-model architecture from Shinn et al. 2023,
// "Reflexion: Language Agents with Verbal Reinforcement Learning"
// (arXiv:2303.11366):
//
//   ┌────────┐  draft  ┌────────┐  PASS/FAIL  ┌──────────┐  reflection
//   │ Actor  │────────>│Evaluator│────────────>│Self-     │──────┐
//   │  (M_a) │         │  (M_e)  │             │Reflection│      │
//   └────┬───┘         └─────────┘             │  (M_sr)  │      │
//        │                                     └──────────┘      │
//        │             ┌─────────────────────────────────────────┘
//        │             │  appends to mem (bounded, Ω=3)
//        └─────────────┘  next iteration's Actor sees mem
//
// Three distinct LLM roles, NOT two: the Self-Reflection model converts a
// (trajectory, evaluator-verdict) pair into a short verbal "lesson" that
// gets accumulated in a persistent memory buffer `mem` and fed back into
// the *next* trial's Actor — separate from (and instead of) the raw critique.
// This is what distinguishes Reflexion from Self-Refine (Madaan et al. 2023):
// Self-Refine feeds the raw critic output back; Reflexion distills it to a
// reusable lesson and keeps a history.
//
// Wired through SchemaProvider ("claude" schema) so the calls go to the
// Anthropic /v1/messages endpoint. Sonnet's syllable-counting and
// constraint-verification is reliable enough for the critic to actually
// do its job — with a weaker model this loop hallucinates violations and
// never converges.
//
// Task: write a haiku about C++ RAII with exact 5-7-5 syllable constraints.
// The critic is strict, so the first draft almost never passes.
//
// Usage:
//   echo 'ANTHROPIC_API_KEY=sk-ant-...' > .env
//   ./example_reflexion
// (auto-loads .env from the cwd or any parent directory.)

#include <neograph/neograph.h>
#include <neograph/llm/schema_provider.h>

#include <cppdotenv/dotenv.hpp>

#include <cstdlib>
#include <deque>
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
    cppdotenv::auto_load_dotenv();

    try {
    const char* api_key = std::getenv("ANTHROPIC_API_KEY");
    if (!api_key) {
        std::cerr << "Set ANTHROPIC_API_KEY environment variable "
                     "(or put it in .env beside the binary)\n";
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

    // Self-Reflection model — converts a (draft, evaluator-critique) pair into
    // a short *durable lesson* that's worth carrying forward to the next
    // attempt. Per the paper, the reflection should be more abstract and more
    // generalisable than the raw critic output: not "line 2 has 8 syllables
    // instead of 7" but "I tend to overshoot the middle line by one syllable
    // when I use trochaic words; prefer iambic tokens to land exactly on 7".
    const std::string reflector_sys =
        "You are a self-reflection model. You are given an attempt that "
        "failed an evaluator's checks and the evaluator's critique. Your job "
        "is to emit ONE short paragraph (2-4 sentences) of *strategic* "
        "lessons the next attempt should remember — distilled, actionable, "
        "and more general than 'line 2 is wrong'. Speak in the first person "
        "('I will…', 'I should avoid…'). Do NOT restate the critique "
        "verbatim. Do NOT mention specific words from the failed attempt — "
        "abstract to the *type* of mistake. Output the reflection only.";

    std::string draft;
    std::string critique;
    std::deque<std::string> mem;          // Bounded reflection buffer.
    const size_t MEM_OMEGA = 3;           // Paper's Ω ≈ 1–3.
    const int MAX_ITER = 6;

    for (int i = 1; i <= MAX_ITER; ++i) {
        std::cout << "── Iteration " << i << " ──────────────────────────────────────\n";

        // 1. ACTOR — generate, conditioning on the accumulated reflection
        //    memory `mem` (NOT on the raw critique).
        std::string gen_user;
        if (i == 1) {
            gen_user = "Task:\n" + task;
        } else {
            gen_user = "Task:\n" + task + "\n\n";
            gen_user += "Lessons from your past attempts on this task "
                        "(internalize these — do not quote them back):\n";
            int n = 1;
            for (auto& m : mem) {
                gen_user += "  [reflection " + std::to_string(n++) + "] " + m + "\n";
            }
            gen_user += "\nProduce a fresh attempt that applies these "
                        "lessons. Output exactly three lines, no commentary.";
        }

        auto gen = ask(*provider, generator_sys, gen_user, 0.8f);
        // Stronger models sometimes leak reasoning into the reply even when
        // told not to. Trust the last 3 non-empty lines — that's the haiku.
        draft = last_n_nonempty_lines(gen.message.content, 3);

        std::cout << "[actor]\n" << draft << "\n\n";

        // 2. EVALUATOR — score the trajectory.
        auto crit = ask(*provider, critic_sys,
            "Task constraints:\n" + task + "\n\nPoem to evaluate:\n" + draft,
            0.1f);
        critique = crit.message.content;

        std::cout << "[evaluator]\n" << critique << "\n\n";

        if (critique.find("VERDICT: ACCEPT") != std::string::npos) {
            std::cout << "✓ Accepted after " << i << " iteration(s) "
                      << "(mem held " << mem.size() << " reflection(s)).\n\n";
            std::cout << "── Final haiku ──────────────────────────────────────\n"
                      << draft << "\n\n";
            return 0;
        }

        // 3. SELF-REFLECTION — distil (draft, critique) into a lesson and
        //    push it into the bounded mem buffer. The next Actor iteration
        //    sees these lessons; the raw critique is discarded after this
        //    point. This is the structural difference from Self-Refine.
        auto refl = ask(*provider, reflector_sys,
            "Failed attempt:\n" + draft +
            "\n\nEvaluator's critique:\n" + critique +
            "\n\nWrite your reflection.",
            0.4f);
        std::string lesson = refl.message.content;
        // Keep the reflection compact — single paragraph.
        if (lesson.size() > 400) lesson.resize(400);

        std::cout << "[self-reflection] " << lesson << "\n\n";

        mem.push_back(std::move(lesson));
        while (mem.size() > MEM_OMEGA) mem.pop_front();
    }

    std::cout << "⚠ Reached max iterations (" << MAX_ITER
              << ") without acceptance. Last draft:\n\n"
              << draft << "\n\n";
    return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
