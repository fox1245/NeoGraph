// NeoGraph Example 17: Self-Ask (question decomposition)
//
// Implements the canonical Self-Ask protocol from Press et al. 2022,
// "Measuring and Narrowing the Compositionality Gap in Language Models"
// (arXiv:2210.03350). To answer a complex multi-hop question, the LLM
// explicitly decides whether a follow-up sub-question is needed, asks
// it, gets an answer from a "knowledge source", and feeds that answer
// back — repeating until it has enough to produce the final answer.
//
// Faithful Self-Ask requires three things in the prompt that this
// example uses:
//   1. The "Are follow up questions needed here:" scaffold, which the
//      paper found materially improved performance even on
//      instruction-tuned models — it forces the model to commit
//      Yes/No before continuing.
//   2. A few-shot exemplar showing the full
//      Question / Are follow up questions needed / Yes / Follow up /
//      Intermediate answer / ... / So the final answer is
//      sequence. The paper notes the rigid scaffold "makes it easier
//      for the model to state the final answer in a concise,
//      parseable way".
//   3. An external knowledge source that handles the sub-questions
//      (paper uses Google search with the SerpAPI in their SA+SE
//      configuration). This example uses a second LLM instance with
//      a terse-answerer system prompt — swap for web/SQL/RAG in a
//      real system.
//
// Usage:
//   echo 'ANTHROPIC_API_KEY=sk-ant-...' > .env
//   ./example_self_ask
// (auto-loads .env from the cwd or any parent directory.)

#include <neograph/neograph.h>
#include <neograph/llm/schema_provider.h>

#include <cppdotenv/dotenv.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

using namespace neograph;

// Look for a line that starts with `marker:` (case sensitive) in the text
// and return the rest of that line, trimmed. Returns empty string if not
// found.
static std::string extract_after(const std::string& text,
                                 const std::string& marker) {
    size_t pos = text.find(marker);
    if (pos == std::string::npos) return "";
    size_t start = pos + marker.size();
    // skip leading spaces
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
        ++start;
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    // trim trailing whitespace
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\r'))
        --end;
    return text.substr(start, end - start);
}

static ChatCompletion ask(Provider& p,
                          const std::vector<ChatMessage>& messages,
                          float temperature) {
    CompletionParams params;
    params.model = "claude-sonnet-4-6";
    params.temperature = temperature;
    params.messages = messages;
    return p.complete(params);
}

// The "knowledge source" — a second LLM instance with a terse-answerer
// system prompt. In production this would be a search tool or RAG lookup.
static std::string lookup(Provider& p, const std::string& sub_question) {
    CompletionParams params;
    params.model = "claude-sonnet-4-6";
    params.temperature = 0.0f;
    params.messages.push_back({"system",
        "You are a reference database. Given a factual question, answer with "
        "the shortest possible correct answer — usually a name, number, or "
        "date. Do not add explanation, do not add punctuation beyond what is "
        "necessary."});
    params.messages.push_back({"user", sub_question});
    return p.complete(params).message.content;
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
              <<   "║  NeoGraph Example 17: Self-Ask                        ║\n"
              <<   "╚══════════════════════════════════════════════════════╝\n\n";

    const std::string question =
        "In the year the C++ programming language was first standardized by "
        "ISO, who was the Secretary-General of the United Nations?";

    // System prompt establishes the Self-Ask protocol. Two key elements
    // from the paper:
    //   - The "Are follow up questions needed here:" scaffold (Press
    //     et al. 2022 §3): commits the model to Yes/No before
    //     emitting a sub-question or final answer. The paper found
    //     this single line materially helped smaller models stay on
    //     protocol.
    //   - A few-shot exemplar in the user message below: shows the
    //     model exactly the sequence of turns expected. Even on a
    //     strong instruction-tuned model, the few-shot pin reduces
    //     protocol drift to near-zero in our test runs.
    const std::string system =
        "You answer complex questions using the Self-Ask protocol. The "
        "conversation happens over MULTIPLE turns — do not try to answer "
        "in one turn.\n\n"
        "FORMAT — your reply for each turn must be EXACTLY one of these "
        "three shapes (always include the 'Are follow up questions "
        "needed here:' line first):\n\n"
        "  Shape A (more decomposition needed, two lines):\n"
        "    Are follow up questions needed here: Yes.\n"
        "    Follow up: <a single factual sub-question>\n\n"
        "  Shape B (you have all the evidence, two lines):\n"
        "    Are follow up questions needed here: No.\n"
        "    So the final answer is: <final answer>\n\n"
        "RULES (strict):\n"
        "1. Always emit the 'Are follow up questions needed here:' line "
        "first; the answer must be exactly 'Yes.' or 'No.'.\n"
        "2. Never include both 'Follow up:' and 'So the final answer is:' "
        "in the same turn.\n"
        "3. Never include reasoning, thinking-out-loud, reconsideration, "
        "or explanation outside these lines.\n"
        "4. After you emit 'Follow up:', the user will reply with an "
        "'Intermediate answer:' line. Only then may you emit your next "
        "turn.\n"
        "5. Emit 'So the final answer is:' only when you have gathered "
        "all the intermediate answers you need.";

    // One-shot exemplar — Press et al.'s SA paper-style canonical form.
    // We pin a fully-worked trajectory so the model imitates the exact
    // line shapes (the paper reports the rigid scaffolding is what
    // makes the final answer parseable).
    const std::string exemplar =
        "Here is a worked example of the protocol. Follow this format "
        "exactly when you reply.\n\n"
        "Question: When was the founder of craigslist born?\n"
        "Are follow up questions needed here: Yes.\n"
        "Follow up: Who was the founder of craigslist?\n"
        "Intermediate answer: Craig Newmark.\n"
        "Are follow up questions needed here: Yes.\n"
        "Follow up: When was Craig Newmark born?\n"
        "Intermediate answer: December 6, 1952.\n"
        "Are follow up questions needed here: No.\n"
        "So the final answer is: December 6, 1952.\n\n"
        "Now do the same for the question below.";

    std::vector<ChatMessage> history;
    history.push_back({"system", system});
    history.push_back({"user", exemplar + "\n\nQuestion: " + question});

    std::cout << "Question: " << question << "\n\n";

    const int MAX_HOPS = 6;
    for (int hop = 1; hop <= MAX_HOPS; ++hop) {
        auto reply = ask(*provider, history, 0.0f);
        const std::string text = reply.message.content;
        history.push_back({"assistant", text});

        std::cout << "── Hop " << hop << " ─────────────────────────────\n"
                  << text << "\n";

        std::string final_answer = extract_after(text, "So the final answer is:");
        if (!final_answer.empty()) {
            std::cout << "\n✓ Final answer: " << final_answer << "\n\n";
            return 0;
        }

        std::string sub_q = extract_after(text, "Follow up:");
        if (sub_q.empty()) {
            std::cout << "\n⚠ Model did not follow the protocol. Stopping.\n";
            return 1;
        }

        std::string sub_a = lookup(*provider, sub_q);
        std::cout << "  → Intermediate answer: " << sub_a << "\n\n";

        history.push_back({"user", "Intermediate answer: " + sub_a});
    }

    std::cout << "⚠ Reached max hops (" << MAX_HOPS << ") without final answer.\n";
    return 1;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
