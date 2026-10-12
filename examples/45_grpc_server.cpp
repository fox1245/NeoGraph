// NeoGraph Example 45: serve a GraphEngine over gRPC
//
// A NeoGraph-native gRPC API (neograph.v1.GraphService: Health, RunGraph, RunGraphStream),
// independent of any MCP-over-gRPC transport. Clients in any language call RunGraph with a graph
// definition and an input; the C++ engine runs it and nothing else needs to link NeoGraph.
//
// Opt-in: build with -DNEOGRAPH_BUILD_GRPC=ON (needs grpc++ and protoc, for example
// `apt install libgrpc++-dev protobuf-compiler-grpc`).
//
// Usage:
//   ./example_grpc_server --self-test            # server and client in this process, free port
//   ./example_grpc_server 127.0.0.1:50051        # serve until stopped; example 53 is the client
//   grpcurl -plaintext 127.0.0.1:50051 neograph.v1.GraphService/Health
//
// smoke-args: --self-test
// smoke-expect: [Health] ok=1
// smoke-expect: [RunGraph] text=HELLO FROM GRPC
// smoke-expect: RESULT: PASS
// smoke-forbid: [neograph] lint

#include <neograph/grpc/graph_service.h>

#include <cstdio>

#ifdef NEOGRAPH_HAVE_GRPC

#include <neograph/neograph.h>

#include <grpcpp/grpcpp.h>

#include <cstring>
#include <memory>
#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

// The default graph: one node upper-cases the "text" channel.
class UpperNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        std::string text = in.state.get("text").get<std::string>();
        for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"text", json(text)});
        co_return out;
    }
    std::string get_name() const override { return "upper"; }
};

// The client half of the self-test: what a Go, Rust or Python client would do with the same proto.
int call_server(const std::string& target) {
    auto stub = neograph::v1::GraphService::NewStub(
        ::grpc::CreateChannel(target, ::grpc::InsecureChannelCredentials()));
    int failures = 0;

    {
        ::grpc::ClientContext context;
        neograph::v1::HealthRequest request;
        neograph::v1::HealthResponse reply;
        const auto status = stub->Health(&context, request, &reply);
        std::printf("[Health] ok=%d default_graph=%d\n", status.ok() && reply.ok(), reply.has_default_graph());
        if (!status.ok() || !reply.ok()) ++failures;
    }
    {
        // graph_def_json is left out, so the server runs its default graph.
        ::grpc::ClientContext context;
        neograph::v1::RunGraphRequest request;
        neograph::v1::RunGraphResponse reply;
        request.set_thread_id("client-1");
        request.set_input_json(R"({"text":"hello from grpc"})");
        const auto status = stub->RunGraph(&context, request, &reply);
        std::string text = "(failed: " + status.error_message() + ")";
        if (status.ok()) {
            const auto output = json::parse(reply.output_json());
            text = output["channels"]["text"]["value"].get<std::string>();
        }
        std::printf("[RunGraph] text=%s\n", text.c_str());
        if (text != "HELLO FROM GRPC") ++failures;
    }
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    bool self_test = false;
    std::string address = "127.0.0.1:50051";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) self_test = true;
        else address = argv[i];
    }
    if (self_test) address = "127.0.0.1:0";  // the OS picks a free port

    NodeFactory::instance().register_type("upper", [](const std::string&, const json&, const NodeContext&) {
        return std::make_unique<UpperNode>();
    });
    const json default_graph = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "upper"},
        {"channels", {{"text", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"upper", {{"type", "upper"}}}}},
        {"edges", {{{"from", "__start__"}, {"to", "upper"}}, {{"from", "upper"}, {"to", "__end__"}}}},
    };

    // make_graph_service gives the service object; the ServerBuilder is ours, so the port,
    // credentials and limits are ours too.
    auto service = neograph::grpc::make_graph_service(NodeContext{}, default_graph.dump());
    ::grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort(address, ::grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(service.get());
    auto server = builder.BuildAndStart();
    if (!server || port == 0) {
        std::printf("could not listen on %s\n", address.c_str());
        return 1;
    }
    const std::string target = address.substr(0, address.rfind(':')) + ":" + std::to_string(port);
    std::printf("NeoGraph GraphService listening on %s\n", target.c_str());

    if (!self_test) {
        server->Wait();  // until the process is signalled
        return 0;
    }
    const int failures = call_server(target);
    server->Shutdown();
    std::printf("RESULT: %s (failures=%d)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

#else   // NEOGRAPH_HAVE_GRPC not defined

int main() {
    std::printf("example_grpc_server: built without gRPC support.\n"
                "Reconfigure with -DNEOGRAPH_BUILD_GRPC=ON (needs grpc++ + protoc) and rebuild.\n");
    return 0;
}

#endif
