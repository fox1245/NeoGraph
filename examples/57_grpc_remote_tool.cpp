// NeoGraph Example 57: a remote gRPC tool as a local neograph::Tool
//
// The mirror of example 55. There we SERVED tools over gRPC; here we
// CONSUME one. neograph::grpc::GrpcRemoteTool wraps a remote
// ToolService.CallTool method as an ordinary neograph::Tool, so an
// Agent/ReAct loop calls it exactly like a local tool — the tool's
// real work runs in another process (or language) with no change on
// the agent side. Ported from NexaGraph's GrpcTool adapter.
//
// Opt-in (-DNEOGRAPH_BUILD_GRPC=ON). Self-contained: spins the gRPC
// ToolService in a background thread, then drives a GrpcRemoteTool
// through the plain neograph::Tool interface.

#include <cstdio>

#ifdef NEOGRAPH_HAVE_GRPC

#include <neograph/grpc/tool_service.h>
#include <neograph/json.h>
#include <neograph/tool.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

int main() {
    const std::string addr = "127.0.0.1:50097";

    // ── Server: one tool, "compute" = sum a vector, echo a tag. The
    //    "fail" arg makes it throw, to show error propagation. ────────
    std::thread srv([&] {
        std::unordered_map<std::string, neograph::grpc::ToolFn> tools;
        tools["compute"] = [](const std::string& args_json) {
            auto a = neograph::json::parse(
                args_json.empty() ? "{}" : args_json);
            if (a.value("fail", false))
                throw std::runtime_error("compute: forced failure");
            double sum = 0;
            if (a.contains("vec") && a["vec"].is_array())
                for (const auto& v : a["vec"]) sum += v.get<double>();
            neograph::json r = {{"sum", sum},
                                {"echo", a.value("tag", std::string{})}};
            return r.dump();
        };
        neograph::grpc::run_tool_server(addr, std::move(tools));
    });
    srv.detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // ── Client: wrap the remote method as a neograph::Tool ───────────
    auto tool = std::make_unique<neograph::grpc::GrpcRemoteTool>(
        addr, "compute",
        "Sum a numeric vector and echo a tag.",
        neograph::json{
            {"type", "object"},
            {"properties",
             {{"vec", {{"type", "array"},
                       {"items", {{"type", "number"}}}}},
              {"tag", {{"type", "string"}}}}}});

    // It's just a neograph::Tool — exactly what an Agent holds.
    std::vector<std::unique_ptr<neograph::Tool>> agent_tools;
    agent_tools.push_back(std::move(tool));
    neograph::Tool& t = *agent_tools.front();

    auto def = t.get_definition();
    std::printf("=== GrpcRemoteTool as neograph::Tool ===\n");
    std::printf("name        : %s\n", t.get_name().c_str());
    std::printf("description : %s\n", def.description.c_str());
    std::printf("params      : %s\n\n", def.parameters.dump().c_str());

    // Normal call — work happens in the server thread, over gRPC.
    neograph::json args = {{"vec", {1.5, 2.5, 4.0}}, {"tag", "demo"}};
    std::printf("execute(%s)\n  → %s\n\n", args.dump().c_str(),
                t.execute(args).c_str());

    // Remote tool error → rethrown as std::runtime_error (a tool
    // error, NOT a transport error — same contract as a local Tool).
    try {
        t.execute(neograph::json{{"fail", true}});
    } catch (const std::exception& e) {
        std::printf("execute({\"fail\":true}) threw as expected:\n"
                    "  %s\n", e.what());
    }

    std::printf("\nTakeaway: a process-boundary tool is indistinguishable "
                "from a local\none at the Agent's call site — polyglot "
                "tools, zero agent-side change.\n");
    return 0;
}

#else

int main() {
    std::printf("example_grpc_remote_tool: built without gRPC. "
                "Reconfigure with -DNEOGRAPH_BUILD_GRPC=ON.\n");
    return 0;
}

#endif
