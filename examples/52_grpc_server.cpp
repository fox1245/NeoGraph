// NeoGraph Example 52: expose a GraphEngine over gRPC.
//
// Opt-in — build only with -DNEOGRAPH_BUILD_GRPC=ON (default OFF;
// needs grpc++ + protoc on the host: apt install libgrpc++-dev
// protobuf-compiler-grpc). A NeoGraph-native gRPC API, independent of
// the unratified MCP-over-gRPC transport.
//
// Run:   ./example_grpc_server            # listens on 0.0.0.0:50051
// Probe: grpcurl -plaintext localhost:50051 neograph.v1.GraphService/Health
//
// Then call RunGraph with a JSON graph_def + input from any gRPC
// client in any language — the C++ engine runs it, no Python anywhere.

#include <neograph/grpc/graph_service.h>

#include <cstdio>

#ifdef NEOGRAPH_HAVE_GRPC

#include <neograph/neograph.h>

using namespace neograph;
using namespace neograph::graph;

// A tiny default graph: one node uppercases the "text" channel.
class UpperNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        std::string s = in.state.get("text").get<std::string>();
        for (auto& c : s) c = static_cast<char>(std::toupper(c));
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"text", json(s)});
        co_return out;
    }
    std::string get_name() const override { return "upper"; }
};

int main(int argc, char** argv) {
    const std::string address =
        (argc > 1) ? argv[1] : "0.0.0.0:50051";

    NodeFactory::instance().register_type("upper",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<UpperNode>();
        });

    // Preloaded default graph — RunGraphRequest may omit graph_def_json
    // and the service reuses this. Clients can also send their own.
    const json default_graph = {
        {"name", "upper"},
        {"channels", {{"text", {{"reducer", "overwrite"}}}}},
        {"nodes",    {{"upper", {{"type", "upper"}}}}},
        {"edges", {
            {{"from", "__start__"}, {"to", "upper"}},
            {{"from", "upper"},     {"to", "__end__"}},
        }},
    };

    NodeContext ctx;
    std::printf("NeoGraph gRPC GraphService listening on %s\n",
                address.c_str());
    std::printf("  RunGraph / RunGraphStream / Health\n");

    // Blocks until the process is signalled.
    neograph::grpc::run_server(address, ctx, default_graph.dump());
    return 0;
}

#else   // NEOGRAPH_HAVE_GRPC not defined

int main() {
    std::printf(
        "example_grpc_server: built without gRPC support.\n"
        "Reconfigure with -DNEOGRAPH_BUILD_GRPC=ON "
        "(needs grpc++ + protoc) and rebuild.\n");
    return 0;
}

#endif
