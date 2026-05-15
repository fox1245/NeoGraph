// NeoGraph Example 54: remote CheckpointStore over gRPC + honest
// overhead measurement.
//
// Opt-in — build only with -DNEOGRAPH_BUILD_GRPC=ON.
//
// Self-contained: spins a CheckpointService (InMemory backend) on a
// background thread, points a GrpcCheckpointStore at it, then measures:
//
//   1. in-process InMemory baseline (save + load_latest)
//   2. gRPC round-trip (save + load_latest) — the network overhead
//   3. payload size: JSON-in-proto vs a notional JSON-RPC envelope
//
// This is the measurement ROADMAP Candidate 7 demanded: turn the
// inherited-from-NexaGraph "PLAUSIBLE BUT UNPROVEN" overhead claim
// into numbers — *including the honest part*: our JSON-string-in-proto
// design (chosen for schema robustness) does NOT get protobuf's
// field-level wire compression. The gRPC win here is transport
// (HTTP/2 framing + connection reuse), not payload shrink.

#include <cstdio>

#ifdef NEOGRAPH_HAVE_GRPC

#include <neograph/grpc/grpc_checkpoint.h>
#include <neograph/neograph.h>

#include "neograph.grpc.pb.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace neograph::graph;
using clk = std::chrono::steady_clock;

// Build a realistically large checkpoint: a 1536-dim embedding (the
// NexaGraph rationale's worked example) plus a chunk of conversation
// history in channel_values.
static Checkpoint make_big_checkpoint(const std::string& thread_id) {
    Checkpoint cp;
    cp.id        = Checkpoint::generate_id();
    cp.thread_id = thread_id;
    cp.parent_id = "";
    cp.current_node = "responder";
    cp.next_nodes   = {"critic", "summarizer"};
    cp.interrupt_phase = CheckpointPhase::Completed;
    cp.barrier_state["join"] = {"branch_a", "branch_b"};
    cp.step      = 7;
    cp.timestamp = 1747000000000LL;

    neograph::json embedding = neograph::json::array();
    for (int i = 0; i < 1536; ++i)
        embedding.push_back(0.0001 * i - 0.07);

    neograph::json msgs = neograph::json::array();
    for (int i = 0; i < 12; ++i)
        msgs.push_back({{"role", i % 2 ? "assistant" : "user"},
                        {"content",
                         "Turn " + std::to_string(i) +
                         " — a representative chat message of moderate "
                         "length so the checkpoint payload looks like a "
                         "real multi-turn session, not a toy."}});

    cp.channel_values = {
        {"channels", {
            {"messages",  {{"value", msgs},      {"version", 12}}},
            {"embedding", {{"value", embedding}, {"version", 1}}},
        }},
        {"global_version", 13},
    };
    cp.channel_versions = {{"messages", 12}, {"embedding", 1}};
    cp.metadata = {{"source", "example_54"}};
    return cp;
}

template <typename F>
static double time_us(int iters, F&& f) {
    auto t0 = clk::now();
    for (int i = 0; i < iters; ++i) f();
    auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  clk::now() - t0).count();
    return (dt / 1000.0) / iters;   // µs / iter
}

int main() {
    const std::string addr = "127.0.0.1:50081";
    const int ITERS = 200;

    // ── Server: InMemory backend behind a gRPC CheckpointService ────
    auto backend = std::make_shared<InMemoryCheckpointStore>();
    std::thread srv([&] {
        neograph::grpc::run_checkpoint_server(addr, backend);
    });
    srv.detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    auto cp = make_big_checkpoint("t-bench");

    // ── Payload sizes ───────────────────────────────────────────────
    std::string cp_json = neograph::grpc::checkpoint_to_json(cp);
    neograph::v1::CheckpointBlob blob;
    blob.set_id(cp.id);
    blob.set_thread_id(cp.thread_id);
    blob.set_checkpoint_json(cp_json);
    std::string proto_wire = blob.SerializeAsString();

    // Notional JSON-RPC 2.0 envelope for the same save (what an
    // HTTP/JSON-RPC checkpoint transport would put on the wire).
    std::string jsonrpc_env =
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"saveCheckpoint",)") +
        R"("params":{"checkpoint":)" + cp_json + "}}";

    // ── In-process InMemory baseline ────────────────────────────────
    double base_save = time_us(ITERS, [&]{ backend->save(cp); });
    double base_load = time_us(ITERS, [&]{
        (void)backend->load_latest("t-bench"); });

    // ── gRPC round-trip ─────────────────────────────────────────────
    neograph::grpc::GrpcCheckpointStore store(addr);
    store.save(cp);  // warm the channel
    double g_save = time_us(ITERS, [&]{ store.save(cp); });
    double g_load = time_us(ITERS, [&]{
        (void)store.load_latest("t-bench"); });

    // Correctness: round-trip must preserve the rich fields NexaGraph's
    // flat mapping didn't carry (next_nodes vector, enum, barrier_state).
    auto rt = store.load_latest("t-bench");
    bool ok = rt
        && rt->next_nodes.size() == 2
        && rt->interrupt_phase == CheckpointPhase::Completed
        && rt->barrier_state.count("join") == 1
        && rt->channel_values["channels"]["embedding"]["value"].size() == 1536;

    std::printf("\n=== gRPC CheckpointStore — honest measurement ===\n");
    std::printf("Checkpoint: 1536-d embedding + 12-turn history\n\n");

    std::printf("--- Payload size (same checkpoint) ---\n");
    std::printf("JSON (checkpoint_json):        %8zu bytes\n", cp_json.size());
    std::printf("Protobuf wire (CheckpointBlob):%8zu bytes\n", proto_wire.size());
    std::printf("Notional JSON-RPC envelope:    %8zu bytes\n", jsonrpc_env.size());
    std::printf("  → protobuf vs JSON-RPC: %.1f%% of the bytes\n",
                100.0 * proto_wire.size() / jsonrpc_env.size());
    std::printf("  NOTE: ~no payload shrink — checkpoint_json rides\n"
                "  inside the proto as a string (schema-robust design).\n"
                "  The gRPC win below is TRANSPORT, not compression.\n\n");

    std::printf("--- Latency (%d iters, µs/op) ---\n", ITERS);
    std::printf("InMemory in-process  save=%.2f  load=%.2f\n",
                base_save, base_load);
    std::printf("gRPC round-trip      save=%.2f  load=%.2f\n",
                g_save, g_load);
    std::printf("  → gRPC network overhead: +%.2f µs save / +%.2f µs load\n",
                g_save - base_save, g_load - base_load);
    std::printf("  (localhost loopback; real network adds RTT, but\n"
                "   HTTP/2 reuses one connection vs JSON-RPC/HTTP1.1\n"
                "   per-call connect — that delta grows under load.)\n\n");

    std::printf("Round-trip correctness (rich fields preserved): %s\n",
                ok ? "PASS" : "FAIL");
    std::printf("\n%s\n", ok ? "RESULT: PASS" : "RESULT: FAIL");
    return ok ? 0 : 1;
}

#else

int main() {
    std::printf("example_grpc_checkpoint: built without gRPC. "
                "Reconfigure with -DNEOGRAPH_BUILD_GRPC=ON.\n");
    return 0;
}

#endif
