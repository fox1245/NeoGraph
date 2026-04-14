// NeoGraph Example 19: REWOO (Reasoning WithOut Observation)
//
// Pattern: the Planner commits to a complete multi-step plan up front, using
// placeholder variables like #E1, #E2 for the not-yet-computed tool results.
// A Worker then executes ALL tool calls in parallel (they are independent by
// construction), and a Solver synthesizes the final answer from the question
// plus all the filled-in evidence.
//
// Contrast with ReAct: ReAct interleaves thought -> tool -> observation ->
// thought, which serializes tool latency. REWOO front-loads the reasoning,
// then fans the I/O out in one shot. When tools are slow (network, LLM,
// DB), the wall-clock savings are substantial — at the cost of losing the
// ability to adapt mid-plan.
//
// Contrast with example 14 (Plan & Executor): 14 uses the graph engine's
// Send machinery and checkpoint store. This example is a raw LLM-only
// implementation showing the pattern's essence: Planner -> parallel Workers
// -> Solver. Three LLM roles, no graph, ~200 lines.
//
// The "tools" here are focused LLM calls with narrow system prompts
// (calculator, country_lookup, year_lookup). Swap them for real tools
// (web search, SQL, RAG) without touching the orchestration.
//
// Usage:
//   OPENAI_API_KEY=sk-... ./example_rewoo

#include <neograph/neograph.h>
#include <neograph/llm/schema_provider.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace neograph;

struct Step {
    std::string id;         // e.g. "E1"
    std::string tool;       // e.g. "year_lookup"
    std::string argument;   // e.g. "When was C++17 standardized?"
    std::string result;     // filled in after execution
};

// A fresh provider per worker — avoids sharing one cpp-httplib client
// across threads. Uses SchemaProvider with the "claude" built-in schema
// so requests go to Anthropic's /v1/messages endpoint.
static std::unique_ptr<Provider> make_provider(const std::string& api_key) {
    llm::SchemaProvider::Config cfg;
    cfg.schema_path = "claude";
    cfg.api_key = api_key;
    cfg.default_model = "claude-sonnet-4-6";
    return llm::SchemaProvider::create(cfg);
}

static std::string complete_one(Provider& p,
                                const std::string& system,
                                const std::string& user,
                                float temperature) {
    CompletionParams params;
    params.model = "claude-sonnet-4-6";
    params.temperature = temperature;
    params.messages.push_back({"system", system});
    params.messages.push_back({"user", user});
    return p.complete(params).message.content;
}

// Tools implemented as LLM calls with narrow system prompts.
static std::string tool_year_lookup(Provider& p, const std::string& q) {
    return complete_one(p,
        "You are a historical reference. Answer the question with ONLY a "
        "year (4 digits), nothing else.", q, 0.0f);
}
static std::string tool_country_lookup(Provider& p, const std::string& q) {
    return complete_one(p,
        "You are a geographical reference. Answer with ONLY a country name, "
        "nothing else.", q, 0.0f);
}
static std::string tool_fact_lookup(Provider& p, const std::string& q) {
    return complete_one(p,
        "You are a factual reference. Answer in under 15 words, no "
        "preamble.", q, 0.0f);
}
static std::string tool_calculator(Provider& p, const std::string& q) {
    return complete_one(p,
        "You are a calculator. Evaluate the arithmetic expression or word "
        "problem and reply with ONLY the numeric result.", q, 0.0f);
}

static std::string run_tool(Provider& p,
                            const std::string& tool,
                            const std::string& arg) {
    if (tool == "year_lookup")    return tool_year_lookup(p, arg);
    if (tool == "country_lookup") return tool_country_lookup(p, arg);
    if (tool == "fact_lookup")    return tool_fact_lookup(p, arg);
    if (tool == "calculator")     return tool_calculator(p, arg);
    return "ERROR: unknown tool '" + tool + "'";
}

// Parse lines of the form: E1 = tool_name["argument text"]
static std::vector<Step> parse_plan(const std::string& plan_text) {
    std::vector<Step> steps;
    std::regex line_re(R"(E(\d+)\s*=\s*(\w+)\s*\[\s*\"([^\"]*)\"\s*\])");
    auto begin = std::sregex_iterator(plan_text.begin(), plan_text.end(), line_re);
    auto end   = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        Step s;
        s.id       = "E" + (*it)[1].str();
        s.tool     = (*it)[2].str();
        s.argument = (*it)[3].str();
        steps.push_back(std::move(s));
    }
    return steps;
}

int main() {
    const char* api_key_env = std::getenv("ANTHROPIC_API_KEY");
    if (!api_key_env) {
        std::cerr << "Set ANTHROPIC_API_KEY environment variable\n";
        return 1;
    }
    const std::string api_key = api_key_env;

    auto planner_provider = make_provider(api_key);
    auto solver_provider  = make_provider(api_key);

    std::cout << "\n╔══════════════════════════════════════════════════════╗\n"
              <<   "║  NeoGraph Example 19: REWOO                           ║\n"
              <<   "╚══════════════════════════════════════════════════════╝\n\n";

    const std::string question =
        "In the year the C++17 language standard was published by ISO, "
        "who was the President of the United States? What is the capital "
        "of the country he led? And how many years ago from 2026 was that "
        "year?";

    std::cout << "Question: " << question << "\n\n";

    // ── PLAN ─────────────────────────────────────────────────────────────
    const std::string planner_sys =
        "You are a Planner. Given a question, decompose it into a sequence of "
        "independent tool calls that, when executed, collectively provide the "
        "evidence needed to answer the question. Use ONLY these tools:\n"
        "  year_lookup     -> returns a 4-digit year\n"
        "  country_lookup  -> returns a country name\n"
        "  fact_lookup     -> returns a short factual answer\n"
        "  calculator      -> evaluates arithmetic\n\n"
        "Output EXACTLY one plan step per line in this format:\n"
        "  E1 = tool_name[\"argument text\"]\n"
        "  E2 = tool_name[\"argument text\"]\n\n"
        "You may reference earlier results with #E1, #E2, etc. Steps that do "
        "not reference earlier results will be run in PARALLEL, so prefer "
        "independent steps when possible. Do not output anything other than "
        "the plan lines.";

    std::string plan_text = complete_one(*planner_provider, planner_sys,
        "Question:\n" + question + "\n\nPlan:", 0.2f);

    std::cout << "── Plan ─────────────────────────────────────────────\n"
              << plan_text << "\n\n";

    auto steps = parse_plan(plan_text);
    if (steps.empty()) {
        std::cerr << "Planner produced no parseable steps.\n";
        return 1;
    }

    // ── WORK (parallel fan-out for steps without dependencies) ──────────
    // Simple dependency model: a step depends on #EN if its argument
    // contains that token. We batch into layers; within a layer every
    // step runs in parallel via std::async.
    std::vector<bool> done(steps.size(), false);

    auto ready = [&](size_t i) {
        if (done[i]) return false;
        for (size_t j = 0; j < steps.size(); ++j) {
            if (i == j || done[j]) continue;
            if (steps[i].argument.find("#" + steps[j].id) != std::string::npos)
                return false;
        }
        return true;
    };

    // Substitute #EN placeholders with actual results.
    auto substitute = [&](std::string arg) {
        for (const auto& s : steps) {
            if (s.result.empty()) continue;
            std::string token = "#" + s.id;
            size_t pos;
            while ((pos = arg.find(token)) != std::string::npos)
                arg.replace(pos, token.size(), s.result);
        }
        return arg;
    };

    int layer = 0;
    auto t_start = std::chrono::steady_clock::now();

    while (true) {
        std::vector<size_t> batch;
        for (size_t i = 0; i < steps.size(); ++i)
            if (ready(i)) batch.push_back(i);
        if (batch.empty()) break;

        ++layer;
        std::cout << "── Layer " << layer << " — " << batch.size()
                  << " parallel tool call(s) ─────────\n";

        std::vector<std::future<std::string>> futures;
        futures.reserve(batch.size());
        for (size_t idx : batch) {
            std::string arg = substitute(steps[idx].argument);
            std::string tool = steps[idx].tool;
            std::string api_key_copy = api_key;
            std::cout << "  [" << steps[idx].id << "] " << tool
                      << "(\"" << arg << "\")\n";
            futures.push_back(std::async(std::launch::async,
                [tool, arg, api_key_copy]() {
                    auto p = make_provider(api_key_copy);
                    return run_tool(*p, tool, arg);
                }));
        }

        for (size_t k = 0; k < batch.size(); ++k) {
            std::string res = futures[k].get();
            steps[batch[k]].result = res;
            done[batch[k]] = true;
            std::cout << "    → " << steps[batch[k]].id << " = " << res << "\n";
        }
        std::cout << "\n";
    }

    auto t_end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();

    // ── SOLVE ────────────────────────────────────────────────────────────
    std::ostringstream evidence;
    for (const auto& s : steps) {
        evidence << s.id << " (" << s.tool << " \"" << s.argument << "\") = "
                 << s.result << "\n";
    }

    const std::string solver_sys =
        "You are a Solver. Given the original question and a set of evidence "
        "collected by tools, produce the final answer. Be concise.";

    std::string answer = complete_one(*solver_provider, solver_sys,
        "Question:\n" + question + "\n\nEvidence:\n" + evidence.str() +
        "\nFinal answer:", 0.2f);

    std::cout << "── Final answer ─────────────────────────────────────\n"
              << answer << "\n\n"
              << "(" << steps.size() << " tool calls across " << layer
              << " layers, work phase took " << ms << " ms)\n\n";
    return 0;
}
