// NeoGraph Example 12: RAG (Retrieval-Augmented Generation) Agent
//
// Demonstrates how to build a RAG agent using NeoGraph's Tool interface.
// Uses an in-memory document store with keyword-based search as a mock
// vector database. In production, replace MockVectorStore with PGVector,
// Pinecone, or any vector DB.
//
// The agent:
//   1. Receives a user question
//   2. Calls the search tool to find relevant documents
//   3. Generates an answer grounded in the retrieved context
//
// Usage:
//   OPENAI_API_KEY=sk-... ./example_rag_agent
//
// For real vector search, see NexaGraph (https://github.com/fox1245/NexaGraph)
// which integrates PGVector + OpenAI embeddings + gRPC.

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/graph/react_graph.h>

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cctype>

// =========================================================================
// Mock Vector Store — In-memory document store with keyword search
// Replace this with PGVector / Pinecone / Weaviate for production use.
// =========================================================================

struct Document {
    std::string id;
    std::string title;
    std::string content;
    std::string source;
};

class MockVectorStore {
public:
    MockVectorStore() {
        // Pre-loaded knowledge base
        docs_ = {
            {"doc_1", "NeoGraph Overview",
             "NeoGraph is a C++17 graph-based agent orchestration engine. "
             "It brings LangGraph-level capabilities to C++ with zero Python dependency. "
             "Key features include JSON-defined graphs, parallel fan-out via Taskflow, "
             "checkpointing for time-travel debugging, and HITL support.",
             "README.md"},

            {"doc_2", "NeoGraph Architecture",
             "NeoGraph consists of four modules: neograph::core (graph engine), "
             "neograph::llm (LLM providers for OpenAI, Claude, Gemini), "
             "neograph::mcp (MCP client for JSON-RPC tool discovery), and "
             "neograph::util (lock-free RequestQueue). The core module has zero "
             "network dependencies.",
             "docs/architecture.md"},

            {"doc_3", "Send and Command",
             "Send enables dynamic fan-out: a node can spawn N parallel tasks at runtime "
             "by returning Send objects. Command enables routing override: a node can "
             "simultaneously update state AND control which node executes next, "
             "bypassing normal edge-based routing. Both are processed by the Taskflow "
             "work-stealing scheduler.",
             "docs/features.md"},

            {"doc_4", "Checkpointing and HITL",
             "NeoGraph supports full state snapshots at every super-step via the "
             "CheckpointStore interface. InMemoryCheckpointStore is provided for testing. "
             "Human-in-the-Loop (HITL) is supported through interrupt_before, "
             "interrupt_after, and dynamic NodeInterrupt exceptions. The resume() API "
             "continues execution from the interrupted checkpoint.",
             "docs/features.md"},

            {"doc_5", "Performance Comparison",
             "NeoGraph produces a ~5MB static binary vs ~500MB for Python+LangGraph. "
             "Memory usage is ~10MB vs ~300MB. Cold start is instant vs seconds. "
             "Parallel fan-out of 3 workers completing in 150ms tasks takes 151ms "
             "(parallel) vs 370ms (sequential). NeoGraph uses Taskflow work-stealing "
             "for true CPU parallelism, unlike Python's GIL-limited asyncio.",
             "README.md"},

            {"doc_6", "LLM Provider Support",
             "NeoGraph supports multiple LLM providers through two mechanisms: "
             "OpenAIProvider for any OpenAI-compatible API (OpenAI, Groq, Together, "
             "vLLM, Ollama), and SchemaProvider which adapts to any LLM API via a "
             "JSON schema. Built-in schemas are provided for OpenAI, Claude, and Gemini. "
             "New providers can be added by creating a JSON schema file.",
             "docs/providers.md"},

            {"doc_7", "Cross-thread Store",
             "The Store interface provides namespaced key-value storage that persists "
             "across threads. Use cases include long-term user preferences, shared "
             "knowledge, and agent memory. Namespace is a vector of strings forming a "
             "hierarchical path, e.g., {\"users\", \"user123\", \"preferences\"}. "
             "InMemoryStore is provided; implement the Store interface for Redis/DB.",
             "docs/features.md"},

            {"doc_8", "ReAct Agent Pattern",
             "The ReAct (Reasoning + Acting) pattern alternates between LLM calls and "
             "tool execution. NeoGraph provides create_react_graph() which builds a "
             "standard 2-node graph: llm_call -> tool_dispatch -> loop until done. "
             "The Agent class provides a simpler non-graph alternative. Both support "
             "streaming via callbacks.",
             "docs/patterns.md"},
        };
    }

    // Simple keyword-based search (replace with embedding similarity in production)
    std::vector<Document> search(const std::string& query, int top_k = 3) const {
        // Tokenize query into lowercase words
        auto query_words = tokenize(query);

        // Score each document by keyword overlap
        std::vector<std::pair<double, size_t>> scores;
        for (size_t i = 0; i < docs_.size(); ++i) {
            auto doc_words = tokenize(docs_[i].title + " " + docs_[i].content);
            double score = 0.0;
            for (const auto& qw : query_words) {
                for (const auto& dw : doc_words) {
                    if (dw.find(qw) != std::string::npos || qw.find(dw) != std::string::npos) {
                        score += 1.0;
                    }
                }
            }
            if (score > 0) {
                scores.push_back({score, i});
            }
        }

        // Sort by score descending
        std::sort(scores.begin(), scores.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

        // Return top-k
        std::vector<Document> results;
        for (int i = 0; i < top_k && i < static_cast<int>(scores.size()); ++i) {
            results.push_back(docs_[scores[i].second]);
        }
        return results;
    }

private:
    std::vector<Document> docs_;

    static std::vector<std::string> tokenize(const std::string& text) {
        std::vector<std::string> words;
        std::string word;
        for (char c : text) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            } else if (!word.empty()) {
                if (word.size() >= 3) words.push_back(word);  // skip short words
                word.clear();
            }
        }
        if (word.size() >= 3) words.push_back(word);
        return words;
    }
};

// =========================================================================
// RAG Search Tool — wraps MockVectorStore as a NeoGraph Tool
// =========================================================================

class RAGSearchTool : public neograph::Tool {
public:
    RAGSearchTool(std::shared_ptr<MockVectorStore> store) : store_(std::move(store)) {}

    neograph::ChatTool get_definition() const override {
        return {
            "search_documents",
            "Search the knowledge base for relevant documents. "
            "Use this tool to find information before answering questions. "
            "Returns the most relevant documents with their content and source.",
            neograph::json{
                {"type", "object"},
                {"properties", {
                    {"query", {
                        {"type", "string"},
                        {"description", "Search query to find relevant documents"}
                    }},
                    {"top_k", {
                        {"type", "integer"},
                        {"description", "Number of results to return (default: 3)"}
                    }}
                }},
                {"required", neograph::json::array({"query"})}
            }
        };
    }

    std::string execute(const neograph::json& args) override {
        auto query = args.value("query", "");
        int top_k = args.value("top_k", 3);

        auto results = store_->search(query, top_k);

        if (results.empty()) {
            return "No relevant documents found for: " + query;
        }

        neograph::json output = neograph::json::array();
        for (const auto& doc : results) {
            output.push_back({
                {"id", doc.id},
                {"title", doc.title},
                {"content", doc.content},
                {"source", doc.source}
            });
        }

        return output.dump(2);
    }

    std::string get_name() const override { return "search_documents"; }

private:
    std::shared_ptr<MockVectorStore> store_;
};

// =========================================================================
// Main — CLI RAG Agent
// =========================================================================

int main() {
    // 1. Check API key
    const char* api_key = std::getenv("OPENAI_API_KEY");
    if (!api_key) {
        std::cerr << "Set OPENAI_API_KEY environment variable\n";
        return 1;
    }

    // 2. Create provider
    auto provider = std::shared_ptr<neograph::Provider>(
        neograph::llm::OpenAIProvider::create({
            .api_key = api_key,
            .default_model = "gpt-4o-mini"
        })
    );

    // 3. Create vector store and tools
    auto store = std::make_shared<MockVectorStore>();

    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<RAGSearchTool>(store));

    // 4. Create ReAct graph
    auto engine = neograph::graph::create_react_graph(
        provider,
        std::move(tools),
        "You are a helpful assistant that answers questions about NeoGraph. "
        "ALWAYS use the search_documents tool first to find relevant information "
        "before answering. Base your answers on the retrieved documents. "
        "If the documents don't contain relevant information, say so. "
        "Cite the source document when possible."
    );

    // 5. Interactive CLI loop
    std::cout << "=== NeoGraph RAG Agent ===\n";
    std::cout << "Ask questions about NeoGraph. Type 'quit' to exit.\n";
    std::cout << "Knowledge base: 8 documents about NeoGraph features.\n\n";

    std::string input;
    while (true) {
        std::cout << "You: " << std::flush;
        std::getline(std::cin, input);

        if (input.empty()) continue;
        if (input == "quit" || input == "exit" || input == "q") break;

        // Build graph input
        neograph::graph::RunConfig config;
        config.input = {{"messages", neograph::json::array({
            {{"role", "user"}, {"content", input}}
        })}};
        config.max_steps = 10;

        std::cout << "\nAssistant: " << std::flush;

        // Run with streaming
        auto result = engine->run_stream(config,
            [](const neograph::graph::GraphEvent& event) {
                if (event.type == neograph::graph::GraphEvent::Type::LLM_TOKEN) {
                    std::cout << event.data.get<std::string>() << std::flush;
                } else if (event.type == neograph::graph::GraphEvent::Type::NODE_START
                           && event.node_name == "tools") {
                    std::cerr << "\n  [searching documents...]\n";
                }
            });

        std::cout << "\n\n";
    }

    std::cout << "Goodbye!\n";
    return 0;
}
