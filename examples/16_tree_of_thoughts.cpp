// NeoGraph Example 16: Tree of Thoughts (ToT)
//
// Pattern: at each depth, generate N candidate "thoughts", score each with a
// value LLM call, keep the top-K, and expand them at the next depth.
// The search tree is a breadth-first beam search over natural-language states.
//
// Task: a lightweight "Game of 24" style puzzle — given four numbers, find
// an expression that evaluates to a target. At each depth the LLM proposes
// partial expressions; an evaluator scores how promising each partial is.
//
// The pattern is what matters, not the puzzle: this same structure works for
// code generation, planning, or any problem where early steps constrain
// later ones and exploring multiple continuations pays off.
//
// Usage:
//   echo 'ANTHROPIC_API_KEY=sk-ant-...' > .env
//   ./example_tree_of_thoughts
// (auto-loads .env from the cwd or any parent directory.)

// Wired through SchemaProvider ("claude" schema) so the calls go to
// Anthropic's /v1/messages endpoint. Sonnet can reliably evaluate simple
// arithmetic — mini-tier models cannot, and the beam search degenerates.
#include <neograph/neograph.h>
#include <neograph/llm/schema_provider.h>

#include <cppdotenv/dotenv.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace neograph;

struct Node {
    std::string thought;  // the partial reasoning / state so far
    float       score;    // evaluator's score, 0..10
};

static ChatCompletion ask(Provider& p,
                          const std::string& system,
                          const std::string& user,
                          float temperature) {
    CompletionParams params;
    params.model = "claude-sonnet-4-6";
    params.temperature = temperature;
    params.messages.push_back({"system", system});
    params.messages.push_back({"user",   user});
    return p.complete(params);
}

// Ask the LLM to propose N continuations of `current_state`.
// We ask for one-per-line so we can split cheaply.
static std::vector<std::string> expand(Provider& p,
                                       const std::string& problem,
                                       const std::string& current_state,
                                       int n) {
    const std::string sys =
        "You are a problem solver exploring multiple solution paths. "
        "Given a problem and the current partial reasoning, produce " +
        std::to_string(n) + " DIFFERENT next-step thoughts, one per line. "
        "Each line should be a single concise step or partial expression. "
        "Do not number them. Do not add commentary.";

    const std::string usr =
        "Problem:\n" + problem +
        "\n\nCurrent partial reasoning (may be empty):\n" +
        (current_state.empty() ? "<none>" : current_state) +
        "\n\nProduce " + std::to_string(n) + " different next thoughts:";

    auto reply = ask(p, sys, usr, 0.9f);

    std::vector<std::string> out;
    std::string line;
    for (char c : reply.message.content) {
        if (c == '\n') {
            if (!line.empty()) out.push_back(line);
            line.clear();
        } else {
            line += c;
        }
    }
    if (!line.empty()) out.push_back(line);
    while ((int)out.size() > n) out.pop_back();
    return out;
}

// Ask the LLM to score a thought from 0..10 on how likely it leads to a
// correct solution. Returns 0 on parse failure.
//
// Critical design note: mini-tier models score sloppily unless forced to
// actually COMPUTE. The prompt below makes evaluation mechanical: if the
// thought contains a complete expression, the model must evaluate it
// step-by-step and award 10 only when it equals 24. This single change
// turns the evaluator from a hand-waving rater into a real verifier.
static float evaluate(Provider& p,
                      const std::string& problem,
                      const std::string& full_state) {
    const std::string sys =
        "You are a strict mathematical evaluator. Follow this protocol "
        "EXACTLY:\n"
        "  1. Read the partial reasoning.\n"
        "  2. If it contains ANY complete arithmetic expression using the "
        "input numbers, compute it step by step on one line. If the "
        "computed value equals 24, score = 10. If the computed value does "
        "NOT equal 24, score = 0. Do not round or approximate.\n"
        "  3. If it contains NO complete expression yet, score how "
        "promising the partial reasoning is on a scale 1..8.\n"
        "  4. Output EXACTLY two lines:\n"
        "       CALC: <your step-by-step calculation, or 'none'>\n"
        "       SCORE: <single integer 0..10>";
    const std::string usr =
        "Problem:\n" + problem +
        "\n\nPartial reasoning:\n" + full_state +
        "\n\nEvaluate:";

    auto reply = ask(p, sys, usr, 0.0f);
    // Extract the integer after "SCORE:"
    const std::string& txt = reply.message.content;
    size_t pos = txt.find("SCORE:");
    if (pos == std::string::npos) return 0.0f;
    try {
        return std::stof(txt.substr(pos + 6));
    } catch (...) {
        return 0.0f;
    }
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
              <<   "║  NeoGraph Example 16: Tree of Thoughts                ║\n"
              <<   "╚══════════════════════════════════════════════════════╝\n\n";

    const std::string problem =
        "Using the numbers 4, 7, 8, 8 exactly once each and the operators "
        "+ - * / (with parentheses allowed), produce an expression that "
        "equals 24. You may not introduce any other numbers. Show the "
        "final expression when you have one.";

    const int DEPTH       = 3;   // search depth
    const int BRANCHING   = 3;   // candidates per expansion
    const int BEAM_WIDTH  = 2;   // top-K kept per depth

    // Start with one empty root
    std::vector<Node> frontier = {{"", 0.0f}};

    for (int d = 0; d < DEPTH; ++d) {
        std::cout << "── Depth " << (d + 1) << " ─────────────────────────────────────\n";

        std::vector<Node> next_level;
        for (const auto& parent : frontier) {
            auto thoughts = expand(*provider, problem, parent.thought, BRANCHING);
            for (const auto& t : thoughts) {
                std::string combined =
                    parent.thought.empty() ? t : parent.thought + "\n" + t;
                float score = evaluate(*provider, problem, combined);
                next_level.push_back({combined, score});
                std::cout << "  [" << score << "] " << t << "\n";
            }
        }

        // Beam: keep top-K by score
        std::sort(next_level.begin(), next_level.end(),
                  [](const Node& a, const Node& b) { return a.score > b.score; });
        if ((int)next_level.size() > BEAM_WIDTH) next_level.resize(BEAM_WIDTH);
        frontier = std::move(next_level);

        std::cout << "  → kept top " << frontier.size() << " for next depth\n\n";
    }

    // Ask the best path for a final expression
    if (frontier.empty()) {
        std::cerr << "Search produced no candidates.\n";
        return 1;
    }

    const auto& best = frontier.front();
    std::cout << "── Best path (score " << best.score << ") ─────────────────\n"
              << best.thought << "\n\n";

    auto final_reply = ask(*provider,
        "You are a math solver. Given the problem and the reasoning trace, "
        "produce the final answer. You MUST:\n"
        "  1. Output a single arithmetic expression that uses each input "
        "number exactly once and only the allowed operators.\n"
        "  2. Compute it step-by-step to verify it equals the target.\n"
        "  3. If your first candidate does not equal the target, propose "
        "another and verify again. Do not output a wrong answer.\n\n"
        "Reply format (exactly two lines):\n"
        "  EXPR: <expression>\n"
        "  CHECK: <step-by-step calculation> = <result>",
        "Problem:\n" + problem + "\n\nReasoning so far:\n" + best.thought +
        "\n\nSolve:",
        0.0f);

    std::cout << "── Final expression ─────────────────────────────────\n"
              << final_reply.message.content << "\n\n";
    return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
