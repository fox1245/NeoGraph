// NeoGraph Example 17: Self-Ask (question decomposition)
//
// Pattern: to answer a complex multi-hop question, the LLM explicitly decides
// whether a follow-up sub-question is needed, asks it, gets an answer from a
// "knowledge source", and feeds that answer back — repeating until it has
// enough to produce the final answer.
//
// This is structurally close to ReAct, but with a crucial difference:
// Self-Ask makes the *decomposition* explicit in the prompt. The LLM is
// forced to articulate "Are follow up questions needed?" and "Intermediate
// answer:" markers, which dramatically improves multi-hop reasoning.
//
// The "knowledge source" for the sub-questions here is the LLM itself with a
// focused system prompt. In a real system you'd swap this for web search,
// a SQL tool, or RAG retrieval.
//
// Usage:
//   OPENAI_API_KEY=sk-... ./example_self_ask

#include <neograph/neograph.h>
#include <neograph/llm/schema_provider.h>

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
              <<   "║  NeoGraph Example 17: Self-Ask                        ║\n"
              <<   "╚══════════════════════════════════════════════════════╝\n\n";

    const std::string question =
        "In the year the C++ programming language was first standardized by "
        "ISO, who was the Secretary-General of the United Nations?";

    // The system prompt establishes the Self-Ask protocol. The LLM must
    // produce either:
    //   Follow up: <sub-question>
    // or:
    //   So the final answer is: <answer>
    // We parse both markers in a loop.
    const std::string system =
        "You answer complex questions using the Self-Ask protocol. The "
        "conversation happens over MULTIPLE turns — do not try to answer "
        "in one turn.\n\n"
        "RULES (strict):\n"
        "1. Your ENTIRE reply for this turn must be EXACTLY ONE line.\n"
        "2. That single line must be one of these two forms and nothing "
        "else:\n"
        "     Follow up: <a single factual sub-question>\n"
        "   — or —\n"
        "     So the final answer is: <final answer>\n"
        "3. Never include both forms in the same reply.\n"
        "4. Never include reasoning, thinking-out-loud, reconsideration, "
        "or explanation. Do not use phrases like 'let me reconsider' or "
        "'actually'.\n"
        "5. After you emit 'Follow up:', the user will reply with an "
        "'Intermediate answer:' line. Only then may you emit your next "
        "turn.\n"
        "6. Emit 'So the final answer is:' only when you have gathered "
        "all the intermediate answers you need.";

    std::vector<ChatMessage> history;
    history.push_back({"system", system});
    history.push_back({"user", "Question: " + question});

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
}
