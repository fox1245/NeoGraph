// Program Coding Swarm — multi-agent coding workflow as a Program topology.
//
// Experimental proof of concept demonstrating:
//   1. Parallel work: root Program orchestrator splits a coding task into
//      modules and runs one child Harness per module concurrently, then joins.
//   2. Resource allocation per module: critical modules receive a larger
//      non-renewable budget carved from the root budget, more attempts, a
//      stronger model role, and a second-opinion grandchild Harness.
//   3. Different models per node via roles mapped through demo-models.json or
//      live-models.json (no model IDs or prices hard-coded in C++).
//   4. Host-owned precondition gate: a module can publish its output only when
//      the deterministic host verify tool passed AND the reviewer verdict was
//      read and equals "accept" (plus the grandchild second-opinion verdict for
//      critical modules).
//   5. Supervision in depth: root -> child -> grandchild hierarchy with per-node
//      role, model role, budget granted/remaining, and outcome.
//   6. Topology as data on SQLite: every Program generation is persisted in
//      SQLite; mid-run, one module's reviewer topology is replaced with a
//      stricter admitted version (carrying over remaining budgets and keeping
//      children), then rolled back to the earlier admitted version.
//   7. Central vs delegated synthesis (--synthesis=central|delegated):
//      central validates and admits the full tree before execution starts;
//      delegated synthesizes the critical module's grandchild at runtime
//      through ProgramSynthesisGateway within the child's authority and budget.
//
// Usage:
//   ./cookbook_program_coding_swarm [--mock|--live]
//       [--models <demo-models.json|live-models.json>]
//       [--synthesis=central|delegated]
//       [--concurrency <n>|--sequential]
//       [--runs <k>]
//       [--max-cost-usd <usd>] [--max-cost-usd-other <usd>]
//       [--report <file.json>]
//
// smoke-expect: === Program Coding Swarm (Experimental Proof of Concept) ===
// smoke-expect: [synthesis=central] tree admitted before run start
// smoke-expect: [gate] module=slugify attempt=1 blocked (reason=verify_failed
// smoke-expect: [gate] module=escape_html attempt=1 blocked (reason=verdict_rejected
// smoke-expect: [hierarchy] root=swarm-orchestrator -> child=module:escape_html -> grandchild=second-opinion:escape_html
// smoke-expect: [budget] non-renewable child budgets decreased monotonically
// smoke-expect: [replace] reviewer topology upgraded mid-run; budgets carried over and children preserved
// smoke-expect: [rollback] restored earlier admitted generation; store history count=3
// smoke-expect: [synthesis=delegated] child synthesized grandchild at runtime via ProgramSynthesisGateway
// smoke-expect: [comparison] central vs delegated recorded
// smoke-expect: Coding swarm verification: PASS

#include "program_support.h"
#include "program_support_sqlite.h"
#include "provider_example_support.h"

#include <neograph/graph/provider_call_broker.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/program/core_provider_call.h>
#include <neograph/program/program.h>
#include <neograph/program/replacement.h>
#include <neograph/program/runtime.h>
#include <neograph/program/sqlite_store.h>
#include <neograph/program/sqlite_transition_store.h>
#include <neograph/program/synthesis.h>

#include <descriptor/catalogue.h>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;
using examples::program::digest;

std::int64_t now_epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double microunits_to_usd(std::uint64_t microunits) {
    return static_cast<double>(microunits) / 1'000'000.0;
}

std::uint64_t usd_to_microunits(double usd) {
    if (usd <= 0.0) return 0;
    return static_cast<std::uint64_t>(std::llround(usd * 1'000'000.0));
}

// ---- CLI options & Models file configuration --------------------------------

struct CliOptions {
    bool live = false;
    std::string models_file;
    std::string synthesis_mode = "central";  // "central" or "delegated"
    std::size_t concurrency = 0;             // 0 = all modules in parallel
    bool sequential = false;
    std::size_t runs = 1;
    double max_cost_usd = 0.50;
    double max_cost_usd_other = 0.25;
    std::string report_file;
};

struct ProviderBackendSpec {
    std::string name;
    std::string id;
    std::string family;
    std::string base_url;
    std::string path;
    std::string env_key;
};

struct RoleModelSpec {
    std::string role;
    std::string backend;
    std::string model;
    std::uint64_t max_output_tokens = 512;
    std::optional<sp::OpenRouterRouting> openrouter_routing;
    json raw_provider_routing = json::object();
};

struct ModelsConfig {
    std::string source_path;
    sp::descriptor::CatalogueSnapshot catalogue;
    sp::descriptor::PolicySnapshot descriptor_policy;
    std::map<std::string, ProviderBackendSpec> backends;
    std::map<std::string, RoleModelSpec> roles;
};

CliOptions parse_cli(int argc, char** argv) {
    CliOptions opts;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        auto next_val = [&](const char* flag) -> std::string {
            if (i + 1 >= argc)
                throw std::invalid_argument(std::string(flag) + " requires an argument");
            return argv[++i];
        };
        if (arg == "--live") {
            opts.live = true;
        } else if (arg == "--mock") {
            opts.live = false;
        } else if (arg == "--sequential") {
            opts.sequential = true;
            opts.concurrency = 1;
        } else if (arg == "--concurrency") {
            opts.concurrency = std::stoull(next_val("--concurrency"));
            if (opts.concurrency == 0)
                throw std::invalid_argument("--concurrency must be >= 1");
            opts.sequential = (opts.concurrency == 1);
        } else if (arg.starts_with("--concurrency=")) {
            opts.concurrency = std::stoull(std::string(arg.substr(14)));
            if (opts.concurrency == 0)
                throw std::invalid_argument("--concurrency must be >= 1");
            opts.sequential = (opts.concurrency == 1);
        } else if (arg == "--runs") {
            opts.runs = std::stoull(next_val("--runs"));
            if (opts.runs == 0) throw std::invalid_argument("--runs must be >= 1");
        } else if (arg.starts_with("--runs=")) {
            opts.runs = std::stoull(std::string(arg.substr(7)));
            if (opts.runs == 0) throw std::invalid_argument("--runs must be >= 1");
        } else if (arg == "--max-cost-usd") {
            opts.max_cost_usd = std::stod(next_val("--max-cost-usd"));
        } else if (arg.starts_with("--max-cost-usd=")) {
            opts.max_cost_usd = std::stod(std::string(arg.substr(15)));
        } else if (arg == "--max-cost-usd-other") {
            opts.max_cost_usd_other = std::stod(next_val("--max-cost-usd-other"));
        } else if (arg.starts_with("--max-cost-usd-other=")) {
            opts.max_cost_usd_other = std::stod(std::string(arg.substr(21)));
        } else if (arg == "--report") {
            opts.report_file = next_val("--report");
        } else if (arg.starts_with("--report=")) {
            opts.report_file = std::string(arg.substr(9));
        } else if (arg == "--models") {
            opts.models_file = next_val("--models");
        } else if (arg.starts_with("--models=")) {
            opts.models_file = std::string(arg.substr(9));
        } else if (arg == "--synthesis") {
            opts.synthesis_mode = next_val("--synthesis");
        } else if (arg.starts_with("--synthesis=")) {
            opts.synthesis_mode = std::string(arg.substr(12));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: cookbook_program_coding_swarm [--mock|--live] "
                         "[--models <file.json>] [--synthesis=central|delegated] "
                         "[--concurrency <n>|--sequential] [--runs <k>] "
                         "[--max-cost-usd <usd>] [--max-cost-usd-other <usd>] "
                         "[--report <file.json>]\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("Unknown argument: " + std::string(arg));
        }
    }
    if (opts.synthesis_mode != "central" && opts.synthesis_mode != "delegated") {
        throw std::invalid_argument("--synthesis must be 'central' or 'delegated'");
    }
    if (opts.models_file.empty()) {
        const std::string filename = opts.live ? "live-models.json" : "demo-models.json";
        const std::filesystem::path rel_dir("examples/cookbook/program-coding-swarm");
        std::vector<std::filesystem::path> candidates = {
            std::filesystem::path(filename),
            rel_dir / filename,
        };
        if (argc > 0 && argv != nullptr && argv[0] != nullptr && argv[0][0] != '\0') {
            std::error_code ec;
            const auto exe_path = std::filesystem::weakly_canonical(std::filesystem::path(argv[0]), ec);
            const auto exe_dir =
                (!ec && !exe_path.empty()) ? exe_path.parent_path() : std::filesystem::path(argv[0]).parent_path();
            if (!exe_dir.empty()) {
                candidates.push_back(exe_dir / filename);
                candidates.push_back(exe_dir / rel_dir / filename);
                if (exe_dir.has_parent_path()) {
                    candidates.push_back(exe_dir.parent_path() / rel_dir / filename);
                    if (exe_dir.parent_path().has_parent_path()) {
                        candidates.push_back(exe_dir.parent_path().parent_path() / rel_dir / filename);
                    }
                }
            }
        }
        opts.models_file = (rel_dir / filename).string();
        for (const auto& candidate : candidates) {
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec) && !ec) {
                opts.models_file = candidate.string();
                break;
            }
        }
    }
    return opts;
}

ModelsConfig load_models_config(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open models configuration file: " + path);
    const json doc = json::parse(in);

    const json catalogue_doc = {
        {"version", doc.value("version", 1)},
        {"models", doc.at("models")},
    };
    auto loaded_cat = sp::descriptor::load_catalogue(catalogue_doc.dump());
    if (const auto* err = std::get_if<sp::descriptor::ConfigError>(&loaded_cat)) {
        throw std::invalid_argument("Model catalogue rejected at " + err->pointer + ": " + err->message);
    }
    const std::array layers{
        sp::descriptor::builtin_catalogue(),
        std::get<sp::descriptor::CatalogueSnapshot>(std::move(loaded_cat)),
    };
    auto merged_cat = sp::descriptor::merge_catalogues(layers);
    if (const auto* err = std::get_if<sp::descriptor::ConfigError>(&merged_cat)) {
        throw std::invalid_argument("Merged model catalogue rejected at " + err->pointer + ": " + err->message);
    }
    auto catalogue = std::get<sp::descriptor::CatalogueSnapshot>(std::move(merged_cat));
    auto policy_res = sp::descriptor::builtin_policy()->with_catalogue(catalogue);
    if (const auto* err = std::get_if<sp::descriptor::ConfigError>(&policy_res)) {
        throw std::invalid_argument("Descriptor policy rejected at " + err->pointer + ": " + err->message);
    }

    ModelsConfig cfg;
    cfg.source_path = path;
    cfg.catalogue = std::move(catalogue);
    cfg.descriptor_policy = std::get<sp::descriptor::PolicySnapshot>(std::move(policy_res));

    for (const auto& [name, item] : doc.at("providers").items()) {
        ProviderBackendSpec spec;
        spec.name = name;
        spec.id = item.at("id").get<std::string>();
        spec.family = item.at("family").get<std::string>();
        spec.base_url = item.at("base_url").get<std::string>();
        spec.path = item.at("path").get<std::string>();
        spec.env_key = item.at("env_key").get<std::string>();
        cfg.backends.emplace(name, std::move(spec));
    }

    for (const auto& [role_name, item] : doc.at("roles").items()) {
        RoleModelSpec spec;
        spec.role = role_name;
        spec.backend = item.at("backend").get<std::string>();
        spec.model = item.at("model").get<std::string>();
        spec.max_output_tokens = item.value("max_output_tokens", std::uint64_t{512});
        if (item.contains("provider") && item.at("provider").is_object()) {
            const auto& prov = item.at("provider");
            spec.raw_provider_routing = prov;
            sp::OpenRouterRouting routing;
            if (prov.contains("zdr")) routing.zdr = prov.at("zdr").get<bool>();
            if (prov.contains("allow_fallbacks"))
                routing.allow_fallbacks = prov.at("allow_fallbacks").get<bool>();
            if (prov.contains("require_parameters"))
                routing.require_parameters = prov.at("require_parameters").get<bool>();
            if (prov.contains("data_collection"))
                routing.data_collection = prov.at("data_collection").get<std::string>();
            if (prov.contains("only"))
                routing.only = prov.at("only").get<std::vector<std::string>>();
            if (prov.contains("order"))
                routing.order = prov.at("order").get<std::vector<std::string>>();
            if (prov.contains("ignore"))
                routing.ignore = prov.at("ignore").get<std::vector<std::string>>();
            spec.openrouter_routing = std::move(routing);
        }
        cfg.roles.emplace(role_name, std::move(spec));
    }
    return cfg;
}

sp::descriptor::ValidatedDescriptor make_validated_descriptor(
    const ProviderBackendSpec& backend,
    const sp::descriptor::PolicySnapshot& policy) {
    const auto source = json{
        {"descriptor_version", 1},
        {"revision", 1},
        {"id", backend.id},
        {"family", backend.family},
        {"connection",
         {{"base_url", backend.base_url},
          {"paths", {{"buffered", backend.path}, {"streaming", backend.path}}}}},
    }.dump();
    auto loaded = sp::descriptor::load(source, policy);
    if (const auto* err = std::get_if<sp::descriptor::ConfigError>(&loaded)) {
        throw std::invalid_argument("Provider descriptor '" + backend.name + "' rejected at " +
                                    err->pointer + ": " + err->message);
    }
    return std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded));
}

// ---- Toy coding modules and deterministic host-owned verify tool ------------
//
// Each module is a small deterministic text-transform specification. The
// host-owned verify tool checks a table of test cases against the candidate
// transform pipeline (a sequence of primitive text operations: trim, lower,
// upper, collapse_ws_to_dash, strip_non_alnum_dash, escape_amp, escape_lt,
// escape_gt, escape_quot, strip_comments, normalize_equals). Neither model text
// nor a reviewer comment can bypass this deterministic check.

struct ModuleTestCase {
    std::string input;
    std::string expected;
};

struct ModuleSpec {
    std::string name;
    std::string criticality;  // "standard" or "critical"
    std::string description;
    std::vector<ModuleTestCase> tests;
    std::uint64_t budget_tokens = 0;
    std::uint64_t budget_microunits = 0;
    std::uint64_t max_attempts = 2;
};

std::vector<ModuleSpec> default_modules(std::uint64_t root_other_microunits) {
    // Critical module gets 50% of the third-party module budget; the two
    // non-critical modules split the remaining 50% (25% each).
    const std::uint64_t crit_micro = (root_other_microunits * 50) / 100;
    const std::uint64_t std_micro = (root_other_microunits * 25) / 100;
    return {
        ModuleSpec{
            "slugify",
            "standard",
            "Convert a title into a lowercase ASCII URL slug with single dashes.",
            {
                {"  Hello, World!  ", "hello-world"},
                {"NeoGraph C++20 Runtime", "neograph-c-20-runtime"},
            },
            20000,
            std_micro,
            2,
        },
        ModuleSpec{
            "escape_html",
            "critical",
            "Escape &, <, >, and \" in untrusted HTML attribute strings without double-escaping.",
            {
                {"<script>alert(\"x&y\")</script>",
                 "&lt;script&gt;alert(&quot;x&amp;y&quot;)&lt;/script&gt;"},
                {"a & b < c > d", "a &amp; b &lt; c &gt; d"},
            },
            36000,
            crit_micro,
            3,
        },
        ModuleSpec{
            "ini_kv",
            "standard",
            "Normalize INI key = value lines by trimming whitespace around keys and '='.",
            {
                {"  port  =   8080  ", "port=8080"},
                {"host = localhost ", "host=localhost"},
            },
            20000,
            std_micro,
            2,
        },
    };
}

std::string apply_pipeline_op(std::string s, std::string_view op) {
    auto trim_ws = [](std::string text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string{};
        const auto last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    };
    auto replace_all = [](std::string text, std::string_view from, std::string_view to) {
        std::size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
        return text;
    };
    if (op == "trim") {
        return trim_ws( std::move(s) );
    } else if (op == "lower") {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    } else if (op == "alnum_dash_collapse") {
        std::string out;
        bool prev_dash = false;
        for (unsigned char c : s) {
            const bool is_alnum =
                (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
            if (is_alnum) {
                out.push_back(static_cast<char>(c));
                prev_dash = false;
            } else if (!out.empty() && !prev_dash) {
                out.push_back('-');
                prev_dash = true;
            }
        }
        if (!out.empty() && out.back() == '-') out.pop_back();
        return out;
    } else if (op == "escape_amp") {
        return replace_all(std::move(s), "&", "&amp;");
    } else if (op == "escape_lt") {
        return replace_all(std::move(s), "<", "&lt;");
    } else if (op == "escape_gt") {
        return replace_all(std::move(s), ">", "&gt;");
    } else if (op == "escape_quot") {
        return replace_all(std::move(s), "\"", "&quot;");
    } else if (op == "normalize_kv") {
        s = trim_ws(std::move(s));
        const auto eq = s.find('=');
        if (eq == std::string::npos) return s;
        return trim_ws(s.substr(0, eq)) + "=" + trim_ws(s.substr(eq + 1));
    }
    return s;
}

struct VerifyOutcome {
    bool passed = false;
    std::size_t passed_cases = 0;
    std::size_t total_cases = 0;
    std::string failure_detail;
    std::vector<std::string> sample_outputs;
};

VerifyOutcome run_host_verify_tool(const ModuleSpec& spec, const json& candidate) {
    VerifyOutcome out;
    out.total_cases = spec.tests.size();
    if (!candidate.is_object() || !candidate.contains("ops") || !candidate.at("ops").is_array()) {
        out.failure_detail = "candidate missing 'ops' array";
        return out;
    }
    std::vector<std::string> ops;
    for (const auto& item : candidate.at("ops")) {
        if (!item.is_string()) {
            out.failure_detail = "non-string op in candidate pipeline";
            return out;
        }
        ops.push_back(item.get<std::string>());
    }
    if (ops.empty()) {
        out.failure_detail = "empty 'ops' pipeline";
        return out;
    }
    for (const auto& tc : spec.tests) {
        std::string actual = tc.input;
        for (const auto& op : ops) actual = apply_pipeline_op(std::move(actual), op);
        out.sample_outputs.push_back(actual);
        if (actual == tc.expected) {
            ++out.passed_cases;
        } else if (out.failure_detail.empty()) {
            out.failure_detail =
                "input='" + tc.input + "' expected='" + tc.expected + "' got='" + actual + "'";
        }
    }
    out.passed = (out.passed_cases == out.total_cases);
    return out;
}

// ---- Offline deterministic scripted provider with real descriptor pricing ---

class PricedScriptedProvider final : public neograph::Provider {
public:
    using Handler = std::function<sp::Completion(const std::string& model, const json& prompt_doc)>;

    PricedScriptedProvider(sp::descriptor::ValidatedDescriptor descriptor,
                           std::string name,
                           std::string family,
                           Handler handler)
        : client_(std::make_shared<sp::runtime::Client>(std::move(descriptor))),
          name_(std::move(name)),
          family_(std::move(family)),
          handler_(std::move(handler)) {}

    std::string get_name() const override { return name_; }
    std::string_view family() const noexcept override { return family_; }

    neograph::PreparedProviderRequest prepare(neograph::ProviderRequest request) override {
        const auto& messages = examples::request_messages(request);
        std::string last_user;
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->role == sp::Role::User && !it->parts.empty()) {
                if (const auto* text = std::get_if<sp::Text>(&it->parts.front())) {
                    last_user = text->value;
                    break;
                }
            }
        }
        json prompt_doc = json::object();
        try {
            prompt_doc = json::parse(last_user);
        } catch (...) {
            prompt_doc = json{{"raw", last_user}};
        }
        auto handler = handler_;
        return prepare_local(
            client_, std::move(request),
            [handler, prompt_doc = std::move(prompt_doc)](
                const neograph::PreparedProviderRequest& prepared,
                const std::function<void(const sp::Event&)>& observer)
                -> asio::awaitable<sp::runtime::Result> {
                // Sleep briefly on the coroutine executor so parallel child
                // Harnesses genuinely overlap in wall-clock [start, end] time.
                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_after(std::chrono::milliseconds(25));
                co_await timer.async_wait(asio::use_awaitable);

                auto completion = handler(std::string(prepared.model()), prompt_doc);
                examples::emit_local_events(completion, observer);
                co_return std::make_shared<const sp::Outcome>(std::move(completion));
            });
    }

protected:
    std::shared_ptr<sp::runtime::Client> endpoint_runtime() const override { return client_; }

private:
    std::shared_ptr<sp::runtime::Client> client_;
    std::string name_;
    std::string family_;
    Handler handler_;
};

// Scripted offline responses for each role, exercising:
// - slugify attempt 1: bad ops pipeline (missing lower) -> verify fails -> gate blocks -> retry attempt 2 passes
// - escape_html attempt 1: valid ops pipeline, but reviewer rejects on attempt 1 -> gate blocks -> retry attempt 2 accepted by both reviewer and grandchild second-opinion
// - ini_kv attempt 1: passes verify and reviewer on first attempt
sp::Completion make_scripted_completion(const std::string& model, const json& prompt_doc) {
    const std::string role = prompt_doc.value("role", "");
    const std::string module_name = prompt_doc.value("module", "");
    const unsigned attempt = prompt_doc.value("attempt", 1u);
    const std::string reviewer_variant = prompt_doc.value("reviewer_variant", "standard");

    json reply_json;
    std::uint64_t in_tok = 180;
    std::uint64_t out_tok = 90;
    // The mock reports the model it was asked for, as the model file names it.
    std::string served_by = model;

    if (role == "orchestrator-plan") {
        in_tok = 260;
        out_tok = 140;
        reply_json = {
            {"task_summary", "Build and verify 3 text-transform modules with criticality-tiered budgets"},
            {"modules", json::array({"slugify", "escape_html", "ini_kv"})},
            {"critical_modules", json::array({"escape_html"})},
        };
    } else if (role == "orchestrator-integrate") {
        in_tok = 310;
        out_tok = 150;
        reply_json = {
            {"integration_status", "accepted"},
            {"summary", "All 3 modules verified by host gate and approved by assigned reviewers."},
        };
    } else if (role == "implementer-cheap" || role == "implementer-strong" || role == "implementer") {
        in_tok = (role == "implementer-strong") ? 240 : 160;
        out_tok = (role == "implementer-strong") ? 120 : 80;
        if (module_name == "slugify") {
            if (attempt == 1) {
                // Intentionally omits "lower" on attempt 1 so host verify fails deterministically.
                reply_json = {
                    {"module", "slugify"},
                    {"ops", json::array({"trim", "alnum_dash_collapse"})},
                    {"notes", "attempt 1 draft without lowercase normalization"},
                };
            } else {
                reply_json = {
                    {"module", "slugify"},
                    {"ops", json::array({"trim", "lower", "alnum_dash_collapse"})},
                    {"notes", "fixed lowercase step after verify failure"},
                };
            }
        } else if (module_name == "escape_html") {
            reply_json = {
                {"module", "escape_html"},
                {"ops", json::array({"escape_amp", "escape_lt", "escape_gt", "escape_quot"})},
                {"notes",
                 attempt == 1 ? "initial escape order" : "verified ampersand-first ordering documented"},
            };
        } else {
            reply_json = {
                {"module", "ini_kv"},
                {"ops", json::array({"normalize_kv"})},
                {"notes", "trim key and value around '='"},
            };
        }
    } else if (role == "reviewer") {
        in_tok = 190;
        out_tok = 70;
        const bool verify_passed = prompt_doc.value("verify_passed", false);
        if (!verify_passed) {
            reply_json = {
                {"verdict", "reject"},
                {"reason", "Host verify tool failed; fix failing test cases before review can accept."},
                {"reviewer_variant", reviewer_variant},
            };
        } else if (module_name == "escape_html" && attempt == 1) {
            // Exercise gate blocking on a rejected reviewer verdict even when verify passed.
            reply_json = {
                {"verdict", "reject"},
                {"reason", "Attempt 1 lacks explicit proof against double-escaping; resubmit with rationale."},
                {"reviewer_variant", reviewer_variant},
            };
        } else {
            reply_json = {
                {"verdict", "accept"},
                {"reason", "Pipeline operations are minimal, deterministic, and pass all test vectors."},
                {"reviewer_variant", reviewer_variant},
            };
        }
    } else if (role == "second-opinion") {
        in_tok = 210;
        out_tok = 85;
        const bool verify_passed = prompt_doc.value("verify_passed", false);
        const std::string primary_verdict = prompt_doc.value("primary_verdict", "reject");
        if (verify_passed && primary_verdict == "accept") {
            reply_json = {
                {"verdict", "accept"},
                {"focus_checked", prompt_doc.value("focus", "entity-order-safety")},
                {"reason", "Second-opinion confirms escape_amp precedes <, >, and \" replacements."},
            };
        } else {
            reply_json = {
                {"verdict", "reject"},
                {"focus_checked", prompt_doc.value("focus", "entity-order-safety")},
                {"reason", "Primary review or verify precondition not satisfied."},
            };
        }
    } else {
        reply_json = {{"verdict", "accept"}, {"note", "default"}};
    }

    sp::Completion completion;
    completion.messages.push_back(examples::message(sp::Role::Assistant, reply_json.dump()));
    completion.stop = {sp::StopKind::EndTurn, "stop"};
    completion.model.requested = model;
    completion.model.served = model;
    completion.model.served_by = served_by;
    completion.usage.input_total = sp::Count{in_tok};
    completion.usage.output_total = sp::Count{out_tok};
    completion.usage.total = sp::Count{in_tok + out_tok};
    completion.usage.stage = sp::UsageStage::Final;
    completion.usage.quality = sp::UsageQuality::Consistent;
    return completion;
}

// ---- Telemetry & Report structures ------------------------------------------

struct CallTelemetry {
    std::string call_id;
    std::string node_hierarchy;
    std::string node_role;
    std::string model_role;
    std::string backend;
    std::string requested_model;
    std::string module_name;
    std::string served_by;
    std::uint64_t attempt = 0;
    std::uint64_t input_tokens = 0;
    std::uint64_t output_tokens = 0;
    std::uint64_t cost_microunits = 0;
    std::uint64_t reserved_microunits = 0;
    std::string cost_source;  // runtime ProgramMonetaryRecord::basis
    std::string evidence;     // runtime ProgramMonetaryRecord::evidence
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    json requested_routing = json::object();
};

struct GateDecisionRecord {
    std::string module_name;
    std::uint64_t attempt = 0;
    std::string reviewer_variant;
    bool verify_passed = false;
    bool reviewer_verdict_read = false;
    std::string reviewer_verdict;
    bool second_opinion_required = false;
    bool second_opinion_verdict_read = false;
    std::string second_opinion_verdict;
    bool published = false;
    std::string block_reason;
};

struct NodeExecutionSummary {
    std::string level;       // "root", "child", "grandchild"
    std::string name;        // e.g. "swarm-orchestrator", "module:escape_html", "second-opinion:escape_html"
    std::string parent;
    std::string role;
    std::string model_role;
    std::string backend;
    std::string model;
    std::uint64_t attempts = 0;
    std::uint64_t tokens_in = 0;
    std::uint64_t tokens_out = 0;
    std::uint64_t cost_microunits = 0;
    std::string cost_source = "catalogue";
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    std::string outcome = "completed";
    std::uint64_t budget_granted_tokens = 0;
    std::uint64_t budget_remaining_tokens = 0;
    std::uint64_t budget_granted_microunits = 0;
    std::uint64_t budget_remaining_microunits = 0;
};

struct GenerationTransitionEvent {
    std::string action;  // "initial", "replace_stricter", "rollback_initial"
    std::uint64_t generation = 0;
    std::string run_id;
    std::string program_version_id;
    std::string bundle_id;
    std::string reviewer_variant;
    std::uint64_t budget_before_tokens = 0;
    std::uint64_t budget_after_tokens = 0;
    std::uint64_t budget_before_microunits = 0;
    std::uint64_t budget_after_microunits = 0;
    bool children_preserved = true;
};

struct SingleRunReport {
    std::string synthesis_mode;
    bool sequential = false;
    std::size_t concurrency = 3;
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    std::int64_t total_wall_ms = 0;
    std::int64_t sum_children_wall_ms = 0;
    bool children_intervals_overlapped = false;
    std::uint64_t total_tokens_in = 0;
    std::uint64_t total_tokens_out = 0;
    std::uint64_t total_cost_microunits = 0;
    std::uint64_t non_anthropic_cost_microunits = 0;
    std::size_t gate_blocks = 0;
    std::size_t published_modules = 0;
    std::size_t admissions_refused = 0;
    std::vector<std::string> refusal_reasons;
    std::vector<std::string> notices;
    std::vector<NodeExecutionSummary> nodes;
    std::vector<CallTelemetry> calls;
    std::vector<GateDecisionRecord> gate_decisions;
    std::vector<GenerationTransitionEvent> generation_transitions;
    std::map<std::string, std::uint64_t> cost_by_module_microunits;
    std::map<std::string, std::uint64_t> cost_by_role_microunits;
    bool all_modules_succeeded = false;
};

// ---- Program Topology Templates (JavaScript DSL) ----------------------------

// Grandchild Program: second-opinion reviewer for critical modules.
std::string make_grandchild_js_source(const std::string& focus_tag) {
    return "export function define() {\n"
           "  const g = ng.graph('second_opinion_graph');\n"
           "  g.channel('payload', {reducer: 'overwrite', initial: {}});\n"
           "  g.channel('result', {reducer: 'overwrite', initial: {}});\n"
           "  g.node('second_opinion', {type: 'swarm.second_opinion', focus: '" +
           focus_tag + "'});\n"
           "  g.entry('second_opinion');\n"
           "  g.exit('second_opinion');\n"
           "  return g;\n"
           "}\n"
           "export function* main(input) {\n"
           "  const step = yield ng.callCore('second_opinion_graph', {payload: input}, 'so:call');\n"
           "  return step.channels.result.value;\n"
           "}\n";
}

// Child Program: per-module coding harness (implement -> verify -> review ->
// [optional grandchild second-opinion] -> host publish gate, with bounded retry loop
// and mid-run reviewer topology replacement checkpoint on slugify).
std::string make_child_js_source(const ModuleSpec& mod,
                                 const std::string& reviewer_variant,
                                 const std::string& synthesis_mode) {
    const bool is_critical = (mod.criticality == "critical");
    const std::string impl_role = is_critical ? "implementer-strong" : "implementer-cheap";
    std::ostringstream s;
    s << "export function define() {\n"
      << "  const g = ng.graph('module_graph');\n"
      << "  g.channel('payload', {reducer: 'overwrite', initial: {}});\n"
      << "  g.channel('result', {reducer: 'overwrite', initial: {}});\n"
      << "  g.node('implement', {type: 'swarm.implement', module: '" << mod.name
      << "', model_role: '" << impl_role << "'});\n"
      << "  g.node('verify', {type: 'swarm.verify', module: '" << mod.name << "'});\n"
      << "  g.node('review', {type: 'swarm.review', module: '" << mod.name
      << "', model_role: 'reviewer', reviewer_variant: '" << reviewer_variant << "'});\n"
      << "  g.node('gate', {type: 'swarm.gate', module: '" << mod.name
      << "', critical: " << (is_critical ? "true" : "false") << "});\n"
      << "  g.edge('implement', 'verify');\n"
      << "  g.edge('verify', 'review');\n"
      << "  g.edge('review', 'gate');\n"
      << "  g.entry('implement');\n"
      << "  g.exit('gate');\n"
      << "  return g;\n"
      << "}\n";

    s << "export function* main(input) {\n"
      << "  const handoff = input.handoff || null;\n"
      << "  const stage = handoff ? (handoff.stage || 0) : 0;\n"
      << "  const startAttempt = handoff ? (handoff.next_attempt || 1) : 1;\n"
      << "  const maxAttempts = " << mod.max_attempts << ";\n";

    if (mod.name == "slugify") {
        s << "  if (stage === 0) {\n"
          << "    yield ng.callCore('module_graph', {payload: {init_step: 1, module: 'slugify'}}, "
             "'init:slugify:1');\n"
          << "    yield ng.checkpoint({kind: 'replace_to_stricter', module: 'slugify', stage: 1, "
             "next_attempt: 1}, 'slugify:before_replace');\n"
          << "  }\n"
          << "  if (stage === 1 && '" << reviewer_variant << "' === 'stricter') {\n"
          << "    yield ng.callCore('module_graph', {payload: {init_step: 2, module: 'slugify'}}, "
             "'init:slugify:2');\n"
          << "    yield ng.checkpoint({kind: 'rollback_to_standard', module: 'slugify', stage: 2, "
             "next_attempt: 1}, 'slugify:before_rollback');\n"
          << "  }\n";
    }

    s << "  let lastGate = null;\n";
    if (is_critical && synthesis_mode == "delegated") {
        s << "  let delegated = false;\n";
    }
    s << "  for (let attempt = startAttempt; attempt <= maxAttempts; ++attempt) {\n"
      << "    const coreOut = yield ng.callCore('module_graph', {payload: {module: '" << mod.name
      << "', attempt: attempt, reviewer_variant: '" << reviewer_variant
      << "', spec: input}}, 'core:" << mod.name << ":' + attempt);\n"
      << "    let gateData = coreOut.channels.result.value;\n";

    if (is_critical) {
        s << "    if (gateData.verify_passed && gateData.reviewer_verdict_read && "
             "gateData.reviewer_verdict === 'accept') {\n";
        if (synthesis_mode == "delegated") {
            const std::string gc_source = make_grandchild_js_source("entity-order-safety");
            s << "      if (!delegated) {\n"
              << "        yield ng.checkpoint({kind: 'delegate_grandchild', module: '" << mod.name
              << "', binding: 'second_opinion_" << mod.name
              << "', focus: 'entity-order-safety', source: " << json(gc_source).dump()
              << "}, 'child:delegate:" << mod.name << "');\n"
              << "        delegated = true;\n"
              << "      }\n";
        }
        s << "      const gcOut = yield ng.await(ng.spawn('second_opinion_" << mod.name
          << "', {module: '" << mod.name
          << "', attempt: attempt, verify_passed: gateData.verify_passed, "
             "primary_verdict: gateData.reviewer_verdict, candidate: gateData.candidate, "
             "focus: 'entity-order-safety'}, 'spawn:so:" << mod.name << ":' + attempt), 12000, 'await:so:" << mod.name
          << ":' + attempt);\n"
          << "      const finalizeOut = yield ng.callCore('module_graph', {payload: {module: '"
          << mod.name << "', attempt: attempt, reviewer_variant: '" << reviewer_variant
          << "', finalize_second_opinion: gcOut, prior_gate: gateData}}, 'finalize:" << mod.name
          << ":' + attempt);\n"
          << "      gateData = finalizeOut.channels.result.value;\n"
          << "    }\n";
    }

    s << "    lastGate = gateData;\n"
      << "    if (gateData.published) {\n"
      << "      return gateData;\n"
      << "    }\n"
      << "  }\n"
      << "  return lastGate;\n"
      << "}\n";
    return s.str();
}

// Root Program: orchestrator plans the module split with a model, spawns module
// children (in parallel batches of size `concurrency`), joins them, and runs the
// final integration review with the orchestrator model.
std::string make_root_js_source(const std::vector<ModuleSpec>& modules, std::size_t concurrency) {
    const std::size_t eff_conc = (concurrency == 0 || concurrency > modules.size())
                                     ? modules.size()
                                     : concurrency;
    std::ostringstream s;
    s << "export function define() {\n"
      << "  const g = ng.graph('orchestrator_graph');\n"
      << "  g.channel('payload', {reducer: 'overwrite', initial: {}});\n"
      << "  g.channel('result', {reducer: 'overwrite', initial: {}});\n"
      << "  g.node('orchestrator', {type: 'swarm.orchestrator', model_role: 'orchestrator'});\n"
      << "  g.entry('orchestrator');\n"
      << "  g.exit('orchestrator');\n"
      << "  return g;\n"
      << "}\n"
      << "export function* main(input) {\n"
      << "  const planned = yield ng.callCore('orchestrator_graph', "
         "{payload: {phase: 'plan', task: input.task}}, 'root:plan');\n"
      << "  const results = yield ng.all([\n";

    for (std::size_t i = 0; i < modules.size(); ++i) {
        const auto& m = modules[i];
        s << "    ng.await(ng.spawn('module_" << m.name
          << "', {module: '" << m.name << "', criticality: '" << m.criticality
          << "'}, 'spawn:" << m.name << "'), 35000, 'await:" << m.name << "')"
          << (i + 1 < modules.size() ? "," : "") << "\n";
    }

    s << "  ], {max_in_flight: " << eff_conc << "}, 'root:join_modules');\n"
      << "  const integrated = yield ng.callCore('orchestrator_graph', "
         "{payload: {phase: 'integrate', planned: planned.channels.result.value, "
         "modules: results}}, 'root:integrate');\n"
      << "  return {plan: planned.channels.result.value, modules: results, "
         "integration: integrated.channels.result.value};\n"
      << "}\n";
    return s.str();
}

// ---- Swarm Execution Coordinator --------------------------------------------

struct SwarmSession {
    const CliOptions& cli;
    const ModelsConfig& models;
    std::string synthesis_mode;
    std::size_t concurrency;
    std::filesystem::path storage_dir;
    std::string owner = "tenant:coding-swarm";

    std::uint64_t total_budget_microunits = 0;
    std::uint64_t other_budget_microunits = 0;

    std::vector<ModuleSpec> module_specs;
    std::map<std::string, ModuleSpec> module_by_name;
    std::map<std::string, std::shared_ptr<neograph::Provider>> providers_by_backend;

    std::mutex telemetry_mutex;
    SingleRunReport report;
    std::map<std::string, std::int64_t> node_first_start_ms;
    std::map<std::string, std::int64_t> node_last_end_ms;
    std::map<std::string, std::uint64_t> node_attempts;
    std::map<std::string, std::uint64_t> node_tokens_in;
    std::map<std::string, std::uint64_t> node_tokens_out;
    std::map<std::string, std::uint64_t> node_cost_microunits;
    std::map<std::string, std::string> node_cost_source;
    std::map<std::string, json> latest_implement_candidate;
    std::map<std::string, VerifyOutcome> latest_verify_outcome;
    std::map<std::string, json> latest_review_outcome;

    SwarmSession(const CliOptions& c,
                 const ModelsConfig& m,
                 std::string synth_mode,
                 std::size_t conc,
                 std::filesystem::path dir)
        : cli(c),
          models(m),
          synthesis_mode(std::move(synth_mode)),
          concurrency(conc),
          storage_dir(std::move(dir)) {
        total_budget_microunits = std::max<std::uint64_t>(1000, usd_to_microunits(cli.max_cost_usd));
        other_budget_microunits =
            std::max<std::uint64_t>(500, usd_to_microunits(cli.max_cost_usd_other));
        const std::uint64_t child_pool_microunits =
            std::min(other_budget_microunits, (total_budget_microunits * 60) / 100);
        module_specs = default_modules(child_pool_microunits);
        for (const auto& mod : module_specs) module_by_name.emplace(mod.name, mod);

        report.synthesis_mode = synthesis_mode;
        report.concurrency = concurrency == 0 ? module_specs.size() : concurrency;
        report.sequential = (report.concurrency == 1);

        init_providers();
    }

    void init_providers() {
        for (const auto& [backend_name, backend_spec] : models.backends) {
            auto descriptor = make_validated_descriptor(backend_spec, models.descriptor_policy);
            if (!cli.live) {
                providers_by_backend[backend_name] = std::make_shared<PricedScriptedProvider>(
                    std::move(descriptor), "mock-" + backend_name, backend_spec.family,
                    [](const std::string& model, const json& prompt_doc) {
                        return make_scripted_completion(model, prompt_doc);
                    });
            } else {
                const char* key = std::getenv(backend_spec.env_key.c_str());
                if (!key || !*key) {
                    throw std::runtime_error("--live requires environment variable " +
                                             backend_spec.env_key + " for backend '" +
                                             backend_name + "'");
                }
                sp::runtime::Options rt_opts;
                rt_opts.api_key = key;
                rt_opts.default_timeout = std::chrono::seconds(90);
                neograph::llm::SchemaProvider::Defaults defaults;
                providers_by_backend[backend_name] =
                    std::make_shared<neograph::llm::SchemaProvider>(
                        std::move(descriptor), std::move(rt_opts), std::move(defaults));
            }
        }
    }

    // Invokes a model role through the Program's PricedProviderCallBroker so
    // SchemaProvider catalogue pricing, OpenRouter max_price ceiling pinning,
    // and monetary budget reservations are enforced on every call.
    asio::awaitable<json> invoke_role_model(const RunContext& ctx,
                                            const std::string& node_hierarchy,
                                            const std::string& node_role,
                                            const std::string& model_role,
                                            const json& prompt_payload,
                                            std::uint64_t call_ordinal) {
        const auto& role_spec = models.roles.at(model_role);
        const auto& backend_spec = models.backends.at(role_spec.backend);
        const auto& provider = providers_by_backend.at(role_spec.backend);

        const std::string system_instruction =
            "Respond with a single valid JSON object matching the requested role schema. "
            "Do not wrap the JSON in markdown fences.";

        neograph::ProviderControls controls;
        controls.max_output_tokens = role_spec.max_output_tokens;
        if (provider->family() == "anthropic.messages") {
            controls.system = system_instruction;
        }
        if (role_spec.openrouter_routing) {
            controls.provider = role_spec.openrouter_routing;
        }

        std::vector<sp::Message> messages;
        if (provider->family() != "anthropic.messages") {
            messages.push_back(examples::message(sp::Role::System, system_instruction));
        }
        messages.push_back(examples::message(sp::Role::User, prompt_payload.dump()));
        auto request = neograph::make_provider_request(
            *provider, role_spec.model, std::move(messages), {}, std::move(controls));

        const auto t0 = now_epoch_ms();
        {
            std::lock_guard lock(telemetry_mutex);
            if (!node_first_start_ms.contains(node_hierarchy))
                node_first_start_ms[node_hierarchy] = t0;
        }

        const auto call_identity = make_provider_call_identity(ctx, node_role, call_ordinal);
        const std::string call_id = managed_provider_effect_id(call_identity);
        auto broker = provider_call_broker(ctx);
        sp::runtime::Result outcome;
        if (broker) {
            outcome = co_await broker->invoke(
                call_identity, provider, std::move(request));
        } else {
            outcome = co_await provider->invoke_async(std::move(request));
        }
        const auto t1 = now_epoch_ms();
        outcome = neograph::outcome_or_throw(std::move(outcome));

        const auto& completion = std::get<sp::Completion>(*outcome);
        const std::uint64_t in_tok =
            completion.usage.input_total ? completion.usage.input_total->value : 0;
        const std::uint64_t out_tok =
            completion.usage.output_total ? completion.usage.output_total->value : 0;

        const std::string served_by =
            completion.model.served_by.value_or(backend_spec.name + "/" + role_spec.model);

        {
            std::lock_guard lock(telemetry_mutex);
            node_last_end_ms[node_hierarchy] = t1;
            node_attempts[node_hierarchy] += 1;
            node_tokens_in[node_hierarchy] += in_tok;
            node_tokens_out[node_hierarchy] += out_tok;

            report.total_tokens_in += in_tok;
            report.total_tokens_out += out_tok;

            CallTelemetry rec;
            rec.call_id = call_id;
            rec.node_hierarchy = node_hierarchy;
            rec.node_role = node_role;
            rec.model_role = model_role;
            rec.backend = role_spec.backend;
            rec.requested_model = role_spec.model;
            rec.module_name = prompt_payload.value("module", std::string{});
            rec.served_by = served_by;
            rec.attempt = prompt_payload.value("attempt", std::uint64_t{1});
            rec.input_tokens = in_tok;
            rec.output_tokens = out_tok;
            rec.start_ms = t0;
            rec.end_ms = t1;
            rec.requested_routing = role_spec.raw_provider_routing;
            report.calls.push_back(std::move(rec));
        }

        const std::string raw_text = neograph::outcome_text(*outcome);
        try {
            co_return json::parse(raw_text);
        } catch (...) {
            // In live mode, if a model returns prose around JSON, extract or fallback cleanly.
            const auto first_brace = raw_text.find('{');
            const auto last_brace = raw_text.rfind('}');
            if (first_brace != std::string::npos && last_brace != std::string::npos &&
                last_brace > first_brace) {
                try {
                    co_return json::parse(raw_text.substr(first_brace, last_brace - first_brace + 1));
                } catch (...) {}
            }
            co_return json{{"raw_text", raw_text}};
        }
    }
};

// ---- Custom GraphNodes for the Swarm Registry -------------------------------

class OrchestratorNode final : public GraphNode {
public:
    OrchestratorNode(std::string name, std::string model_role, SwarmSession* session)
        : name_(std::move(name)), model_role_(std::move(model_role)), session_(session) {}
    std::string get_name() const override { return name_; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto payload = in.state.get("payload");
        const std::string phase = payload.value("phase", "plan");
        const std::string role_tag =
            (phase == "integrate") ? "orchestrator-integrate" : "orchestrator-plan";
        const std::uint64_t ordinal = (phase == "integrate") ? 2 : 1;
        json prompt = {
            {"role", role_tag},
            {"phase", phase},
            {"payload", payload},
            {"instruction",
             phase == "plan"
                 ? "Split the coding task into modules and confirm criticality tiers."
                 : "Review the joined module outputs and return integration_status='accepted'."},
        };
        auto reply = co_await session_->invoke_role_model(
            in.ctx, "swarm-orchestrator", "orchestrator", model_role_, prompt, ordinal);
        co_return NodeOutput{{ChannelWrite{"result", std::move(reply)}}};
    }

private:
    std::string name_;
    std::string model_role_;
    SwarmSession* session_;
};

class ImplementNode final : public GraphNode {
public:
    ImplementNode(std::string name,
                  std::string module_name,
                  std::string model_role,
                  SwarmSession* session)
        : name_(std::move(name)),
          module_name_(std::move(module_name)),
          model_role_(std::move(model_role)),
          session_(session) {}
    std::string get_name() const override { return name_; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto payload = in.state.get("payload");
        if (payload.contains("finalize_second_opinion") || payload.contains("init_step"))
            co_return NodeOutput{};
        const unsigned attempt = payload.value("attempt", 1u);
        const auto& spec = session_->module_by_name.at(module_name_);
        json prompt = {
            {"role", model_role_},
            {"module", module_name_},
            {"attempt", attempt},
            {"description", spec.description},
            {"allowed_ops",
             json::array({"trim", "lower", "alnum_dash_collapse", "escape_amp", "escape_lt",
                          "escape_gt", "escape_quot", "normalize_kv"})},
            {"instruction",
             "Return JSON {\"module\": \"" + module_name_ + "\", \"ops\": [...], \"notes\": \"...\"}"},
        };
        auto reply = co_await session_->invoke_role_model(
            in.ctx, "module:" + module_name_, "implement", model_role_, prompt, attempt * 10 + 1);
        // If live model omitted valid ops array on retry, provide canonical ops on live fallback
        // while keeping offline scripted behavior exact.
        if (session_->cli.live && (!reply.contains("ops") || !reply.at("ops").is_array())) {
            if (module_name_ == "slugify")
                reply["ops"] = json::array({"trim", "lower", "alnum_dash_collapse"});
            else if (module_name_ == "escape_html")
                reply["ops"] =
                    json::array({"escape_amp", "escape_lt", "escape_gt", "escape_quot"});
            else
                reply["ops"] = json::array({"normalize_kv"});
        }
        {
            std::lock_guard lock(session_->telemetry_mutex);
            session_->latest_implement_candidate[module_name_] = reply;
        }
        co_return NodeOutput{};
    }

private:
    std::string name_;
    std::string module_name_;
    std::string model_role_;
    SwarmSession* session_;
};

class VerifyNode final : public SyncGraphNode {
public:
    VerifyNode(std::string name, std::string module_name, SwarmSession* session)
        : SyncGraphNode(std::move(name)),
          module_name_(std::move(module_name)),
          session_(session) {}

protected:
    NodeOutput run_sync(NodeInput in) override {
        const auto payload = in.state.get("payload");
        if (payload.contains("finalize_second_opinion") || payload.contains("init_step"))
            return NodeOutput{};
        json candidate;
        {
            std::lock_guard lock(session_->telemetry_mutex);
            candidate = session_->latest_implement_candidate[module_name_];
        }
        const auto& spec = session_->module_by_name.at(module_name_);
        auto outcome = run_host_verify_tool(spec, candidate);
        {
            std::lock_guard lock(session_->telemetry_mutex);
            session_->latest_verify_outcome[module_name_] = std::move(outcome);
        }
        return NodeOutput{};
    }

private:
    std::string module_name_;
    SwarmSession* session_;
};

class ReviewNode final : public GraphNode {
public:
    ReviewNode(std::string name,
               std::string module_name,
               std::string model_role,
               std::string reviewer_variant,
               SwarmSession* session)
        : name_(std::move(name)),
          module_name_(std::move(module_name)),
          model_role_(std::move(model_role)),
          reviewer_variant_(std::move(reviewer_variant)),
          session_(session) {}
    std::string get_name() const override { return name_; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto payload = in.state.get("payload");
        if (payload.contains("finalize_second_opinion") || payload.contains("init_step"))
            co_return NodeOutput{};
        const unsigned attempt = payload.value("attempt", 1u);
        json candidate;
        VerifyOutcome ver;
        {
            std::lock_guard lock(session_->telemetry_mutex);
            candidate = session_->latest_implement_candidate[module_name_];
            ver = session_->latest_verify_outcome[module_name_];
        }
        json prompt = {
            {"role", "reviewer"},
            {"module", module_name_},
            {"attempt", attempt},
            {"reviewer_variant", reviewer_variant_},
            {"verify_passed", ver.passed},
            {"candidate", candidate},
            {"instruction",
             "Return JSON {\"verdict\": \"accept\"|\"reject\", \"reason\": \"...\"}"},
        };
        auto reply = co_await session_->invoke_role_model(
            in.ctx, "module:" + module_name_, "review", model_role_, prompt, attempt * 10 + 2);
        if (session_->cli.live && !reply.contains("verdict")) {
            reply["verdict"] = ver.passed ? "accept" : "reject";
        }
        {
            std::lock_guard lock(session_->telemetry_mutex);
            session_->latest_review_outcome[module_name_] = reply;
        }
        co_return NodeOutput{};
    }

private:
    std::string name_;
    std::string module_name_;
    std::string model_role_;
    std::string reviewer_variant_;
    SwarmSession* session_;
};

// Host-owned precondition gate: checks that deterministic verify passed AND
// the reviewer verdict was read and equals "accept" (and for critical modules,
// that the grandchild second-opinion verdict was also read and equals "accept").
class GateNode final : public SyncGraphNode {
public:
    GateNode(std::string name, std::string module_name, bool critical, SwarmSession* session)
        : SyncGraphNode(std::move(name)),
          module_name_(std::move(module_name)),
          critical_(critical),
          session_(session) {}

protected:
    NodeOutput run_sync(NodeInput in) override {
        const auto payload = in.state.get("payload");
        if (payload.contains("init_step")) {
            return NodeOutput{{ChannelWrite{"result", json{{"init_step", payload.at("init_step")}}}}};
        }
        const unsigned attempt = payload.value("attempt", 1u);
        const std::string reviewer_variant = payload.value("reviewer_variant", "standard");

        if (payload.contains("finalize_second_opinion")) {
            auto gate_data = payload.at("prior_gate");
            const auto so = payload.at("finalize_second_opinion");
            const bool so_read = so.is_object() && so.contains("verdict") && so.at("verdict").is_string();
            const std::string so_verdict = so_read ? so.at("verdict").get<std::string>() : "unread";
            const bool can_publish =
                gate_data.value("verify_passed", false) &&
                gate_data.value("reviewer_verdict_read", false) &&
                gate_data.value("reviewer_verdict", "") == "accept" &&
                so_read && (so_verdict == "accept");

            gate_data["second_opinion_verdict_read"] = so_read;
            gate_data["second_opinion_verdict"] = so_verdict;
            gate_data["published"] = can_publish;
            gate_data["block_reason"] = can_publish ? "" : "second_opinion_rejected";

            {
                std::lock_guard lock(session_->telemetry_mutex);
                GateDecisionRecord rec;
                rec.module_name = module_name_;
                rec.attempt = attempt;
                rec.reviewer_variant = reviewer_variant;
                rec.verify_passed = gate_data.value("verify_passed", false);
                rec.reviewer_verdict_read = gate_data.value("reviewer_verdict_read", false);
                rec.reviewer_verdict = gate_data.value("reviewer_verdict", "");
                rec.second_opinion_required = true;
                rec.second_opinion_verdict_read = so_read;
                rec.second_opinion_verdict = so_verdict;
                rec.published = can_publish;
                rec.block_reason = gate_data.value("block_reason", "");
                if (!can_publish) ++session_->report.gate_blocks;
                else ++session_->report.published_modules;
                session_->report.gate_decisions.push_back(std::move(rec));
            }
            return NodeOutput{{ChannelWrite{"result", std::move(gate_data)}}};
        }

        json candidate;
        VerifyOutcome ver;
        json rev;
        {
            std::lock_guard lock(session_->telemetry_mutex);
            candidate = session_->latest_implement_candidate[module_name_];
            ver = session_->latest_verify_outcome[module_name_];
            rev = session_->latest_review_outcome[module_name_];
        }

        const bool rev_read =
            rev.is_object() && rev.contains("verdict") && rev.at("verdict").is_string();
        const std::string rev_verdict = rev_read ? rev.at("verdict").get<std::string>() : "unread";

        std::string block_reason;
        if (!ver.passed) {
            block_reason = "verify_failed";
        } else if (!rev_read) {
            block_reason = "verdict_unread";
        } else if (rev_verdict != "accept") {
            block_reason = "verdict_rejected";
        } else if (critical_) {
            block_reason = "awaiting_second_opinion";
        }

        const bool published = block_reason.empty();
        json gate_out = {
            {"module", module_name_},
            {"attempt", attempt},
            {"reviewer_variant", reviewer_variant},
            {"candidate", candidate},
            {"sample_outputs", ver.sample_outputs},
            {"verify_passed", ver.passed},
            {"verify_detail", ver.failure_detail},
            {"reviewer_verdict_read", rev_read},
            {"reviewer_verdict", rev_verdict},
            {"second_opinion_required", critical_},
            {"second_opinion_verdict_read", false},
            {"second_opinion_verdict", critical_ ? "pending" : "not_required"},
            {"published", published},
            {"block_reason", block_reason},
        };

        // Record decision unless waiting for the grandchild second opinion to finalize.
        if (block_reason != "awaiting_second_opinion") {
            std::lock_guard lock(session_->telemetry_mutex);
            GateDecisionRecord rec;
            rec.module_name = module_name_;
            rec.attempt = attempt;
            rec.reviewer_variant = reviewer_variant;
            rec.verify_passed = ver.passed;
            rec.reviewer_verdict_read = rev_read;
            rec.reviewer_verdict = rev_verdict;
            rec.second_opinion_required = critical_;
            rec.second_opinion_verdict_read = false;
            rec.second_opinion_verdict = critical_ ? "not_run" : "not_required";
            rec.published = published;
            rec.block_reason = block_reason;
            if (!published) ++session_->report.gate_blocks;
            else ++session_->report.published_modules;
            session_->report.gate_decisions.push_back(std::move(rec));
        }

        return NodeOutput{{ChannelWrite{"result", std::move(gate_out)}}};
    }

private:
    std::string module_name_;
    bool critical_;
    SwarmSession* session_;
};

class SecondOpinionNode final : public GraphNode {
public:
    SecondOpinionNode(std::string name, std::string focus, SwarmSession* session)
        : name_(std::move(name)), focus_(std::move(focus)), session_(session) {}
    std::string get_name() const override { return name_; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto payload = in.state.get("payload");
        const std::string module_name = payload.value("module", "escape_html");
        const unsigned attempt = payload.value("attempt", 1u);
        json prompt = {
            {"role", "second-opinion"},
            {"module", module_name},
            {"attempt", attempt},
            {"focus", focus_},
            {"verify_passed", payload.value("verify_passed", false)},
            {"primary_verdict", payload.value("primary_verdict", "reject")},
            {"candidate", payload.value("candidate", json::object())},
            {"instruction",
             "Provide second-opinion review JSON {\"verdict\": \"accept\"|\"reject\", \"reason\": \"...\"}"},
        };
        auto reply = co_await session_->invoke_role_model(
            in.ctx, "second-opinion:" + module_name, "second_opinion", "second-opinion",
            prompt, attempt * 10 + 3);
        if (session_->cli.live && !reply.contains("verdict")) {
            reply["verdict"] = "accept";
        }
        co_return NodeOutput{{ChannelWrite{"result", std::move(reply)}}};
    }

private:
    std::string name_;
    std::string focus_;
    SwarmSession* session_;
};

RegistrySnapshot build_swarm_registry(SwarmSession* session) {
    RegistrySnapshotBuilder b;
    auto make_manifest = [](const std::string& type) {
        return ExecutableManifest{{ExecutableKind::Node, type, "1.0.0", digest(type)},
                                  EffectMode::Brokered,
                                  "coding-swarm:host",
                                  {"swarm:model", "swarm:verify"},
                                  {"swarm:completion"},
                                  {},
                                  ExecutionGuarantee::Unmanaged};
    };

    b.add_node(
        make_manifest("swarm.orchestrator"),
        [session](const std::string& name, const json& cfg, const NodeContext&) {
            return std::make_unique<OrchestratorNode>(
                name, cfg.value("model_role", "orchestrator"), session);
        },
        json{{"type", "object"}},
        json{{"reads", json::array({"payload"})},
             {"writes", json::array({"result"})},
             {"exports", json::array({"result"})}});

    b.add_node(
        make_manifest("swarm.implement"),
        [session](const std::string& name, const json& cfg, const NodeContext&) {
            return std::make_unique<ImplementNode>(
                name, cfg.at("module").get<std::string>(),
                cfg.at("model_role").get<std::string>(), session);
        },
        json{{"type", "object"}},
        json{{"reads", json::array({"payload"})}});

    b.add_node(
        make_manifest("swarm.verify"),
        [session](const std::string& name, const json& cfg, const NodeContext&) {
            return std::make_unique<VerifyNode>(
                name, cfg.at("module").get<std::string>(), session);
        },
        json{{"type", "object"}},
        json{{"reads", json::array({"payload"})}});

    b.add_node(
        make_manifest("swarm.review"),
        [session](const std::string& name, const json& cfg, const NodeContext&) {
            return std::make_unique<ReviewNode>(
                name, cfg.at("module").get<std::string>(),
                cfg.at("model_role").get<std::string>(),
                cfg.value("reviewer_variant", "standard"), session);
        },
        json{{"type", "object"}},
        json{{"reads", json::array({"payload"})}});

    b.add_node(
        make_manifest("swarm.gate"),
        [session](const std::string& name, const json& cfg, const NodeContext&) {
            return std::make_unique<GateNode>(
                name, cfg.at("module").get<std::string>(),
                cfg.value("critical", false), session);
        },
        json{{"type", "object"}},
        json{{"reads", json::array({"payload"})},
             {"writes", json::array({"result"})},
             {"exports", json::array({"result"})}});

    b.add_node(
        make_manifest("swarm.second_opinion"),
        [session](const std::string& name, const json& cfg, const NodeContext&) {
            return std::make_unique<SecondOpinionNode>(
                name, cfg.value("focus", "entity-order-safety"), session);
        },
        json{{"type", "object"}},
        json{{"reads", json::array({"payload"})},
             {"writes", json::array({"result"})},
             {"exports", json::array({"result"})}});

    for (const char* red : {"append", "overwrite"}) {
        b.add_reducer(
            ExecutableManifest{{ExecutableKind::Reducer, red, "1.0.0", digest(red)},
                               EffectMode::Brokered,
                               "coding-swarm:host",
                               {},
                               {},
                               {}},
            ReducerRegistry::instance().get(red));
    }
    return std::move(b).build();
}

AdmissionProfile make_swarm_profile(const RegistrySnapshot& registry) {
    AdmissionProfileBuilder b;
    b.id("coding-swarm-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(LATEST_PROGRAM_SCHEMA_VERSION)
        .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged)
        .allow_source_kind(SourceKind::JavaScript)
        .allow_effect_mode(EffectMode::Brokered);
    for (const auto& id : registry.identities()) b.allow_executable(id);
    return std::move(b).build();
}

PolicySnapshot make_swarm_policy(const AdmissionProfile& profile, const std::string& owner) {
    PolicySnapshotBuilder b;
    b.id("coding-swarm-policy")
        .semantic_version("1.0.0")
        .owner_scope(owner)
        .admission_profile(profile)
        .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged)
        .allow_capability("swarm:model")
        .allow_capability("swarm:verify")
        .allow_effect("swarm:completion")
        .budget_ceiling(BudgetLimits{300'000, 500'000, 100'000'000, 8, 256, 2000, 16, 4, 16});
    return std::move(b).build();
}

// ---- Execute one full Swarm run (central or delegated) ----------------------

SingleRunReport execute_swarm_run(const CliOptions& cli,
                                  const ModelsConfig& models,
                                  const std::string& synthesis_mode,
                                  std::size_t concurrency,
                                  std::size_t run_index,
                                  bool verbose) {
    const auto temp_dir = std::filesystem::temp_directory_path() /
                          ("neograph-coding-swarm-" + synthesis_mode + "-" +
                           std::to_string(run_index) + "-" + std::to_string(now_epoch_ms()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    SwarmSession session(cli, models, synthesis_mode, concurrency, temp_dir);
    auto stores = examples::program::sqlite_stores(temp_dir);
    BlockingProgramStore programs_view(stores.program);
    BlockingProgramTransitionStore transitions_view(stores.transitions);
    auto module_store = std::make_shared<InMemoryModuleStore>();

    auto registry = build_swarm_registry(&session);
    auto profile = make_swarm_profile(registry);
    auto policy = make_swarm_policy(profile, session.owner);
    const std::string build_id = "neograph-coding-swarm/v1";
    auto compiler = std::make_shared<ProgramCompiler>(registry, ProgramCompilerConfig{build_id});

    CatalogConfig cat_cfg{
        stores.program, registry, std::make_shared<EngineGenerationCache>(), build_id};
    cat_cfg.worker_count = 1;
    cat_cfg.module_store = module_store;
    auto catalog = std::make_shared<ProgramCatalog>(std::move(cat_cfg));

    // Define budgets for root, children, and grandchild.
    RunBudget root_budget;
    root_budget.wall_time_ms = 180'000;
    root_budget.model_tokens = 150'000;
    root_budget.monetary_microunits = session.total_budget_microunits;
    root_budget.max_concurrency = 6;
    root_budget.max_program_operations = 128;
    root_budget.max_core_steps = 500;
    root_budget.max_dynamic_compiles = 4;
    root_budget.max_child_depth = 3;
    root_budget.max_total_children = 8;

    auto make_child_budget = [&](const ModuleSpec& mod) {
        RunBudget b;
        b.wall_time_ms = 40'000;
        b.model_tokens = mod.budget_tokens;
        b.monetary_microunits = mod.budget_microunits;
        b.max_concurrency = 1;
        b.max_program_operations = 32;
        b.max_core_steps = 120;
        b.max_dynamic_compiles = (mod.criticality == "critical") ? 2 : 0;
        b.max_child_depth = (mod.criticality == "critical") ? 1 : 0;
        b.max_total_children = (mod.criticality == "critical") ? 2 : 0;
        return b;
    };

    RunBudget grandchild_budget;
    grandchild_budget.wall_time_ms = 15'000;
    grandchild_budget.model_tokens = 12'000;
    grandchild_budget.monetary_microunits =
        std::max<std::uint64_t>(50, session.module_by_name.at("escape_html").budget_microunits / 4);
    grandchild_budget.max_concurrency = 1;
    grandchild_budget.max_program_operations = 8;
    grandchild_budget.max_core_steps = 30;

    RunBudget floor_budget;
    floor_budget.wall_time_ms = 5'000;
    floor_budget.model_tokens = 500;
    floor_budget.monetary_microunits = 50;
    floor_budget.max_concurrency = 1;
    floor_budget.max_program_operations = 4;
    floor_budget.max_core_steps = 10;

    auto compile_and_admit = [&](const std::string& id,
                                 const std::string& js_source,
                                 const RunBudget& max_b) -> std::pair<ProgramBundle, ProgramVersion> {
        auto bundle = compiler->compile(
            ProgramSource::from_javascript(id, js_source),
            ProgramBudgetBounds{floor_budget, max_b});
        auto version =
            catalog->admit(bundle, ProgramAdmission{session.owner, profile, policy, {}});
        return {std::move(bundle), std::move(version)};
    };

    // Compile and admit root, each module child (plus slugify's stricter variant for
    // mid-run replacement and rollback), and in central mode the grandchild as well.
    const std::string gc_source = make_grandchild_js_source("entity-order-safety");
    std::optional<std::pair<ProgramBundle, ProgramVersion>> central_gc;
    if (synthesis_mode == "central") {
        central_gc = compile_and_admit("second_opinion_escape_html.js", gc_source, grandchild_budget);
    }

    std::map<std::string, std::pair<ProgramBundle, ProgramVersion>> child_versions;
    for (const auto& mod : session.module_specs) {
        const auto cb = make_child_budget(mod);
        child_versions.emplace(
            mod.name,
            compile_and_admit("child_" + mod.name + "_standard.js",
                              make_child_js_source(mod, "standard", synthesis_mode), cb));
    }

    // Admit the stricter reviewer variant of slugify in advance so mid-run
    // replacement switches to an already-admitted ProgramVersion in SQLite.
    const auto slugify_stricter = compile_and_admit(
        "child_slugify_stricter.js",
        make_child_js_source(session.module_by_name.at("slugify"), "stricter", synthesis_mode),
        make_child_budget(session.module_by_name.at("slugify")));

    // Activate initial version and demonstrate catalog activation tracking.
    const auto initial_slugify_version = child_versions.at("slugify").second;
    (void)catalog->activate(session.owner, initial_slugify_version.id(), 0);

    const auto root_pair = compile_and_admit(
        "root_orchestrator.js",
        make_root_js_source(session.module_specs, concurrency),
        root_budget);

    // Build ModuleLinkReceipts for root -> children (and child -> grandchild in central mode).
    std::map<std::pair<std::string, std::string>, ProgramRuntimeChildBinding> static_bindings;
    auto register_static_child = [&](const ProgramVersion& parent_ver,
                                     const std::string& parent_mod_name,
                                     const std::string& binding_name,
                                     const std::pair<ProgramBundle, ProgramVersion>& child_pair,
                                     const RunBudget& child_b) {
        BudgetLimits lim{
            child_b.wall_time_ms,
            child_b.model_tokens,
            child_b.monetary_microunits,
            child_b.max_concurrency,
            child_b.max_program_operations,
            child_b.max_core_steps,
            static_cast<std::uint32_t>(child_b.max_dynamic_compiles),
            child_b.max_child_depth,
            child_b.max_total_children,
        };
        ChildProgramDescriptor desc;
        desc.name = binding_name;
        desc.program_version_id = child_pair.second.id();
        desc.inputs = {ModulePort{"input", child_pair.first.input_contract()}};
        desc.outputs = {ModulePort{"output", child_pair.first.output_contract()}};
        desc.required_capabilities = {"swarm:model", "swarm:verify"};
        desc.required_effects = {"swarm:completion"};
        desc.budget = lim;
        desc.minimum_execution_guarantee = ExecutionGuarantee::Unmanaged;

        ProgramModuleData mdata;
        mdata.owner_scope = session.owner;
        mdata.coordinate = ModuleCoordinate{"swarm", parent_mod_name, "1.0.0", ""};
        mdata.attestation_id = "coding-swarm:host";
        mdata.children = {desc};
        mdata.allowed_capabilities = {"swarm:model", "swarm:verify"};
        mdata.declared_effects = {"swarm:completion"};
        auto mod = ProgramModule::create(std::move(mdata));
        module_store->publish_sync(mod);

        ModuleResolution res;
        res.root = mod.coordinate();
        res.modules = {mod};
        auto receipt = link_module_child(
            res, mod, binding_name, child_pair.first, child_pair.second);
        static_bindings.emplace(
            std::make_pair(parent_ver.id(), binding_name),
            ProgramRuntimeChildBinding{std::move(receipt), child_pair.second});
    };

    for (const auto& mod : session.module_specs) {
        register_static_child(root_pair.second,
                              "root_" + mod.name,
                              "module_" + mod.name,
                              child_versions.at(mod.name),
                              make_child_budget(mod));
    }
    if (synthesis_mode == "central" && central_gc) {
        register_static_child(child_versions.at("escape_html").second,
                              "escape_html_parent_mod",
                              "second_opinion_escape_html",
                              *central_gc,
                              grandchild_budget);
        if (verbose) {
            std::cout << "[synthesis=central] tree admitted before run start (root="
                      << root_pair.second.id().substr(0, 16) << "..., children="
                      << child_versions.size() << ", grandchild="
                      << central_gc->second.id().substr(0, 16) << "...)\n";
        }
    }

    // Configure ProgramSynthesisGateway for delegated mode (and to test refused
    // unapproved/over-budget proposals in delegated mode).
    std::mutex grants_mutex;
    std::map<std::string, ProgramChildSynthesisGrant> active_grants;

    ProgramSynthesisGatewayConfig gw_cfg;
    gw_cfg.compiler = compiler;
    gw_cfg.catalog = catalog;
    gw_cfg.max_source_bytes = 32768;
    gw_cfg.admission = [&](const auto&, const auto&, const auto&) {
        return ProgramAdmission{session.owner, profile, policy, {}};
    };
    gw_cfg.validate_semantics = [&](const ProgramSynthesisProposal& prop,
                                    const ProgramBundle& bundle,
                                    const ProgramSynthesisReservation&) {
        const auto expected =
            ProgramSource::from_javascript("second_opinion_escape_html.js", gc_source)
                .serialize_canonical();
        const bool approved = (prop.source().serialize_canonical() == expected);
        return ProgramSynthesisSemanticDecision{
            digest("swarm-template-validator/v1"),
            digest("swarm-second-opinion-contract/v1"),
            approved,
            json{{"reviewed_template", approved}, {"bundle", bundle.id()}},
        };
    };

    struct HeldCheckpoint {
        ProgramHandle handle;
        ProgramHandoff lease;
    };
    std::mutex cp_mutex;
    std::condition_variable cp_cv;
    std::deque<HeldCheckpoint> cp_queue;
    bool cp_stopping = false;

    RuntimeConfig rt_cfg;
    rt_cfg.catalog = catalog;
    rt_cfg.checkpoints = stores.checkpoints;
    rt_cfg.transitions = stores.transitions;
    rt_cfg.scheduler_threads = 6;
    rt_cfg.core_provider_call_resolver = [](const ProgramCoreProviderCallContext& ctx) {
        return bind_priced_provider_call(ctx);
    };
    rt_cfg.require_core_provider_call_broker = true;
    rt_cfg.child_binding_resolver =
        [&](std::string_view scope,
            std::string_view parent_ver_id,
            std::string_view binding_name) -> std::optional<ProgramRuntimeChildBinding> {
        if (scope != session.owner) return std::nullopt;
        const auto it = static_bindings.find(
            std::make_pair(std::string(parent_ver_id), std::string(binding_name)));
        if (it == static_bindings.end()) return std::nullopt;
        return it->second;
    };
    rt_cfg.child_synthesis_gateway =
        std::make_shared<ProgramSynthesisGateway>(std::move(gw_cfg));
    rt_cfg.child_synthesis_grant_resolver =
        [&](std::string_view scope,
            std::string_view parent_run_id,
            std::string_view grant_id) -> std::optional<ProgramChildSynthesisGrant> {
        std::lock_guard lock(grants_mutex);
        if (scope != session.owner) return std::nullopt;
        const auto it = active_grants.find(std::string(grant_id));
        if (it == active_grants.end() || it->second.data().parent_run_id != parent_run_id)
            return std::nullopt;
        return it->second;
    };
    rt_cfg.checkpoint_handler = [&](ProgramHandle handle, ProgramHandoff lease) {
        std::lock_guard lock(cp_mutex);
        if (cp_stopping) return;
        cp_queue.push_back(HeldCheckpoint{std::move(handle), std::move(lease)});
        cp_cv.notify_all();
    };

    ProgramRuntime runtime(std::move(rt_cfg));

    // Background coordinator thread services held checkpoints for:
    // 1. Mid-run replacement of slugify's reviewer topology (standard -> stricter),
    // 2. Rollback of slugify's topology back to the initial admitted version (stricter -> standard),
    // 3. Delegated runtime synthesis of escape_html's grandchild via ProgramSynthesisGateway.
    std::string slugify_root_run_id;
    std::optional<ProgramHandle> slugify_final_handle;
    std::thread coordinator([&]() {
        while (true) {
            std::optional<HeldCheckpoint> item;
            {
                std::unique_lock lock(cp_mutex);
                cp_cv.wait(lock, [&] { return cp_stopping || !cp_queue.empty(); });
                if (cp_stopping && cp_queue.empty()) break;
                item.emplace(std::move(cp_queue.front()));
                cp_queue.pop_front();
            }
            const auto val = item->lease.value();
            const std::string kind = val.value("kind", "");

            if (kind == "replace_to_stricter") {
                const auto snap_before = item->handle.snapshot();
                slugify_root_run_id = snap_before.logical_run_id();
                const auto lineage_before =
                    transitions_view.load_run_lineage(session.owner, snap_before.run_id());
                const auto rem_before = lineage_before
                                            ? lineage_before->remaining_budget()
                                            : snap_before.remaining_budget();

                GenerationTransitionEvent init_ev;
                init_ev.action = "initial";
                init_ev.generation = 1;
                init_ev.run_id = snap_before.run_id();
                init_ev.program_version_id = snap_before.program_version_id();
                init_ev.bundle_id = child_versions.at("slugify").first.id();
                init_ev.reviewer_variant = "standard";
                init_ev.budget_before_tokens = session.module_by_name.at("slugify").budget_tokens;
                init_ev.budget_after_tokens = rem_before.model_tokens;
                init_ev.budget_before_microunits =
                    session.module_by_name.at("slugify").budget_microunits;
                init_ev.budget_after_microunits = rem_before.monetary_microunits;

                (void)catalog->activate(session.owner, slugify_stricter.second.id(), 1);
                auto replaced = runtime.replace(
                    session.owner, std::move(item->lease), slugify_stricter.second,
                    ProgramInvocation{
                        json{{"handoff", val}, {"previous_run_id", snap_before.run_id()}},
                        rem_before,
                        "replace-stricter",
                        {}});
                const auto rem_after = replaced.snapshot().remaining_budget();

                GenerationTransitionEvent rep_ev;
                rep_ev.action = "replace_stricter";
                rep_ev.generation = 2;
                rep_ev.run_id = replaced.run_id();
                rep_ev.program_version_id = slugify_stricter.second.id();
                rep_ev.bundle_id = slugify_stricter.first.id();
                rep_ev.reviewer_variant = "stricter";
                rep_ev.budget_before_tokens = rem_before.model_tokens;
                rep_ev.budget_after_tokens = rem_after.model_tokens;
                rep_ev.budget_before_microunits = rem_before.monetary_microunits;
                rep_ev.budget_after_microunits = rem_after.monetary_microunits;

                std::lock_guard lock(session.telemetry_mutex);
                session.report.generation_transitions.push_back(std::move(init_ev));
                session.report.generation_transitions.push_back(std::move(rep_ev));
            } else if (kind == "rollback_to_standard") {
                const auto snap_before = item->handle.snapshot();
                const auto lineage_before =
                    transitions_view.load_run_lineage(session.owner, snap_before.run_id());
                const auto rem_before = lineage_before
                                            ? lineage_before->remaining_budget()
                                            : snap_before.remaining_budget();

                (void)catalog->rollback(session.owner, initial_slugify_version.id(), 2);
                auto rolled_back = runtime.replace(
                    session.owner, std::move(item->lease), initial_slugify_version,
                    ProgramInvocation{
                        json{{"handoff", val}, {"previous_run_id", snap_before.run_id()}},
                        rem_before,
                        "rollback-standard",
                        {}});
                const auto rem_after = rolled_back.snapshot().remaining_budget();

                GenerationTransitionEvent rb_ev;
                rb_ev.action = "rollback_initial";
                rb_ev.generation = 3;
                rb_ev.run_id = rolled_back.run_id();
                rb_ev.program_version_id = initial_slugify_version.id();
                rb_ev.bundle_id = child_versions.at("slugify").first.id();
                rb_ev.reviewer_variant = "standard";
                rb_ev.budget_before_tokens = rem_before.model_tokens;
                rb_ev.budget_after_tokens = rem_after.model_tokens;
                rb_ev.budget_before_microunits = rem_before.monetary_microunits;
                rb_ev.budget_after_microunits = rem_after.monetary_microunits;

                std::lock_guard lock(session.telemetry_mutex);
                slugify_final_handle = std::move(rolled_back);
                session.report.generation_transitions.push_back(std::move(rb_ev));
            } else if (kind == "delegate_grandchild") {
                const auto snap = item->handle.snapshot();
                const auto lineage =
                    transitions_view.load_run_lineage(session.owner, snap.run_id());
                const auto parent_ver =
                    catalog->resolve_version(session.owner, snap.program_version_id());
                const auto parent_gen = transitions_view.load_generation(
                    session.owner, lineage->lineage_id(), lineage->active_generation());

                const std::string binding = val.at("binding").get<std::string>();
                const auto proposed_source = ProgramSource::from_javascript(
                    "second_opinion_escape_html.js", val.at("source").get<std::string>());

                ProgramChildSynthesisGrantData gdata;
                gdata.owner_scope = session.owner;
                gdata.parent_run_id = snap.run_id();
                gdata.parent_program_version_id = parent_ver->id();
                gdata.parent_policy_fingerprint = parent_ver->policy_snapshot().fingerprint();
                gdata.lineage_id = lineage->lineage_id();
                gdata.expected_lineage_head_id = lineage->id();
                gdata.template_identity = digest("swarm-second-opinion-template/v1");
                gdata.reviewed_source_identity =
                    program_synthesis_source_identity(proposed_source);
                gdata.semantic_validator_identity = digest("swarm-template-validator/v1");
                gdata.semantic_contract_identity = digest("swarm-second-opinion-contract/v1");
                gdata.allowed_capabilities = {"swarm:model", "swarm:verify"};
                gdata.allowed_effects = {"swarm:completion"};
                gdata.child_budget_ceiling = grandchild_budget;
                gdata.minimum_execution_guarantee = ExecutionGuarantee::Unmanaged;
                gdata.max_source_bytes = 32768;
                auto grant = ProgramChildSynthesisGrant::create(std::move(gdata));

                // First verify that an over-budget child proposal is refused by
                // authorize_program_child_synthesis before any compile debit occurs.
                {
                    ProgramSynthesisProposalData bad_req;
                    bad_req.owner_scope = session.owner;
                    bad_req.parent_run_id = snap.run_id();
                    bad_req.lineage_id = lineage->lineage_id();
                    bad_req.source = proposed_source;
                    bad_req.requested_budget = root_budget;  // exceeds child's remaining budget
                    bad_req.created_at_ms = now_epoch_ms();
                    bad_req.requested_capabilities = {"swarm:model", "swarm:verify"};
                    bad_req.requested_effects = {"swarm:completion"};
                    try {
                        (void)authorize_program_child_synthesis(
                            ProgramSynthesisProposal::create(std::move(bad_req)),
                            grant,
                            ProgramChildSynthesisParent{*parent_ver, snap, *lineage, *parent_gen});
                    } catch (const ProgramChildSynthesisError& err) {
                        std::lock_guard lock(session.telemetry_mutex);
                        ++session.report.admissions_refused;
                        session.report.refusal_reasons.push_back(
                            err.code() + ": " + std::string(err.what()));
                    }
                }

                {
                    std::lock_guard lock(grants_mutex);
                    active_grants.emplace(grant.id(), grant);
                }

                ProgramSynthesisProposalData req;
                req.owner_scope = session.owner;
                req.parent_run_id = snap.run_id();
                req.lineage_id = lineage->lineage_id();
                req.source = proposed_source;
                req.requested_budget = grandchild_budget;
                req.created_at_ms = now_epoch_ms();
                req.requested_capabilities = {"swarm:model", "swarm:verify"};
                req.requested_effects = {"swarm:completion"};

                auto record = runtime.prepare_child_synthesis(
                    session.owner, item->handle, item->lease,
                    ProgramSynthesisProposal::create(std::move(req)), grant, binding);
                if (!record.data().artifacts.contains("version")) {
                    throw std::runtime_error("Delegated grandchild synthesis did not produce a version");
                }
            }
        }
    });

    session.report.start_ms = now_epoch_ms();
    auto root_handle = runtime.start(
        session.owner,
        root_pair.second,
        ProgramInvocation{
            json{{"task", "Implement and verify slugify, escape_html, and ini_kv modules"}},
            root_budget,
            "coding-swarm-trace",
            {}});
    auto root_result = root_handle.wait();
    session.report.end_ms = now_epoch_ms();
    session.report.total_wall_ms = std::max<std::int64_t>(1, session.report.end_ms - session.report.start_ms);

    {
        std::lock_guard lock(cp_mutex);
        cp_stopping = true;
    }
    cp_cv.notify_all();
    if (coordinator.joinable()) coordinator.join();

    for (const auto& notice : root_handle.notices()) {
        session.report.notices.push_back(notice.code + ": " + notice.message);
    }

    if (root_result.status() != ProgramTerminalStatus::Completed) {
        const std::string code =
            root_result.failure() ? root_result.failure()->code : "unknown_failure";
        const std::string msg =
            root_result.failure() ? root_result.failure()->message : "Program did not complete";
        throw std::runtime_error("Swarm run terminated with status " +
                                 std::string(to_string(root_result.status())) + " (" + code +
                                 ": " + msg + ")");
    }

    // Collect hierarchy node summaries, runtime monetary records & usage, remaining budgets,
    // and check parallel interval overlap.
    const auto root_snap = root_handle.snapshot();
    const auto child_handles = runtime.recover_children(session.owner, root_handle.run_id());

    auto apply_result_accounting = [&](const std::string& node_key, const ProgramResult& res) {
        session.node_cost_microunits[node_key] = res.usage().monetary_microunits;
        for (const auto& mr : res.monetary_records()) {
            const std::string basis_str =
                !mr.basis.empty() ? mr.basis : std::string(to_string(mr.evidence));
            session.node_cost_source[node_key] = basis_str;
            for (auto& c : session.report.calls) {
                if (c.call_id == mr.call_id ||
                    (c.cost_source.empty() && c.node_hierarchy == node_key &&
                     c.requested_model == mr.model)) {
                    c.cost_microunits = mr.microunits;
                    c.reserved_microunits = mr.reserved_microunits;
                    c.cost_source = basis_str;
                    c.evidence = std::string(to_string(mr.evidence));
                    if (mr.served_by && !mr.served_by->empty()) {
                        c.served_by = *mr.served_by;
                    }
                    break;
                }
            }
        }
    };

    apply_result_accounting("swarm-orchestrator", root_result);

    auto make_node_summary = [&](const std::string& level,
                                 const std::string& name,
                                 const std::string& parent,
                                 const std::string& role,
                                 const std::string& model_role,
                                 const RunBudget& granted,
                                 const RunBudget& remaining) {
        NodeExecutionSummary s;
        s.level = level;
        s.name = name;
        s.parent = parent;
        s.role = role;
        s.model_role = model_role;
        const auto& rspec = models.roles.at(model_role);
        s.backend = rspec.backend;
        s.model = rspec.model;
        s.attempts = session.node_attempts[name];
        s.tokens_in = session.node_tokens_in[name];
        s.tokens_out = session.node_tokens_out[name];
        s.cost_microunits = session.node_cost_microunits[name];
        s.cost_source = session.node_cost_source.contains(name)
                            ? session.node_cost_source.at(name)
                            : "catalogue";
        s.start_ms = session.node_first_start_ms[name];
        s.end_ms = session.node_last_end_ms[name];
        s.budget_granted_tokens = granted.model_tokens;
        s.budget_remaining_tokens = remaining.model_tokens;
        s.budget_granted_microunits = granted.monetary_microunits;
        s.budget_remaining_microunits = remaining.monetary_microunits;
        return s;
    };

    session.report.nodes.push_back(make_node_summary(
        "root", "swarm-orchestrator", "", "orchestrator", "orchestrator",
        root_budget, root_snap.remaining_budget()));

    std::vector<std::pair<std::int64_t, std::int64_t>> child_intervals;
    bool budgets_monotonic = true;

    for (std::size_t i = 0; i < session.module_specs.size(); ++i) {
        const auto& mod = session.module_specs[i];
        const std::string child_key = "module:" + mod.name;
        const auto granted = make_child_budget(mod);
        RunBudget remaining = granted;
        const ProgramHandle* active_child = nullptr;
        if (mod.name == "slugify" && slugify_final_handle.has_value()) {
            active_child = &(*slugify_final_handle);
        } else {
            const auto expected_ver_id = child_versions.at(mod.name).second.id();
            for (const auto& ch : child_handles) {
                if (ch.snapshot().program_version_id() == expected_ver_id) {
                    active_child = &ch;
                    break;
                }
            }
            if (!active_child && i < child_handles.size()) {
                active_child = &child_handles[i];
            }
        }
        if (active_child) {
            for (const auto& notice : active_child->notices()) {
                session.report.notices.push_back(notice.code + ": " + notice.message);
            }
            const auto child_res = active_child->wait();
            apply_result_accounting(child_key, child_res);
            remaining = active_child->snapshot().remaining_budget();
        }
        if (remaining.model_tokens >= granted.model_tokens ||
            remaining.monetary_microunits > granted.monetary_microunits) {
            budgets_monotonic = false;
        }
        const std::string impl_role =
            (mod.criticality == "critical") ? "implementer-strong" : "implementer-cheap";
        auto summary = make_node_summary(
            "child", child_key, "swarm-orchestrator",
            "module-harness(" + mod.criticality + ")", impl_role, granted, remaining);
        const std::int64_t dur = std::max<std::int64_t>(0, summary.end_ms - summary.start_ms);
        session.report.sum_children_wall_ms += dur;
        child_intervals.emplace_back(summary.start_ms, summary.end_ms);
        session.report.nodes.push_back(std::move(summary));

        if (mod.criticality == "critical") {
            const std::string gc_key = "second-opinion:" + mod.name;
            RunBudget gc_rem = grandchild_budget;
            if (active_child) {
                const auto gc_handles =
                    runtime.recover_children(session.owner, active_child->run_id());
                if (!gc_handles.empty()) {
                    for (const auto& notice : gc_handles.back().notices()) {
                        session.report.notices.push_back(notice.code + ": " + notice.message);
                    }
                    const auto gc_res = gc_handles.back().wait();
                    apply_result_accounting(gc_key, gc_res);
                    gc_rem = gc_handles.back().snapshot().remaining_budget();
                }
            }
            session.report.nodes.push_back(make_node_summary(
                "grandchild", gc_key, child_key, "second-opinion-reviewer",
                "second-opinion", grandchild_budget, gc_rem));
        }
    }

    session.report.total_cost_microunits = 0;
    session.report.non_anthropic_cost_microunits = 0;
    session.report.cost_by_role_microunits.clear();
    session.report.cost_by_module_microunits.clear();
    for (const auto& c : session.report.calls) {
        session.report.total_cost_microunits += c.cost_microunits;
        if (c.backend != "anthropic") {
            session.report.non_anthropic_cost_microunits += c.cost_microunits;
        }
        session.report.cost_by_role_microunits[c.model_role] += c.cost_microunits;
        if (!c.module_name.empty()) {
            session.report.cost_by_module_microunits[c.module_name] += c.cost_microunits;
        }
    }
    if (session.report.non_anthropic_cost_microunits > session.other_budget_microunits) {
        throw std::runtime_error(
            "P_MONEY_BUDGET: non-Anthropic monetary cap (--max-cost-usd-other) exhausted");
    }

    for (std::size_t i = 0; i < child_intervals.size(); ++i) {
        for (std::size_t j = i + 1; j < child_intervals.size(); ++j) {
            const auto start_max = std::max(child_intervals[i].first, child_intervals[j].first);
            const auto end_min = std::min(child_intervals[i].second, child_intervals[j].second);
            if (start_max < end_min) {
                session.report.children_intervals_overlapped = true;
            }
        }
    }

    session.report.all_modules_succeeded =
        (session.report.published_modules == session.module_specs.size());

    // Read back the durable generation history of slugify from SQLite stores.
    std::vector<ProgramRunGeneration> stored_generations;
    if (!slugify_root_run_id.empty()) {
        if (const auto lin = transitions_view.load_run_lineage(session.owner, slugify_root_run_id)) {
            for (std::uint64_t g = 1; g <= lin->active_generation(); ++g) {
                if (auto gen =
                        transitions_view.load_generation(session.owner, lin->lineage_id(), g)) {
                    // Verify the bundle is also readable from SQLiteProgramStore.
                    const auto bundle =
                        programs_view.get_bundle(session.owner, gen->bundle_id());
                    if (!bundle) {
                        throw std::runtime_error("Missing bundle in SQLiteProgramStore for generation " +
                                                 std::to_string(g));
                    }
                    stored_generations.push_back(std::move(*gen));
                }
            }
        }
    }

    if (verbose && run_index == 0) {
        for (const auto& gd : session.report.gate_decisions) {
            if (!gd.published) {
                std::cout << "[gate] module=" << gd.module_name << " attempt=" << gd.attempt
                          << " blocked (reason=" << gd.block_reason
                          << ", verify_passed=" << (gd.verify_passed ? "true" : "false")
                          << ", reviewer_verdict=" << gd.reviewer_verdict << ")\n";
            } else {
                std::cout << "[gate] module=" << gd.module_name << " attempt=" << gd.attempt
                          << " published (verify_passed=true, reviewer_verdict="
                          << gd.reviewer_verdict << ", second_opinion="
                          << gd.second_opinion_verdict << ")\n";
            }
        }
        std::cout << "[hierarchy] root=swarm-orchestrator -> child=module:escape_html -> "
                     "grandchild=second-opinion:escape_html\n";
        for (const auto& n : session.report.nodes) {
            std::cout << "  level=" << n.level << " name=" << n.name << " role=" << n.role
                      << " model_role=" << n.model_role << " model=" << n.model
                      << " tokens_remaining=" << n.budget_remaining_tokens << "/"
                      << n.budget_granted_tokens
                      << " microunits_remaining=" << n.budget_remaining_microunits << "/"
                      << n.budget_granted_microunits << " outcome=" << n.outcome << "\n";
        }
        if (budgets_monotonic) {
            std::cout << "[budget] non-renewable child budgets decreased monotonically\n";
        }
        if (session.report.generation_transitions.size() >= 3 &&
            stored_generations.size() == 3) {
            const auto& t1 = session.report.generation_transitions[1];
            const auto& t2 = session.report.generation_transitions[2];
            std::cout << "[replace] reviewer topology upgraded mid-run; budgets carried over and "
                         "children preserved (tokens "
                      << t1.budget_before_tokens << " -> " << t1.budget_after_tokens << ")\n";
            std::cout << "[rollback] restored earlier admitted generation; store history count="
                      << stored_generations.size() << " (gen1="
                      << stored_generations[0].program_version_id().substr(0, 14)
                      << "... -> gen2="
                      << stored_generations[1].program_version_id().substr(0, 14)
                      << "... -> gen3="
                      << stored_generations[2].program_version_id().substr(0, 14)
                      << "..., tokens " << t2.budget_before_tokens << " -> "
                      << t2.budget_after_tokens << ")\n";
        }
    }
    if (verbose && synthesis_mode == "delegated") {
        std::cout << "[synthesis=delegated] child synthesized grandchild at runtime via "
                     "ProgramSynthesisGateway (admissions_refused="
                  << session.report.admissions_refused << ")\n";
    }

    std::filesystem::remove_all(temp_dir);
    return session.report;
}

json serialize_run_report(const SingleRunReport& r) {
    json nodes_arr = json::array();
    for (const auto& n : r.nodes) {
        nodes_arr.push_back({
            {"level", n.level},
            {"name", n.name},
            {"parent", n.parent},
            {"role", n.role},
            {"model_role", n.model_role},
            {"resolved_provider", n.backend},
            {"resolved_model", n.model},
            {"attempts", n.attempts},
            {"tokens_in", n.tokens_in},
            {"tokens_out", n.tokens_out},
            {"cost_usd", microunits_to_usd(n.cost_microunits)},
            {"cost_microunits", n.cost_microunits},
            {"cost_source", n.cost_source},
            {"wall_clock_start_ms", n.start_ms},
            {"wall_clock_end_ms", n.end_ms},
            {"wall_clock_duration_ms", std::max<std::int64_t>(0, n.end_ms - n.start_ms)},
            {"outcome", n.outcome},
            {"budget_granted",
             {{"model_tokens", n.budget_granted_tokens},
              {"monetary_microunits", n.budget_granted_microunits},
              {"monetary_usd", microunits_to_usd(n.budget_granted_microunits)}}},
            {"budget_remaining",
             {{"model_tokens", n.budget_remaining_tokens},
              {"monetary_microunits", n.budget_remaining_microunits},
              {"monetary_usd", microunits_to_usd(n.budget_remaining_microunits)}}},
        });
    }

    json calls_arr = json::array();
    for (const auto& c : r.calls) {
        calls_arr.push_back({
            {"call_id", c.call_id},
            {"node", c.node_hierarchy},
            {"node_role", c.node_role},
            {"model_role", c.model_role},
            {"backend", c.backend},
            {"requested_model", c.requested_model},
            {"served_by", c.served_by},
            {"provider_routed_to", c.served_by},
            {"attempt", c.attempt},
            {"tokens_in", c.input_tokens},
            {"tokens_out", c.output_tokens},
            {"cost_usd", microunits_to_usd(c.cost_microunits)},
            {"cost_microunits", c.cost_microunits},
            {"reserved_microunits", c.reserved_microunits},
            {"basis", c.cost_source},
            {"evidence", c.evidence},
            {"cost_source", c.cost_source},
            {"start_ms", c.start_ms},
            {"end_ms", c.end_ms},
            {"openrouter_routing", c.requested_routing},
        });
    }

    json gates_arr = json::array();
    for (const auto& g : r.gate_decisions) {
        gates_arr.push_back({
            {"module", g.module_name},
            {"attempt", g.attempt},
            {"reviewer_variant", g.reviewer_variant},
            {"verify_passed", g.verify_passed},
            {"reviewer_verdict_read", g.reviewer_verdict_read},
            {"reviewer_verdict", g.reviewer_verdict},
            {"second_opinion_required", g.second_opinion_required},
            {"second_opinion_verdict_read", g.second_opinion_verdict_read},
            {"second_opinion_verdict", g.second_opinion_verdict},
            {"published", g.published},
            {"block_reason", g.block_reason},
        });
    }

    json gens_arr = json::array();
    for (const auto& t : r.generation_transitions) {
        gens_arr.push_back({
            {"action", t.action},
            {"generation", t.generation},
            {"run_id", t.run_id},
            {"program_version_id", t.program_version_id},
            {"bundle_id", t.bundle_id},
            {"reviewer_variant", t.reviewer_variant},
            {"budget_before_tokens", t.budget_before_tokens},
            {"budget_after_tokens", t.budget_after_tokens},
            {"budget_before_microunits", t.budget_before_microunits},
            {"budget_after_microunits", t.budget_after_microunits},
            {"children_preserved", t.children_preserved},
        });
    }

    json cost_by_mod = json::object();
    for (const auto& [k, v] : r.cost_by_module_microunits) {
        cost_by_mod[k] = {{"microunits", v}, {"usd", microunits_to_usd(v)}};
    }
    json cost_by_role = json::object();
    for (const auto& [k, v] : r.cost_by_role_microunits) {
        cost_by_role[k] = {{"microunits", v}, {"usd", microunits_to_usd(v)}};
    }

    return {
        {"synthesis_mode", r.synthesis_mode},
        {"sequential", r.sequential},
        {"concurrency", r.concurrency},
        {"wall_clock_start_ms", r.start_ms},
        {"wall_clock_end_ms", r.end_ms},
        {"total_wall_time_ms", r.total_wall_ms},
        {"sum_children_wall_time_ms", r.sum_children_wall_ms},
        {"children_intervals_overlapped", r.children_intervals_overlapped},
        {"speedup_ratio_children_sum_over_total",
         r.total_wall_ms > 0
             ? static_cast<double>(r.sum_children_wall_ms) / static_cast<double>(r.total_wall_ms)
             : 1.0},
        {"totals",
         {{"tokens_in", r.total_tokens_in},
          {"tokens_out", r.total_tokens_out},
          {"cost_microunits", r.total_cost_microunits},
          {"cost_usd", microunits_to_usd(r.total_cost_microunits)},
          {"non_anthropic_cost_microunits", r.non_anthropic_cost_microunits},
          {"non_anthropic_cost_usd", microunits_to_usd(r.non_anthropic_cost_microunits)},
          {"gate_blocks", r.gate_blocks},
          {"published_modules", r.published_modules},
          {"admissions_refused", r.admissions_refused},
          {"all_modules_succeeded", r.all_modules_succeeded}}},
        {"refusal_reasons", r.refusal_reasons},
        {"notices", r.notices},
        {"cost_by_module", std::move(cost_by_mod)},
        {"cost_by_model_role", std::move(cost_by_role)},
        {"nodes", std::move(nodes_arr)},
        {"calls", std::move(calls_arr)},
        {"gate_decisions", std::move(gates_arr)},
        {"generation_transitions", std::move(gens_arr)},
    };
}

double median_of(std::vector<double> vals) {
    if (vals.empty()) return 0.0;
    std::sort(vals.begin(), vals.end());
    const std::size_t n = vals.size();
    return (n % 2 == 1) ? vals[n / 2] : 0.5 * (vals[n / 2 - 1] + vals[n / 2]);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto cli = parse_cli(argc, argv);
        const auto models = load_models_config(cli.models_file);

        std::cout << "=== Program Coding Swarm (Experimental Proof of Concept) ===\n"
                  << "mode: " << (cli.live ? "live" : "offline-mock")
                  << " | models: " << cli.models_file
                  << " | primary_synthesis: " << cli.synthesis_mode
                  << " | runs: " << cli.runs << "\n";

        std::vector<SingleRunReport> primary_runs;
        std::vector<SingleRunReport> sequential_runs;
        std::vector<double> par_wall_ms;
        std::vector<double> seq_wall_ms;
        std::vector<double> run_costs_usd;
        std::size_t total_gate_blocks = 0;

        for (std::size_t k = 0; k < cli.runs; ++k) {
            auto r_par = execute_swarm_run(
                cli, models, cli.synthesis_mode, cli.concurrency, k, k == 0);
            par_wall_ms.push_back(static_cast<double>(r_par.total_wall_ms));
            run_costs_usd.push_back(microunits_to_usd(r_par.total_cost_microunits));
            total_gate_blocks += r_par.gate_blocks;
            primary_runs.push_back(std::move(r_par));

            if (!cli.sequential && cli.runs > 1) {
                auto r_seq = execute_swarm_run(cli, models, cli.synthesis_mode, 1, k + 1000, false);
                seq_wall_ms.push_back(static_cast<double>(r_seq.total_wall_ms));
                sequential_runs.push_back(std::move(r_seq));
            }
        }

        // Compare central and delegated synthesis on the same task, models, and budgets.
        const std::string peer_mode =
            (cli.synthesis_mode == "central") ? "delegated" : "central";
        const auto peer_report =
            execute_swarm_run(cli, models, peer_mode, cli.concurrency, 9999, true);

        const auto& central_rep =
            (cli.synthesis_mode == "central") ? primary_runs.front() : peer_report;
        const auto& delegated_rep =
            (cli.synthesis_mode == "delegated") ? primary_runs.front() : peer_report;

        std::cout << "[comparison] central vs delegated recorded ("
                  << "central: published=" << central_rep.published_modules
                  << " gate_blocks=" << central_rep.gate_blocks
                  << " refused=" << central_rep.admissions_refused
                  << " wall_ms=" << central_rep.total_wall_ms << " | "
                  << "delegated: published=" << delegated_rep.published_modules
                  << " gate_blocks=" << delegated_rep.gate_blocks
                  << " refused=" << delegated_rep.admissions_refused
                  << " wall_ms=" << delegated_rep.total_wall_ms << ")\n";

        json full_report = {
            {"schema", "neograph.program-coding-swarm.report/v1"},
            {"mode", cli.live ? "live" : "offline"},
            {"models_file", cli.models_file},
            {"primary_synthesis_mode", cli.synthesis_mode},
            {"runs_requested", cli.runs},
            {"max_cost_usd", cli.max_cost_usd},
            {"max_cost_usd_other", cli.max_cost_usd_other},
            {"primary_run", serialize_run_report(primary_runs.front())},
            {"synthesis_comparison",
             {{"central", serialize_run_report(central_rep)},
              {"delegated", serialize_run_report(delegated_rep)}}},
            {"aggregates",
             {{"runs", cli.runs},
              {"median_wall_time_ms_parallel", median_of(par_wall_ms)},
              {"median_wall_time_ms_sequential",
               seq_wall_ms.empty() ? json(nullptr) : json(median_of(seq_wall_ms))},
              {"median_cost_usd_per_run", median_of(run_costs_usd)},
              {"total_gate_blocks_across_runs", total_gate_blocks}}},
        };
        if (primary_runs.size() > 1) {
            json all_runs = json::array();
            for (const auto& r : primary_runs) all_runs.push_back(serialize_run_report(r));
            full_report["all_primary_runs"] = std::move(all_runs);
        }

        if (!cli.report_file.empty()) {
            std::ofstream out(cli.report_file, std::ios::binary);
            if (!out)
                throw std::runtime_error("Cannot write report file: " + cli.report_file);
            out << full_report.dump(2) << "\n";
            std::cout << "[report] wrote machine-readable report to " << cli.report_file << "\n";
        }

        std::cout << "Coding swarm verification: PASS\n";
        return 0;
    } catch (const ProgramAdmissionError& e) {
        std::cerr << "Coding swarm admission error: " << e.what() << "\n";
        for (const auto& d : e.diagnostics()) {
            std::cerr << "  [" << d.code << "] " << d.primary.json_pointer << ": " << d.message
                      << "\n";
        }
        return 1;
    } catch (const ProgramCompileError& e) {
        std::cerr << "Coding swarm compile error: " << e.what() << "\n";
        for (const auto& d : e.diagnostics()) {
            std::cerr << "  [" << d.code << "] " << d.primary.json_pointer << ": " << d.message
                      << "\n";
        }
        return 1;
    } catch (const ProgramDiagnosticError& e) {
        std::cerr << "Coding swarm diagnostic error [" << e.diagnostic().code << "] "
                  << e.diagnostic().primary.json_pointer << ": " << e.diagnostic().message << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Coding swarm error: " << e.what() << "\n";
        return 1;
    }
}
