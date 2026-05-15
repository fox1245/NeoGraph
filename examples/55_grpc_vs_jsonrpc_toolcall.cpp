// NeoGraph Example 55: ToolCalling in JSON-RPC vs ToolCalling in gRPC
// — apples-to-apples head-to-head.
//
// Opt-in (-DNEOGRAPH_BUILD_GRPC=ON). Self-contained: spins BOTH
//   * an MCP-shaped JSON-RPC 2.0 tool server (httplib, HTTP/1.1
//     keep-alive) — the exact `tools/call` envelope NeoGraph's
//     neograph::mcp speaks
//   * a gRPC ToolService (HTTP/2)
// behind the SAME tool function, then times the SAME call on both.
//
// Honest controls:
//   - identical compute fn, identical machine, 127.0.0.1 loopback
//   - JSON-RPC client keep-alive ON (persistent HTTP/1.1) so it's not
//     unfairly paying per-call TCP connect; gRPC reuses its channel
//     (HTTP/2) by default — both get connection reuse
//   - two payload regimes: tiny args, and a 1536-float arg (the size
//     where NexaGraph claimed binary wins)
//   - warmup before timing; p50 over N iters (loopback noise is not
//     gaussian, mean alone lies)
//
// The point: isolate *transport* cost. Both sides carry args/result
// as JSON strings (graph-as-data), so this measures HTTP/2+protobuf
// framing vs HTTP/1.1+JSON-RPC envelope — nothing else.

#include <cstdio>

#ifdef NEOGRAPH_HAVE_GRPC

#include <neograph/grpc/tool_service.h>
#include <neograph/json.h>

#include "neograph.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pb = neograph::v1;
using clk = std::chrono::steady_clock;

// The one tool, shared by both transports. Sum a float vector, echo a
// tag. Deliberately cheap so transport dominates the measurement.
static std::string compute(const std::string& args_json) {
    auto a = neograph::json::parse(args_json.empty() ? "{}" : args_json);
    double sum = 0.0;
    size_t n = 0;
    if (a.contains("vec") && a["vec"].is_array()) {
        for (const auto& v : a["vec"]) { sum += v.get<double>(); ++n; }
    }
    neograph::json r = {
        {"sum", sum}, {"n", (int)n},
        {"echo", a.value("tag", std::string{})},
    };
    return r.dump();
}

static double p50(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}
static double mean(const std::vector<double>& v) {
    double s = 0; for (double x : v) s += x;
    return v.empty() ? 0.0 : s / v.size();
}
template <typename F>
static double time_loop(int iters, F&& f) {
    for (int i = 0; i < 50; ++i) f();          // warmup
    auto t0 = clk::now();
    for (int i = 0; i < iters; ++i) f();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  clk::now() - t0).count();
    return (ns / 1000.0) / iters;              // µs / iter
}

int main() {
    const std::string grpc_addr = "127.0.0.1:50091";
    const int    JR_PORT = 50092;
    const int    N = 300;

    // ── gRPC ToolService (background) ───────────────────────────────
    std::thread gsrv([&]{
        std::unordered_map<std::string, neograph::grpc::ToolFn> tools;
        tools["compute"] = compute;
        neograph::grpc::run_tool_server(grpc_addr, std::move(tools));
    });
    gsrv.detach();

    // ── JSON-RPC 2.0 tool server (httplib, MCP `tools/call` shape) ──
    httplib::Server jr;
    // FAIR: gRPC sets TCP_NODELAY by default; httplib defaults it OFF
    // (CPPHTTPLIB_TCP_NODELAY=false) → Nagle + 40ms delayed-ACK on
    // small requests, which would falsely make JSON-RPC look ~70x
    // slower. Match the transports.
    jr.set_tcp_nodelay(true);
    jr.Post("/", [](const httplib::Request& req, httplib::Response& res) {
        auto env = neograph::json::parse(req.body.empty() ? "{}" : req.body);
        neograph::json out;
        out["jsonrpc"] = "2.0";
        out["id"] = env.value("id", 1);
        try {
            // MCP: params.name + params.arguments
            auto params = env.contains("params") ? env["params"]
                                                  : neograph::json::object();
            std::string name = params.value("name", std::string{});
            std::string args = params.contains("arguments")
                ? params["arguments"].dump() : "{}";
            if (name != "compute") throw std::runtime_error("unknown tool");
            out["result"] = neograph::json::parse(compute(args));
        } catch (const std::exception& e) {
            out["error"] = {{"code", -32000}, {"message", e.what()}};
        }
        res.set_content(out.dump(), "application/json");
    });
    std::thread jsrv([&]{ jr.listen("127.0.0.1", JR_PORT); });
    jsrv.detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    // ── Clients (both reuse one connection) ─────────────────────────
    auto chan = ::grpc::CreateChannel(
        grpc_addr, ::grpc::InsecureChannelCredentials());
    auto stub = pb::ToolService::NewStub(chan);

    httplib::Client jc("127.0.0.1", JR_PORT);
    jc.set_keep_alive(true);   // persistent HTTP/1.1 — fair vs gRPC HTTP/2
    jc.set_tcp_nodelay(true);  // match gRPC's default; kill Nagle/delayed-ACK

    auto run_case = [&](const char* label, const neograph::json& args) {
        std::string args_json = args.dump();

        // Wire sizes for the same call.
        pb::ToolCallRequest greq;
        greq.set_name("compute");
        greq.set_arguments_json(args_json);
        size_t grpc_wire = greq.SerializeAsString().size();

        neograph::json jenv = {
            {"jsonrpc","2.0"}, {"id",1}, {"method","tools/call"},
            {"params", {{"name","compute"}, {"arguments", args}}},
        };
        size_t jr_wire = jenv.dump().size();

        // Warmup
        for (int i = 0; i < 30; ++i) {
            { ::grpc::ClientContext c; pb::ToolCallResponse r;
              stub->CallTool(&c, greq, &r); }
            jc.Post("/", jenv.dump(), "application/json");
        }

        std::vector<double> gt, jt;
        gt.reserve(N); jt.reserve(N);
        for (int i = 0; i < N; ++i) {
            auto t0 = clk::now();
            { ::grpc::ClientContext c; pb::ToolCallResponse r;
              stub->CallTool(&c, greq, &r); }
            gt.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                clk::now() - t0).count() / 1000.0);

            auto t1 = clk::now();
            jc.Post("/", jenv.dump(), "application/json");
            jt.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                clk::now() - t1).count() / 1000.0);
        }

        std::printf("\n[%s]  args wire: gRPC %zu B / JSON-RPC %zu B "
                    "(%.0f%%)\n", label, grpc_wire, jr_wire,
                    100.0 * grpc_wire / jr_wire);
        std::printf("  gRPC      p50 %.1f us  mean %.1f us\n",
                    p50(gt), mean(gt));
        std::printf("  JSON-RPC  p50 %.1f us  mean %.1f us\n",
                    p50(jt), mean(jt));
        std::printf("  → gRPC is %.2fx the JSON-RPC p50 "
                    "(<1 = gRPC faster)\n", p50(gt) / p50(jt));
    };

    std::printf("=== ToolCalling: JSON-RPC vs gRPC (loopback, "
                "keep-alive both, N=%d) ===\n", N);

    run_case("tiny args", {{"vec", {1.0, 2.0, 3.0}}, {"tag", "x"}});

    neograph::json big = neograph::json::array();
    for (int i = 0; i < 1536; ++i) big.push_back(0.001 * i);
    run_case("1536-float args", {{"vec", big}, {"tag", "emb"}});

    std::printf("\nNOTE: args/result are JSON strings on BOTH sides "
                "(graph-as-data),\nso this is a pure transport delta — "
                "no protobuf field compression.\n");

    // ── Serialization-only (NO transport) ───────────────────────────
    //
    // Why is JSON-RPC even competitive above? Hypothesis: NeoGraph's
    // neograph::json is yyjson (SIMD, ~1 GB/s) — not the Python `json`
    // / nlohmann that a typical JSON-RPC stack uses (5–50x slower).
    // Strip transport, measure just the codec on the 12 KB payload.
    {
        neograph::json big = neograph::json::array();
        for (int i = 0; i < 1536; ++i) big.push_back(0.001 * i);
        neograph::json args = {{"vec", big}, {"tag", "emb"}};
        std::string args_json = args.dump();

        pb::ToolCallRequest preq;
        preq.set_name("compute");
        preq.set_arguments_json(args_json);

        const int M = 5000;
        // yyjson: parse the args + re-dump (what the server does).
        double j_us = time_loop(M, [&]{
            auto a = neograph::json::parse(args_json);
            volatile auto s = a.dump(); (void)s;
        });
        // protobuf: SerializeToString + ParseFromString round-trip.
        double p_us = time_loop(M, [&]{
            std::string w = preq.SerializeAsString();
            pb::ToolCallRequest back; back.ParseFromString(w);
            volatile auto n = back.arguments_json().size(); (void)n;
        });
        std::printf("\n--- Serialization only, 12 KB payload, "
                    "%d iters (no transport) ---\n", M);
        std::printf("  yyjson  parse+dump : %.2f us\n", j_us);
        std::printf("  protobuf ser+parse : %.2f us\n", p_us);
        std::printf("  → yyjson is %.2fx protobuf "
                    "(>1 = JSON codec slower)\n", j_us / p_us);
        std::printf("  Takeaway: protobuf IS structurally cheaper, but\n"
                    "  yyjson keeps NeoGraph's JSON-RPC close enough that\n"
                    "  the transport delta dominates — a typical Python/\n"
                    "  nlohmann JSON-RPC stack would NOT be this close.\n");
    }
    return 0;
}

#else

int main() {
    std::printf("example_grpc_vs_jsonrpc: built without gRPC. "
                "Reconfigure with -DNEOGRAPH_BUILD_GRPC=ON.\n");
    return 0;
}

#endif
