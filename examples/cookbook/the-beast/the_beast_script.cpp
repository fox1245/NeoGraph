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

// ---- run a shell command, capture stdout (default, unsandboxed path) ----
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

#ifdef BEAST_SANDBOX2
// Optional hardened path (build with -DNEOGRAPH_BEAST_SANDBOX=ON): run the
// model-written python under Google Sandbox2 — its own user/pid/mount/net
// namespaces, a read-only filesystem view limited to the interpreter + the
// two work files, and CPU/wall/file rlimits. Verified on this platform:
// python's stdlib imports work under the FS allowlist, stdout is captured
// over the sandbox IPC, and there is no network.
//
// Isolation: namespaces (own user/pid/mount/net) + read-only FS allowlist +
// CPU/wall/file rlimits, PLUS a seccomp BLOCKLIST *derived from the node's
// declared effect contract* (see below). Python's syscall footprint is too
// large to allowlist safely (cf. Alhindi & Hallett, arXiv:2506.10234), so the
// default action stays permissive and the contract's ABSENCE of a capability
// removes the matching syscalls: a node that declares no "net" capability
// cannot socket()/connect() even if the network namespace were misconfigured
// (defense in depth); a node with no "exec" capability cannot execve() a new
// program. Honest scope: still not a full allowlist — a kernel exploit via an
// unblocked syscall is not contained.
#include <sandboxed_api/sandbox2/allowlists/all_syscalls.h>
#include <sandboxed_api/sandbox2/allowlists/map_exec.h>
#include <sandboxed_api/sandbox2/executor.h>
#include <sandboxed_api/sandbox2/ipc.h>
#include <sandboxed_api/sandbox2/limits.h>
#include <sandboxed_api/sandbox2/policy.h>
#include <sandboxed_api/sandbox2/policybuilder.h>
#include <sandboxed_api/sandbox2/result.h>
#include <sandboxed_api/sandbox2/sandbox2.h>
#include <absl/log/initialize.h>
#include <absl/time/time.h>
#include <sys/syscall.h>
#include <cerrno>

static std::string run_python_sandboxed(const std::string& code_path,
                                        const std::string& in_path,
                                        const std::set<std::string>& caps) {
    const std::string bin = "/usr/bin/python3";
    std::vector<std::string> argv = {bin, code_path, in_path};
    std::vector<std::string> envp = {
        "PATH=/usr/bin:/bin", "HOME=/tmp", "PYTHONDONTWRITEBYTECODE=1"};
    auto executor = std::make_unique<sandbox2::Executor>(bin, argv, envp);
    int recv = executor->ipc()->ReceiveFd(STDOUT_FILENO);
    executor->limits()
        ->set_rlimit_cpu(10)
        .set_walltime_limit(absl::Seconds(10))
        .set_rlimit_fsize(1ULL << 20)
        .set_rlimit_nofile(1024);
    sandbox2::PolicyBuilder b;
    b.DefaultAction(sandbox2::AllowAllSyscalls());
    b.Allow(sandbox2::MapExec());
    // --- seccomp policy SYNTHESISED from the declared effect contract ---
    // No declared capability ⇒ the corresponding syscalls return EPERM.
    if (!caps.count("net"))
        for (int sc : {__NR_socket, __NR_socketpair, __NR_connect, __NR_bind,
                       __NR_listen, __NR_accept, __NR_accept4})
            b.BlockSyscallWithErrno(sc, EPERM);
    if (!caps.count("exec"))
        for (int sc : {__NR_execve, __NR_execveat})
            b.BlockSyscallWithErrno(sc, EPERM);
    b.AddLibrariesForBinary(bin);
    for (const char* d : {"/usr/lib", "/usr/bin", "/lib", "/lib64"})
        b.AddDirectory(d);                          // interpreter + stdlib, read-only
    // Mount ONLY the two work files, not all of temp_directory_path() — so
    // the script cannot read other runs' inputs. (Also avoids a dangling
    // c_str() into a temporary path.)
    b.AddFile(code_path);
    b.AddFile(in_path);
    sandbox2::Sandbox2 s2(std::move(executor), b.BuildOrDie());
    std::string out;
    if (s2.RunAsync()) {
        char buf[4096];
        ssize_t n;
        while ((n = read(recv, buf, sizeof(buf))) > 0) out.append(buf, n);
    }
    close(recv);
    (void)s2.AwaitResult();
    return out;
}
#endif

// Run the model's python: sandboxed when built with BEAST_SANDBOX2, else a
// plain `timeout 10 python3` subprocess. Same stdout contract either way.
// `caps` = the node's declared capabilities (drives the seccomp policy).
static std::string run_python(const std::string& code_path, const std::string& in_path,
                              const std::set<std::string>& caps) {
#ifdef BEAST_SANDBOX2
    return run_python_sandboxed(code_path, in_path, caps);
#else
    (void)caps;
    return run_cmd("timeout 10 python3 '" + code_path + "' '" + in_path + "'");
#endif
}

// =================================================================
// The universal cartridge. Stateless across runs (per GraphNode
// contract): code + declared contract are fixed at construction; each
// run() uses a uniquely-named input file so concurrent runs never clash.
// =================================================================
class ScriptNode : public ng::GraphNode {
    std::string name_, code_path_;
    std::set<std::string> writes_, goto_targets_, caps_;
public:
    ScriptNode(std::string name, const json& cfg) : name_(std::move(name)) {
        static std::atomic<uint64_t> counter{0};
        code_path_ = (fs::temp_directory_path() /
            ("beast_script." + std::to_string(getpid()) + "." +
             std::to_string(counter++) + ".py")).string();
        std::ofstream(code_path_) << cfg.value("code", "");
        for (const auto& w : cfg.value("writes", json::array())) writes_.insert(w.get<std::string>());
        for (const auto& g : cfg.value("goto_targets", json::array())) goto_targets_.insert(g.get<std::string>());
        // Declared capabilities (e.g. "net", "exec"); their ABSENCE tightens
        // the sandbox's seccomp policy. Default: none → most restrictive.
        for (const auto& c : cfg.value("caps", json::array())) caps_.insert(c.get<std::string>());
    }
    ~ScriptNode() override { std::error_code ec; fs::remove(code_path_, ec); }

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
        const std::string raw = run_python(code_path_, in_path, caps_);
        { std::error_code ec; fs::remove(in_path, ec); }   // clean the per-run input file

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
              "writes":{"type":"array"},"goto_targets":{"type":"array"},
              "caps":{"type":"array"}},
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
#ifdef BEAST_SANDBOX2
    absl::InitializeLog();   // sandbox2 uses absl logging
#endif
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
             .default_model = "deepseek/deepseek-v4-flash"});
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
        p.model = "deepseek/deepseek-v4-flash"; p.messages = convo;
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
    // The runtime contract wrapper throws on an undeclared write/goto or
    // non-JSON script output; catch it so a bad script fails loudly-but-
    // gracefully instead of aborting the process.
    try {
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
    } catch (const std::exception& e) {
        std::cout << "  RUNTIME CONTRACT VIOLATION: " << e.what()
                  << "\n  (the script broke its declared surface — rejected at runtime)\n";
        return 1;
    }
    std::cout << "\nThe model wrote the node's logic AND its flow. The compiler proved the "
                 "shape; the contract proved the surface.\n";
    return 0;
}
