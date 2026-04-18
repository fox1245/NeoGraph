// NeoGraph Example 12: RAG (Retrieval-Augmented Generation) Agent
//
// Demonstrates how to build a RAG agent using NeoGraph with real embeddings.
// Uses OpenAI text-embedding-3-small for vector similarity search and
// an in-memory vector store. In production, replace with PGVector / Pinecone.
//
// The agent:
//   1. On startup, embeds 8 documents into vectors
//   2. Receives a user question via CLI
//   3. Embeds the query, finds top-k similar documents (cosine similarity)
//   4. Generates an answer grounded in the retrieved context
//
// Usage:
//   OPENAI_API_KEY=sk-... ./example_rag_agent
//
// For production RAG with PGVector + gRPC, see NexaGraph.

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/graph/react_graph.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <numeric>

using json = neograph::json;

// =========================================================================
// Embedding Client — calls OpenAI /v1/embeddings API
// =========================================================================

class EmbeddingClient {
public:
    EmbeddingClient(const std::string& api_key,
                    const std::string& model = "text-embedding-3-small")
        : api_key_(api_key), model_(model) {}

    // Embed a single text, returns a float vector
    std::vector<float> embed(const std::string& text) const {
        auto results = embed_batch({text});
        return results.empty() ? std::vector<float>{} : results[0];
    }

    // Embed multiple texts in one API call
    std::vector<std::vector<float>> embed_batch(const std::vector<std::string>& texts) const {
        json body = json::object();
        body["model"] = model_;
        body["input"] = json(texts);

        httplib::Client cli("https://api.openai.com");
        cli.set_read_timeout(30, 0);

        httplib::Headers headers = {
            {"Authorization", "Bearer " + api_key_}
        };
        cli.set_default_headers(headers);

        auto res = cli.Post("/v1/embeddings", body.dump(), "application/json");

        if (!res || res->status != 200) {
            std::cerr << "[embedding] API error: "
                      << (res ? std::to_string(res->status) + " " + res->body : "connection failed")
                      << "\n";
            return {};
        }

        auto resp = json::parse(res->body);
        std::vector<std::vector<float>> embeddings;

        for (const auto& item : resp["data"]) {
            std::vector<float> vec;
            for (const auto& v : item["embedding"]) {
                vec.push_back(v.get<float>());
            }
            embeddings.push_back(std::move(vec));
        }

        return embeddings;
    }

private:
    std::string api_key_;
    std::string model_;
};

// =========================================================================
// Vector Store — in-memory with cosine similarity search
// =========================================================================

struct Document {
    std::string id;
    std::string title;
    std::string content;
    std::string source;
    std::vector<float> embedding;
};

static double cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot    += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    double denom = std::sqrt(norm_a) * std::sqrt(norm_b);
    return denom > 0.0 ? dot / denom : 0.0;
}

class VectorStore {
public:
    VectorStore(std::shared_ptr<EmbeddingClient> embedder)
        : embedder_(std::move(embedder)) {}

    // Add documents and compute their embeddings
    void add_documents(std::vector<Document> docs) {
        // Batch embed all documents at once
        std::vector<std::string> texts;
        for (const auto& doc : docs) {
            texts.push_back(doc.title + ". " + doc.content);
        }

        std::cout << "[vector_store] Embedding " << texts.size() << " documents... " << std::flush;
        auto embeddings = embedder_->embed_batch(texts);
        std::cout << "done (" << (embeddings.empty() ? 0 : embeddings[0].size())
                  << " dimensions)\n";

        for (size_t i = 0; i < docs.size(); ++i) {
            docs[i].embedding = (i < embeddings.size()) ? embeddings[i] : std::vector<float>{};
            docs_.push_back(std::move(docs[i]));
        }
    }

    // Search by cosine similarity
    std::vector<std::pair<Document, double>> search(const std::string& query, int top_k = 3) const {
        auto query_vec = embedder_->embed(query);
        if (query_vec.empty()) return {};

        std::vector<std::pair<double, size_t>> scores;
        for (size_t i = 0; i < docs_.size(); ++i) {
            if (!docs_[i].embedding.empty()) {
                double sim = cosine_similarity(query_vec, docs_[i].embedding);
                scores.push_back({sim, i});
            }
        }

        std::sort(scores.begin(), scores.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

        std::vector<std::pair<Document, double>> results;
        for (int i = 0; i < top_k && i < static_cast<int>(scores.size()); ++i) {
            results.push_back({docs_[scores[i].second], scores[i].first});
        }
        return results;
    }

private:
    std::shared_ptr<EmbeddingClient> embedder_;
    std::vector<Document> docs_;
};

// =========================================================================
// RAG Search Tool — wraps VectorStore as a NeoGraph Tool
// =========================================================================

class RAGSearchTool : public neograph::Tool {
public:
    RAGSearchTool(std::shared_ptr<VectorStore> store) : store_(std::move(store)) {}

    neograph::ChatTool get_definition() const override {
        return {
            "search_documents",
            "Search the knowledge base for relevant documents using semantic similarity. "
            "Use this tool to find information before answering questions. "
            "Returns the most relevant documents with their content, source, and similarity score.",
            json{
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
                {"required", json::array({"query"})}
            }
        };
    }

    std::string execute(const json& args) override {
        auto query = args.value("query", "");
        int top_k = args.value("top_k", 3);

        auto results = store_->search(query, top_k);

        if (results.empty()) {
            return "No relevant documents found for: " + query;
        }

        json output = json::array();
        for (const auto& [doc, score] : results) {
            output.push_back({
                {"id", doc.id},
                {"title", doc.title},
                {"content", doc.content},
                {"source", doc.source},
                {"similarity", std::round(score * 1000.0) / 1000.0}
            });
        }

        return output.dump(2);
    }

    std::string get_name() const override { return "search_documents"; }

private:
    std::shared_ptr<VectorStore> store_;
};

// =========================================================================
// Knowledge Base — NeoGraph documentation
// =========================================================================

static std::vector<Document> create_knowledge_base() {
    return {
        {"doc_1", "NeoGraph Overview",
         "NeoGraph is a C++17 graph-based agent orchestration engine. "
         "It brings LangGraph-level capabilities to C++ with zero Python dependency. "
         "Key features include JSON-defined graphs, parallel fan-out via Taskflow, "
         "checkpointing for time-travel debugging, and HITL support.",
         "README.md", {}},

        {"doc_2", "NeoGraph Architecture",
         "NeoGraph consists of four modules: neograph::core (graph engine), "
         "neograph::llm (LLM providers for OpenAI, Claude, Gemini), "
         "neograph::mcp (MCP client for JSON-RPC tool discovery), and "
         "neograph::util (lock-free RequestQueue). The core module has zero "
         "network dependencies.",
         "docs/architecture.md", {}},

        {"doc_3", "Send and Command",
         "Send enables dynamic fan-out: a node can spawn N parallel tasks at runtime "
         "by returning Send objects. Command enables routing override: a node can "
         "simultaneously update state AND control which node executes next, "
         "bypassing normal edge-based routing. Both are processed by the Taskflow "
         "work-stealing scheduler.",
         "docs/features.md", {}},

        {"doc_4", "Checkpointing and HITL",
         "NeoGraph supports full state snapshots at every super-step via the "
         "CheckpointStore interface. InMemoryCheckpointStore is provided for testing. "
         "Human-in-the-Loop (HITL) is supported through interrupt_before, "
         "interrupt_after, and dynamic NodeInterrupt exceptions. The resume() API "
         "continues execution from the interrupted checkpoint.",
         "docs/features.md", {}},

        {"doc_5", "Performance Comparison",
         "NeoGraph produces a ~5MB static binary vs ~500MB for Python+LangGraph. "
         "Memory usage is ~10MB vs ~300MB. Cold start is instant vs seconds. "
         "Parallel fan-out of 3 workers completing in 150ms tasks takes 151ms "
         "(parallel) vs 370ms (sequential). NeoGraph uses Taskflow work-stealing "
         "for true CPU parallelism, unlike Python's GIL-limited asyncio.",
         "README.md", {}},

        {"doc_6", "LLM Provider Support",
         "NeoGraph supports multiple LLM providers through two mechanisms: "
         "OpenAIProvider for any OpenAI-compatible API (OpenAI, Groq, Together, "
         "vLLM, Ollama), and SchemaProvider which adapts to any LLM API via a "
         "JSON schema. Built-in schemas are provided for OpenAI, Claude, and Gemini. "
         "New providers can be added by creating a JSON schema file.",
         "docs/providers.md", {}},

        {"doc_7", "Cross-thread Store",
         "The Store interface provides namespaced key-value storage that persists "
         "across threads. Use cases include long-term user preferences, shared "
         "knowledge, and agent memory. Namespace is a vector of strings forming a "
         "hierarchical path. InMemoryStore is provided; implement the Store "
         "interface for Redis or a database backend.",
         "docs/features.md", {}},

        {"doc_8", "ReAct Agent Pattern",
         "The ReAct (Reasoning + Acting) pattern alternates between LLM calls and "
         "tool execution. NeoGraph provides create_react_graph() which builds a "
         "standard 2-node graph: llm_call -> tool_dispatch -> loop until done. "
         "The Agent class provides a simpler non-graph alternative. Both support "
         "streaming via callbacks.",
         "docs/patterns.md", {}},
    };
}

// =========================================================================
// Main — CLI RAG Agent with OpenAI Embeddings
// =========================================================================

int main() {
    // 1. Check API key
    const char* api_key = std::getenv("OPENAI_API_KEY");
    if (!api_key) {
        std::cerr << "Set OPENAI_API_KEY environment variable\n";
        return 1;
    }

    // 2. Create embedding client + vector store
    auto embedder = std::make_shared<EmbeddingClient>(api_key);
    auto store = std::make_shared<VectorStore>(embedder);

    // 3. Load and embed documents
    store->add_documents(create_knowledge_base());

    // 4. Create LLM provider
    auto provider = std::shared_ptr<neograph::Provider>(
        neograph::llm::OpenAIProvider::create({
            .api_key = api_key,
            .default_model = "gpt-4o-mini"
        })
    );

    // 5. Create tools and ReAct graph
    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<RAGSearchTool>(store));

    auto engine = neograph::graph::create_react_graph(
        provider,
        std::move(tools),
        "You are a helpful assistant that answers questions about NeoGraph. "
        "ALWAYS use the search_documents tool first to find relevant information "
        "before answering. Base your answers on the retrieved documents. "
        "If the documents don't contain relevant information, say so. "
        "Cite the source document when possible."
    );

    // 6. Interactive CLI loop
    std::cout << "\n=== NeoGraph RAG Agent (text-embedding-3-small) ===\n";
    std::cout << "Ask questions about NeoGraph. Type 'quit' to exit.\n\n";

    std::string input;
    while (true) {
        std::cout << "You: " << std::flush;
        if (!std::getline(std::cin, input)) break;

        if (input.empty()) continue;
        if (input == "quit" || input == "exit" || input == "q") break;

        neograph::graph::RunConfig config;
        config.input = {{"messages", json::array({
            {{"role", "user"}, {"content", input}}
        })}};
        config.max_steps = 10;

        std::cout << "\nAssistant: " << std::flush;

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
