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
#include <descriptor/descriptor.h>
#include <json/json.h>
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
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

class InstalledProvider final : public neograph::Provider {
    struct State {
        std::string request_digest;
        std::shared_ptr<sp::runtime::Client> client;
    };
public:
    InstalledProvider() : state_(std::make_shared<State>()) {
        auto admitted = sp::descriptor::load(R"({
            "descriptor_version":1,"revision":1,"id":"installed-local",
            "family":"openai.chat","connection":{"base_url":"https://fixture.invalid",
            "paths":{"buffered":"/v1/chat/completions","streaming":"/v1/chat/completions"}}})");
        if (!std::holds_alternative<sp::descriptor::ValidatedDescriptor>(admitted))
            throw std::runtime_error("installed descriptor admission failed");
        state_->client = std::make_shared<sp::runtime::Client>(
            std::get<sp::descriptor::ValidatedDescriptor>(std::move(admitted)));
    }
    std::string get_name() const override { return "installed"; }
    std::string_view family() const noexcept override { return "openai.chat"; }
    neograph::PreparedProviderRequest prepare(neograph::ProviderRequest request) override {
        return prepare_local(state_->client, std::move(request),
            [state = state_](const neograph::PreparedProviderRequest& prepared,
                             const std::function<void(const sp::Event&)>& observer)
                             -> asio::awaitable<sp::runtime::Result> {
                const auto body = neograph::json::parse(prepared.encoded_body());
                if (body.at("model") != "installed-provider" ||
                    body.at("messages").at(0).at("content").at(0).at("text") != "review installation" ||
                    prepared.mode() != neograph::ProviderMode::Stream ||
                    state->request_digest != neograph::Provider::request_digest(prepared))
                    throw std::runtime_error("installed prepared request binding changed");
                auto parsed = sp::json::parse(R"({"package":"NeoGraph"})");
                auto input = std::make_shared<const sp::json::Document>(
                    std::get<sp::json::Document>(std::move(parsed)));
                sp::Completion completion;
                completion.messages.push_back(sp::Message{"installed-message", sp::Role::Assistant,
                    {sp::Text{"installed result"}, sp::Refusal{"preserved refusal", "policy"},
                     sp::ToolCall{"installed-call", "inspect.package",
                                  sp::ToolCallKind::ClientExecuted, std::move(input)}}});
                completion.stop = {sp::StopKind::ToolUse, "tool_calls"};
                completion.usage.input_total = sp::Count{9};
                completion.usage.output_total = sp::Count{0};
                completion.usage.total = sp::Count{9, sp::Evidence::Derived};
                completion.usage.stage = sp::UsageStage::Final;
                if (observer) {
                    observer(sp::Begin{"installed-generation"});
                    observer(sp::UsageUpdate{completion.usage});
                    observer(sp::Stop{completion.stop});
                }
                co_return std::make_shared<const sp::Outcome>(std::move(completion));
            });
    }
    void pin(const neograph::PreparedProviderRequest& prepared) {
        state_->request_digest = request_digest(prepared);
    }
private:
    std::shared_ptr<State> state_;
};

} // namespace

int main() {
    using namespace neograph;
    using namespace neograph::graph;

    auto provider = std::make_unique<InstalledProvider>();
    ProviderControls controls;
    controls.max_output_tokens = 17;
    auto request = make_provider_request(*provider, "installed-provider",
        {sp::Message{"installed-user", sp::Role::User, {sp::Text{"review installation"}}}},
        {}, controls, ProviderMode::Stream);
    std::vector<std::size_t> event_kinds;
    request.on_event = [&event_kinds](const sp::Event& event) { event_kinds.push_back(event.index()); };
    auto prepared = provider->prepare(std::move(request));
    if (!prepared.valid()) {
        std::cerr << "installed local preparation failed\n";
        return EXIT_FAILURE;
    }
    provider->pin(prepared);
    auto operation = provider->dispatch_async(std::move(prepared));
    provider.reset();
    const auto result = neograph::async::run_sync(std::move(operation));
    if (!result || !std::holds_alternative<sp::Completion>(*result)) return EXIT_FAILURE;
    const auto& completion = std::get<sp::Completion>(*result);
    const auto& parts = completion.messages.at(0).parts;
    if (parts.size() != 3 || std::get<sp::Text>(parts[0]).value != "installed result" ||
        std::get<sp::Refusal>(parts[1]).raw_code != "policy" ||
        std::get<sp::ToolCall>(parts[2]).name != "inspect.package" ||
        std::get<sp::ToolCall>(parts[2]).id != "installed-call" ||
        completion.stop.kind != sp::StopKind::ToolUse ||
        !completion.usage.output_total || completion.usage.output_total->value != 0 ||
        !completion.usage.total || completion.usage.total->evidence != sp::Evidence::Derived ||
        completion.usage.stage != sp::UsageStage::Final ||
        event_kinds != std::vector<std::size_t>{sp::Event{sp::Begin{}}.index(),
            sp::Event{sp::UsageUpdate{}}.index(), sp::Event{sp::Stop{}}.index()}) {
        std::cerr << "installed typed result/event preservation failed\n";
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
