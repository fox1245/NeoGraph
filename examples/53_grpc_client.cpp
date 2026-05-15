// NeoGraph Example 53: call a NeoGraph gRPC GraphService from C++.
//
// Pairs with example_grpc_server (52). Opt-in — build only with
// -DNEOGRAPH_BUILD_GRPC=ON.
//
//   Terminal 1:  ./example_grpc_server                 # 0.0.0.0:50051
//   Terminal 2:  ./example_grpc_client localhost:50051
//
// Demonstrates the polyglot story: this client speaks the same proto
// any Go/Rust/TS/Java client would — the C++ engine runs the graph,
// the caller never links NeoGraph itself.

#include <cstdio>
#include <cstdlib>

#ifdef NEOGRAPH_HAVE_GRPC

#include "neograph.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

namespace pb = neograph::v1;

int main(int argc, char** argv) {
    const std::string target = (argc > 1) ? argv[1] : "localhost:50051";

    auto channel = ::grpc::CreateChannel(
        target, ::grpc::InsecureChannelCredentials());
    auto stub = pb::GraphService::NewStub(channel);

    int failures = 0;

    // ── 1) Health ────────────────────────────────────────────────
    {
        ::grpc::ClientContext ctx;
        pb::HealthRequest  req;
        pb::HealthResponse resp;
        auto st = stub->Health(&ctx, req, &resp);
        if (!st.ok()) {
            std::printf("[Health] RPC failed: %s\n",
                        st.error_message().c_str());
            ++failures;
        } else {
            std::printf("[Health] ok=%d version=%s default_graph=%d\n",
                        resp.ok(), resp.version().c_str(),
                        resp.has_default_graph());
            if (!resp.ok()) ++failures;
        }
    }

    // ── 2) RunGraph against the server's preloaded default graph ──
    //    (upper-cases the "text" channel). graph_def_json omitted →
    //    server reuses its default.
    {
        ::grpc::ClientContext ctx;
        pb::RunGraphRequest  req;
        pb::RunGraphResponse resp;
        req.set_thread_id("t-client-1");
        req.set_input_json(R"({"text":"hello from grpc"})");
        auto st = stub->RunGraph(&ctx, req, &resp);
        if (!st.ok()) {
            std::printf("[RunGraph] RPC failed: %s\n",
                        st.error_message().c_str());
            ++failures;
        } else if (!resp.error().empty()) {
            std::printf("[RunGraph] engine error: %s\n",
                        resp.error().c_str());
            ++failures;
        } else {
            std::printf("[RunGraph] output_json=%s\n",
                        resp.output_json().c_str());
            std::printf("[RunGraph] trace=[");
            for (int i = 0; i < resp.execution_trace_size(); ++i)
                std::printf("%s%s", i ? "," : "",
                            resp.execution_trace(i).c_str());
            std::printf("]\n");
            // The default graph upper-cases → expect "HELLO FROM GRPC".
            if (resp.output_json().find("HELLO FROM GRPC")
                    == std::string::npos) {
                std::printf("[RunGraph] FAIL: expected upper-cased text "
                            "not found in output\n");
                ++failures;
            }
        }
    }

    // ── 3) RunGraphStream — stream events, expect a FINAL ─────────
    {
        ::grpc::ClientContext ctx;
        pb::RunGraphRequest req;
        req.set_thread_id("t-client-stream");
        req.set_input_json(R"({"text":"streamed"})");
        auto reader = stub->RunGraphStream(&ctx, req);

        pb::GraphEvent ev;
        int  events = 0;
        bool saw_final = false;
        while (reader->Read(&ev)) {
            ++events;
            if (ev.kind() == pb::GraphEvent::FINAL) {
                saw_final = true;
                std::printf("[Stream] FINAL payload=%s\n",
                            ev.payload_json().c_str());
            }
        }
        auto st = reader->Finish();
        std::printf("[Stream] events=%d final=%d status=%s\n",
                    events, saw_final,
                    st.ok() ? "OK" : st.error_message().c_str());
        if (!st.ok() || !saw_final) ++failures;
    }

    std::printf("\n%s (failures=%d)\n",
                failures ? "RESULT: FAIL" : "RESULT: PASS", failures);
    return failures ? 1 : 0;
}

#else   // NEOGRAPH_HAVE_GRPC not defined

int main() {
    std::printf("example_grpc_client: built without gRPC support. "
                "Reconfigure with -DNEOGRAPH_BUILD_GRPC=ON.\n");
    return 0;
}

#endif
