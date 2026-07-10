// NeoGraph Cookbook — "The Beast", SCRIPT (the universal cartridge)
// =================================================================
// Every other Beast variant lets the model author *tools* (leaf
// capabilities). This one lets it author NODE LOGIC — including control
// flow (goto / dynamic fan-out) that tools categorically cannot express.
//
// `script_node` is ONE pre-compiled C++ node (the "universal cartridge"):
// its config carries model-written code; at run() it hands the node the
// current channel state and applies whatever the code returns —
// {writes, goto, sends} — to the graph. So the model defines a node's
// behavior AND the graph's flow, in data, with no recompile.
//
// Coherence stays non-negotiable. The script declares its contract in
// config (reads / writes / goto_targets); the harness passes the three
// DSL gates, PLUS a contract check (declared writes must be declared
// channels; goto targets must be real nodes), PLUS a runtime wrapper that
// REJECTS any write or goto outside the declaration. That restores the
// effect/route guarantees (boundaries 2 & 6) at the Beast layer with
// ZERO change to NeoGraph core — additive and backward compatible.
//
//   The compiler proves the graph's shape; the contract proves the
//   node's surface; only the script's inner logic is (unavoidably)
//   unproven — bounded by `timeout` + `max_steps`.
//
// Setup:  OPENROUTER_API_KEY in .env ; python3 + timeout on PATH.
// Build:  cmake --build build --target cookbook_the_beast_script
// Run:    ./build/cookbook_the_beast_script

#include <neograph/neograph.h>
#include <neograph/graph/elaborator.h>
#include <neograph/graph/validator.h>
#include <neograph/graph/loader.h>
#include <neograph/llm/openai_provider.h>

#include <cppdotenv/dotenv.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>

using neograph::json;
namespace ng = neograph::graph;
namespace fs = std::filesystem;

// ---- run a shell command, capture stdout ----
static std::string run_cmd(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) throw std::runtime_error("popen failed: " + cmd);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

// =================================================================
// The universal cartridge. Stateless across runs (per GraphNode
// contract): code + declared contract are fixed at construction; each
// run() uses a uniquely-named input file so concurrent runs never clash.
// =================================================================
class ScriptNode : public ng::GraphNode {
    std::string name_, code_path_;
    std::set<std::string> writes_, goto_targets_;
public:
    ScriptNode(std::string name, const json& cfg) : name_(std::move(name)) {
        static std::atomic<uint64_t> counter{0};
        code_path_ = (fs::temp_directory_path() /
            ("beast_script." + std::to_string(getpid()) + "." +
             std::to_string(counter++) + ".py")).string();
        std::ofstream(code_path_) << cfg.value("code", "");
        for (const auto& w : cfg.value("writes", json::array())) writes_.insert(w.get<std::string>());
        for (const auto& g : cfg.value("goto_targets", json::array())) goto_targets_.insert(g.get<std::string>());
    }

    asio::awaitable<ng::NodeOutput> run(ng::NodeInput in) override {
        // (a) flatten channel state → {name: value}
        json flat = json::object();
        const json ser = in.state.serialize();
        if (ser.contains("channels")) {
            const json chans = ser["channels"];             // bind temporary
            for (auto it = chans.begin(); it != chans.end(); ++it)
                flat[it.key()] = (*it).value("value", json());
        }

        // (b) materialize input, run the model's code under a hard timeout
        static std::atomic<uint64_t> call{0};
        const std::string in_path = code_path_ + "." + std::to_string(call++) + ".in.json";
        std::ofstream(in_path) << json{{"state", flat}}.dump();
        const std::string raw = run_cmd("timeout 10 python3 '" + code_path_ + "' '" + in_path + "'");

        json out;
        try { out = json::parse(raw); }
        catch (const std::exception&) {
            throw std::runtime_error("script_node '" + name_ + "' produced non-JSON: " + raw.substr(0, 200));
        }

        // (c) ENFORCE the declared contract, then map → NodeOutput
        ng::NodeOutput r;
        const json wr = out.value("writes", json::object());   // bind temporary (see contract_check)
        for (auto it = wr.begin(); it != wr.end(); ++it) {
            if (!writes_.empty() && !writes_.count(it.key()))
                throw std::runtime_error("script_node '" + name_ + "' wrote undeclared channel '" + it.key() + "'");
            r.writes.push_back({it.key(), it.value()});
        }
        const std::string go = out.value("goto", "");
        if (!go.empty()) {
            if (!goto_targets_.empty() && !goto_targets_.count(go))
                throw std::runtime_error("script_node '" + name_ + "' jumped to undeclared target '" + go + "'");
            r.command = ng::Command{go, {}};
        }
        for (const auto& s : out.value("sends", json::array()))
            r.sends.push_back({s.value("target_node", ""), s.value("input", json::object())});
        co_return r;
    }
    std::string get_name() const override { return name_; }
};

void register_script_node() {
    static bool once = [] {
        // 3-arg register: config schema, NO type-level effect contract —
        // script effects are per-instance (declared in config), enforced
        // by contract_check() + the runtime wrapper above.
        ng::NodeFactory::instance().register_type(
            "script_node",
            [](const std::string& n, const json& cfg, const ng::NodeContext&) {
                return std::unique_ptr<ng::GraphNode>(new ScriptNode(n, cfg)); },
            json::parse(R"({"type":"object","properties":{
              "code":{"type":"string"},"reads":{"type":"array"},
              "writes":{"type":"array"},"goto_targets":{"type":"array"}},
              "required":["code"]})"));
        return true;
    }();
    (void)once;
}

// ---- three DSL gates ----
struct Verdict { bool ok = false; std::string gate, report; json core; };
Verdict forge_gate(const json& dsl, const ng::NodeContext& ctx) {
    json core;
    try { core = ng::Elaborator::elaborate(dsl).core; }
    catch (const std::exception& e) { return {false, "elaborate", e.what(), {}}; }
    try {
        auto cg = ng::GraphCompiler::compile(core, ctx);
        ng::GraphCompiler::verify_roundtrip(core, cg);
        auto rep = ng::GraphValidator::validate(cg);
        if (rep.has_errors()) return {false, "validate", rep.summary(), {}};
        return {true, "accepted", {}, core};
    } catch (const std::exception& e) { return {false, "compile", e.what(), {}}; }
}

// ---- Beast-layer contract check: restores E4/E3-equivalent guarantees
//      for script_node instances, without touching GraphValidator core.
static std::string contract_check(const json& core) {
    // Bind to locals — `core.value(...)` returns a fresh temporary each
    // call, so begin()/end() on separate calls would be iterators into
    // different objects (never equal → infinite loop with the yyjson type).
    const json chans = core.value("channels", json::object());
    const json node_map = core.value("nodes", json::object());
    std::set<std::string> channels, nodes{"__start__", "__end__"};
    for (auto it = chans.begin(); it != chans.end(); ++it) channels.insert(it.key());
    for (auto it = node_map.begin(); it != node_map.end(); ++it) nodes.insert(it.key());
    std::string err;
    for (auto it = node_map.begin(); it != node_map.end(); ++it) {
        const json& n = *it;
        if (n.value("type", "") != "script_node") continue;
        // Node config is INLINE (siblings of "type"), not nested under a
        // "config" key — that's how GraphCompiler passes node_def to the
        // factory and how strict consumed-key accounting works.
        for (const auto& w : n.value("writes", json::array()))
            if (!channels.count(w.get<std::string>()))
                err += "  node '" + it.key() + "': declared write to undeclared channel '" + w.get<std::string>() + "'\n";
        for (const auto& g : n.value("goto_targets", json::array()))
            if (!nodes.count(g.get<std::string>()))
                err += "  node '" + it.key() + "': goto_target '" + g.get<std::string>() + "' is not a node\n";
    }
    return err;
}

static json extract_json(const std::string& t) {
    auto a = t.find('{'), b = t.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b < a)
        throw std::runtime_error("no JSON object in reply");
    return json::parse(t.substr(a, b - a + 1));
}

// The harness the model is asked to author — also used verbatim by
// --selftest to exercise the mechanism deterministically (no API key).
static json canned_harness() {
    return {
        {"schema_version", 1},
        {"name", "counter_loop"},
        {"channels", {{"counter", {{"reducer", "overwrite"}, {"initial", 0}}}}},
        {"nodes", {{"tick", {
            {"type", "script_node"},
            {"code",
             "import json,sys\n"
             "d=json.load(open(sys.argv[1]))\n"
             "n=d['state'].get('counter',0)\n"
             "nn=n+1\n"
             "print(json.dumps({'writes':{'counter':nn},'goto':'tick' if nn<3 else '__end__'}))\n"},
            {"reads", json::array({"counter"})},
            {"writes", json::array({"counter"})},
            {"goto_targets", json::array({"tick", "__end__"})}
        }}}},
        {"edges", json::array({ {{"from", "__start__"}, {"to", "tick"}} })}
    };
}

int main(int argc, char** argv) {
    const bool selftest = (argc > 1 && std::string(argv[1]) == "--selftest");
    register_script_node();

    std::cout << "============ THE BEAST (script) ============\n"
                 "The model writes NODE LOGIC — including flow control (goto)\n"
                 "— in data. The compiler + contract keep it coherent.\n\n";

    ng::NodeContext ctx;
    std::shared_ptr<neograph::Provider> provider;
    json core;

    if (selftest) {
        // Deterministic path: gate the canned harness, no LLM. Proves the
        // script_node mechanism (subprocess exec + goto flow + contract).
        std::cout << "── --selftest: gating a canned script_node harness (offline) ──\n";
        const json dsl = canned_harness();
        Verdict v = forge_gate(dsl, ctx);
        std::string cc = v.ok ? contract_check(v.core) : std::string{};
        if (!v.ok || !cc.empty()) {
            std::cout << "  REJECTED: " << (v.ok ? cc : v.report) << "\n"; return 1;
        }
        std::cout << "  ACCEPTED — coherent + contract-checked.\n";
        core = v.core;
    }

    cppdotenv::auto_load_dotenv();
    const char* key = std::getenv("OPENROUTER_API_KEY");
    if (!selftest) {
        if (!key || !*key) { std::cerr << "OPENROUTER_API_KEY not set (or use --selftest)\n"; return 2; }
        provider = neograph::llm::OpenAIProvider::create_shared(
            {.api_key = key, .base_url = "https://openrouter.ai/api",
             .default_model = "deepseek/deepseek-v4-pro"});
        ctx.provider = provider;
    }

    const std::string sys =
        "Output ONLY one JSON object (no prose, no fences): a NeoGraph harness.\n"
        "- schema_version: 1\n"
        "- channels: {\"counter\": {\"reducer\": \"overwrite\", \"initial\": 0}}\n"
        "- nodes: {\"tick\": {...}} where tick is type \"script_node\" with these INLINE "
        "fields (siblings of \"type\"; no nested \"config\", no \"id\"): "
        "\"code\" (Python string), \"reads\":[\"counter\"], \"writes\":[\"counter\"], "
        "\"goto_targets\":[\"tick\",\"__end__\"]\n"
        "- edges: [{\"from\":\"__start__\",\"to\":\"tick\"}]\n"
        "The Python in \"code\" reads sys.argv[1] (a JSON file {\"state\":{\"counter\":N}}) "
        "and prints one line: {\"writes\":{\"counter\":N+1},\"goto\":\"tick\" if N+1<3 else "
        "\"__end__\"}. The loop is driven by goto, not static edges.\n"
        "IMPORTANT: write the Python as a SINGLE LINE using semicolons — NO newlines and "
        "NO backslash-n inside the code string (e.g. "
        "\"import json,sys; d=json.load(open(sys.argv[1])); n=d['state'].get('counter',0); ...\").";

    std::vector<neograph::ChatMessage> convo = {{"system", sys}, {"user", "Author the harness JSON."}};

    for (int attempt = 1; attempt <= 3 && core.is_null(); ++attempt) {
        std::cout << "── Attempt #" << attempt << ": model writes node logic ──\n";
        neograph::CompletionParams p;
        p.model = "deepseek/deepseek-v4-pro"; p.messages = convo;
        p.temperature = 0.2f; p.max_tokens = 4000;
        neograph::ChatCompletion resp;
        try { resp = provider->complete(p); }
        catch (const std::exception& e) { std::cerr << "  LLM error: " << e.what() << "\n"; return 1; }

        json dsl;
        try { dsl = extract_json(resp.message.content); }
        catch (const std::exception&) {
            convo.push_back({"user", "Not valid JSON. Output ONLY the JSON harness."});
            std::cout << "  unparseable; retry.\n\n"; continue;
        }

        Verdict v = forge_gate(dsl, ctx);
        std::string cerr_ = v.ok ? contract_check(v.core) : std::string{};
        if (!v.ok || !cerr_.empty()) {
            const std::string gate = v.ok ? "contract" : v.gate;
            const std::string report = v.ok ? cerr_ : v.report;
            std::cout << "  REJECTED at '" << gate << "':\n    " << report.substr(0, 300) << "\n";
            convo.push_back({"assistant", dsl.dump()});
            convo.push_back({"user", "Compiler/contract REJECTED at '" + gate + "':\n" + report +
                "\nFix only what it names. Output ONLY corrected JSON."});
            std::cout << "  → self-repair.\n\n";
            continue;
        }
        std::cout << "  ACCEPTED — coherent, and the script's write/goto surface is "
                     "contract-checked.\n";
        core = v.core;
    }
    if (core.is_null()) { std::cout << "no coherent harness.\n"; return 1; }

    // ---- spawn: the loop is driven by model-written goto logic ----
    std::cout << "\n── Spawning — the node's own code drives the loop via goto ──\n";
    auto engine = ng::GraphEngine::compile(core, ctx);
    ng::RunConfig rc;
    rc.max_steps = 20;
    rc.input = {{"counter", 0}};
    int ticks = 0;
    auto result = engine->run_stream(rc, [&](const ng::GraphEvent& ev) {
        if (ev.type == ng::GraphEvent::Type::NODE_START && ev.node_name == "tick")
            std::cout << "  [tick #" << ++ticks << " — script decides: continue or exit]\n";
    });
    std::cout << "  trace: ";
    for (const auto& n : result.execution_trace) std::cout << n << " -> ";
    std::cout << "END\n";
    json fc = result.has_channel("counter") ? result.channel<json>("counter") : json();
    std::cout << "  final counter = " << fc.dump()
              << "  (the model's goto logic ran the loop, contract-enforced)\n";
    std::cout << "\nThe model wrote the node's logic AND its flow. The compiler proved the "
                 "shape; the contract proved the surface.\n";
    return 0;
}
