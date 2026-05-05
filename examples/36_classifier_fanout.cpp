// NeoGraph Example 36: Small-model classifier fan-out (edge inference pattern)
//
// Demonstrates the orchestration pattern for parallel small-model
// inference — the "edge story" that motivates NeoGraph's per-iter
// latency tradeoff over LangGraph (130× faster engine overhead, 76×
// lower RSS). NeoGraph itself is not an inference runtime; you bring
// your own (ONNXRuntime, libtorch, ggml, TransformerCPP). This
// example shows the part NeoGraph DOES handle: orchestrating N small
// classifiers concurrently with sub-µs scheduling overhead, so the
// wall-clock cost is `max(per-classifier latency)` not `sum(...)`.
//
// Scenario: a chat moderation pipeline runs five lightweight
// classifiers in parallel on every incoming message:
//
//                              ┌─→ sentiment    ─┐
//                              ├─→ toxicity     ─┤
//   incoming text  →  planner ─┼─→ language     ─┼─→ summarizer  →  verdict
//                              ├─→ topic        ─┤
//                              └─→ intent       ─┘
//
// Each classifier "runs" for ~5 ms (std::this_thread::sleep_for; stand-in
// for a DistilBERT/MiniLM CPU forward pass). The five Sends spawn the
// same `classifier` node type with different payloads (label set + head
// name). The engine's worker pool dispatches them to OS threads, so they
// sleep concurrently and wall ≈ max(per-classifier latency).
//
// THIS EXAMPLE USES NO INFERENCE RUNTIME. The classifier body is a
// blocking sleep with deterministic mock outputs, so it demonstrates
// the orchestration cost in isolation. To swap in real ONNXRuntime —
// about 30 lines, all inside ClassifierNode::execute — see the inline
// comment block below labelled "[ONNX SWAP-IN]".
//
// Why this is the right scope for the example: the inference runtime
// integration is well-trodden (the Ort::Session API is stable across
// versions). The interesting part — and what NeoGraph contributes —
// is the parallel super-step scheduling that makes 5×5ms collapse
// to ~5ms. That's what a mock-latency implementation can show; that's
// what the example shows.
//
// Usage:
//   ./example_classifier_fanout
//   ./example_classifier_fanout "Your message here to classify"

#include <neograph/neograph.h>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

// One classifier node type. The five Send-spawned instances share
// this implementation — only their `head` (sentiment / toxicity / ...)
// and `labels` differ, both injected via Send payload.
//
// [ONNX SWAP-IN] — to drive a real DistilBERT (or whatever) model:
//
//   #include <onnxruntime/onnxruntime_cxx_api.h>
//
//   // member of ClassifierNode (constructed once in main, reused per inference):
//   Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "neograph"};
//   Ort::SessionOptions opts_;
//   Ort::Session session_{env_, "model.onnx", opts_};
//
//   // inside execute(state) — replace the sleep_for with:
//   auto input_ids = tokenize(text);    // your tokenizer (HF tokenizers C++,
//                                       // SentencePiece, etc.)
//   std::vector<int64_t> shape{1, (int64_t)input_ids.size()};
//   auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
//   auto in_tensor = Ort::Value::CreateTensor<int64_t>(
//       mem, input_ids.data(), input_ids.size(), shape.data(), shape.size());
//   const char* in_names[]  = {"input_ids"};
//   const char* out_names[] = {"logits"};
//   auto outputs = session_.Run(Ort::RunOptions{nullptr},
//       in_names, &in_tensor, 1, out_names, 1);
//   float* logits = outputs[0].GetTensorMutableData<float>();
//   int label_idx = std::distance(logits, std::max_element(logits, logits + N_LABELS));
//   std::string label = labels_[label_idx];
//
// Note that Ort::Session::Run is blocking — same as our mock sleep.
// The engine's worker pool dispatches the 5 Sends to 5 OS threads, so
// blocking inference still parallelises across cores. For sub-ms
// dispatch overhead you don't need anything fancier.
class ClassifierNode : public GraphNode {
public:
    std::string get_name() const override { return "classifier"; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto start = std::chrono::steady_clock::now();

        std::string head = in.state.get("head").get<std::string>();
        std::string text = in.state.get("text").get<std::string>();
        auto labels      = in.state.get("labels");

        // Pick a label deterministically from the text's hash. Real
        // inference goes here; see [ONNX SWAP-IN] above.
        std::size_t h = std::hash<std::string>{}(text + head);
        std::string label = labels[h % labels.size()].get<std::string>();

        // Sleep ~5 ms to simulate a CPU-bound DistilBERT forward pass.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();

        json verdict;
        verdict["head"]     = head;
        verdict["label"]    = label;
        verdict["us"]       = elapsed_us;

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"verdicts", json::array({verdict})});
        co_return out;
    }
};

// Planner emits one Send per classifier head, each with a distinct
// (head, labels) payload that the shared ClassifierNode reads from
// state. The text channel is shared across all five (set from input).
class PlannerNode : public GraphNode {
public:
    std::string get_name() const override { return "planner"; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        std::string text = in.state.get("text").get<std::string>();

        struct Head { const char* name; std::vector<std::string> labels; };
        std::vector<Head> heads = {
            {"sentiment", {"positive", "neutral", "negative"}},
            {"toxicity",  {"safe", "borderline", "toxic"}},
            {"language",  {"en", "ko", "ja", "fr", "de"}},
            {"topic",     {"tech", "politics", "sports", "art", "food"}},
            {"intent",    {"inform", "ask", "complain", "praise", "request"}},
        };

        NodeOutput out;
        for (const auto& h : heads) {
            json pl;
            pl["text"]   = text;
            pl["head"]   = h.name;
            pl["labels"] = h.labels;
            out.sends.emplace_back(Send{"classifier", pl});
        }
        co_return out;
    }
};

class SummarizerNode : public GraphNode {
    std::chrono::steady_clock::time_point t_start_;
public:
    explicit SummarizerNode(std::chrono::steady_clock::time_point t)
        : t_start_(t) {}

    std::string get_name() const override { return "summarizer"; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto wall_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t_start_).count();

        auto verdicts = in.state.get("verdicts");

        std::cout << "\n  ── Classifier verdicts ──\n";
        long total_inference_us = 0;
        if (verdicts.is_array()) {
            for (const auto& v : verdicts) {
                std::string head  = v.value("head", std::string{"?"});
                std::string label = v.value("label", std::string{"?"});
                long us           = v.value("us", 0L);
                total_inference_us += us;

                std::cout << "    " << std::left << std::setw(11) << (head + ":")
                          << std::setw(12) << label
                          << "(" << std::fixed << std::setprecision(1)
                          << (us / 1000.0) << " ms)\n";
            }
        }

        std::cout << "\n  wall time:           " << std::fixed
                  << std::setprecision(1) << (wall_us / 1000.0) << " ms\n";
        std::cout << "  sequential cost:     "
                  << (total_inference_us / 1000.0) << " ms"
                  << "  (sum of per-classifier latencies)\n";
        std::cout << "  parallel speedup:    "
                  << std::setprecision(2)
                  << (double(total_inference_us) / std::max(1L, wall_us))
                  << "×\n";

        co_return NodeOutput{};
    }
};

int main(int argc, char** argv) {
    std::string text = "I really enjoyed the new C++ async story";
    if (argc > 1) text = argv[1];

    auto t_start = std::chrono::steady_clock::now();

    auto& factory = NodeFactory::instance();
    factory.register_type("planner",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<PlannerNode>();
        });
    factory.register_type("classifier",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<ClassifierNode>();
        });
    factory.register_type("summarizer",
        [t_start](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<SummarizerNode>(t_start);
        });

    json definition = {
        {"name", "classifier_fanout"},
        {"channels", {
            {"text",     {{"reducer", "overwrite"}}},
            {"head",     {{"reducer", "overwrite"}}},
            {"labels",   {{"reducer", "overwrite"}}},
            {"verdicts", {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"planner",    {{"type", "planner"}}},
            {"classifier", {{"type", "classifier"}}},
            {"summarizer", {{"type", "summarizer"}}}
        }},
        {"edges", json::array({
            json{{"from", "__start__"}, {"to", "planner"}},
            // Send-spawned classifiers run in parallel in the same
            // super-step as the planner→summarizer transition.
            json{{"from", "planner"},    {"to", "summarizer"}},
            json{{"from", "summarizer"}, {"to", "__end__"}}
        })}
    };

    NodeContext ctx;
    auto engine = GraphEngine::compile(definition, ctx);
    // 5 concurrent classifiers — match the worker pool to the fan-out
    // width. Default is hardware_concurrency(); explicit setting makes
    // the demo's wall time stable.
    engine->set_worker_count(5);

    RunConfig cfg;
    cfg.thread_id = "edge-pipeline";
    cfg.input["text"] = text;

    std::cout << "  Classifying: \"" << text << "\"\n";
    auto result = engine->run(cfg);
    return result.interrupted ? 1 : 0;
}
