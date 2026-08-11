// The smallest program that proves an installed NeoGraph is actually usable:
// include a public header, construct engine types, run a graph, print the result.
//
// It deliberately touches a header that pulls in the vendored asio
// (graph/engine.h ships asio::awaitable through the coroutine surface) and one
// that pulls in the vendored yyjson (json.h). Those are the dependencies that
// leak through NeoGraph's public API, so a consumer cannot compile unless the
// install ships them and the exported targets point at them. Linking alone is
// not enough — this has to *compile*.

#include <neograph/neograph.h>
#include <neograph/async/run_sync.h>
#ifdef NEOGRAPH_CONSUMER_HAS_MCP_SQLITE
#include <neograph/mcp/sqlite_harness_store.h>
#endif
#ifdef NEOGRAPH_CONSUMER_HAS_A2A
#include <neograph/a2a/collaboration.h>
#include <neograph/a2a/types.h>
#include <neograph/a2a/agent_card_candidate.h>
#endif
#ifdef NEOGRAPH_CONSUMER_HAS_ACP
#include <neograph/acp/types.h>
#endif

#include <cstdlib>
#include <iostream>

namespace {

class InstalledProvider final : public neograph::CompletionProvider {
  public:
    std::string get_name() const override { return "installed"; }

  protected:
    asio::awaitable<neograph::ChatCompletion>
    do_invoke(neograph::CompletionRequest request) override {
        neograph::ChatCompletion result;
        result.message.role = "assistant";
        result.message.content = request.params().model;
        co_return result;
    }
};

} // namespace

int main() {
    using namespace neograph;
    using namespace neograph::graph;

    InstalledProvider provider;
    CompletionParams params;
    params.model = "installed-provider";
    const auto completion = neograph::async::run_sync(provider.invoke_request(
        CompletionRequest::collect(std::move(params))));
    if (completion.message.content != "installed-provider") {
        std::cerr << "installed CompletionProvider dispatch failed\n";
        return EXIT_FAILURE;
    }

    GraphState state;
    state.init_channel("greeting", ReducerType::OVERWRITE,
                       ReducerRegistry::instance().get("overwrite"), json(""));
    state.write("greeting", json("hello from an installed NeoGraph"));

    const auto value = state.get("greeting").get<std::string>();
    std::cout << value << "\n";

    if (value != "hello from an installed NeoGraph") {
        std::cerr << "unexpected channel value\n";
        return EXIT_FAILURE;
    }

#ifdef NEOGRAPH_CONSUMER_HAS_MCP_SQLITE
    neograph::mcp::SqliteHarnessRecordStore records(":memory:");
    records.save_artifact("artifact_installed", {
        {"artifact_id", "artifact_installed"},
        {"request", json::object()},
    });
    records.save_run("run_installed", {
        {"run_id", "run_installed"},
        {"artifact_id", "artifact_installed"},
        {"status", "completed"},
    });
    if (records.load_run("run_installed")->value("status", "") != "completed") {
        std::cerr << "installed SqliteHarnessRecordStore failed\n";
        return EXIT_FAILURE;
    }
#endif

#ifdef NEOGRAPH_CONSUMER_HAS_A2A
    neograph::a2a::CollaborationLinkSpec link_spec;
    if (link_spec.schema_version != 2 ||
        neograph::a2a::CollaborationLink::STORAGE_SCHEMA_VERSION != 2) {
        std::cerr << "installed A2A collaboration schema is stale\n";
        return EXIT_FAILURE;
    }
    const auto a2a_part = neograph::a2a::Part::text_part("installed-a2a");
    if (a2a_part.text != "installed-a2a") {
        std::cerr << "installed A2A type surface failed\n";
        return EXIT_FAILURE;
    }
    // Constructing the collector proves the installed A2A candidate surface
    // resolves its exported implementation without making any network call.
    neograph::a2a::AgentCardCollector candidate_collector;
    (void)candidate_collector;
#endif

#ifdef NEOGRAPH_CONSUMER_HAS_ACP
    neograph::acp::InitializeRequest acp_request;
    const auto acp_part = neograph::acp::ContentBlock::text_block("installed-acp");
    if (acp_request.protocol_version != 1 || acp_part.text != "installed-acp") {
        std::cerr << "installed ACP type surface failed\n";
        return EXIT_FAILURE;
    }
#endif
    return EXIT_SUCCESS;
}
