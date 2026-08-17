#include <gtest/gtest.h>

#include <neograph/async/run_sync.h>
#include <neograph/graph/engine.h>
#include <neograph/hook_runtime.h>

#include <stdexcept>
#include <set>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

class RecordingTool final : public Tool {
public:
    ChatTool get_definition() const override { return {"checkpoint-audit", "", json::object()}; }
    std::string get_name() const override { return "checkpoint-audit"; }
    std::string execute(const json& arguments) override {
        arguments_.push_back(arguments);
        return "ok";
    }

    std::vector<json> arguments_;
};

class WriteNode final : public GraphNode {
public:
    explicit WriteNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput output;
        output.writes.push_back({"result", name_});
        co_return output;
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
};

class FailingAsyncSaveStore final : public InMemoryCheckpointStore {
public:
    asio::awaitable<void> save_async(const Checkpoint&) override {
        throw std::runtime_error("checkpoint save failed");
    }
};

std::shared_ptr<HookRuntime> checkpoint_runtime(
    RecordingTool& tool, std::vector<json>& events) {
    HookTargetResolver resolver = [&tool](std::string_view target)
        -> std::optional<HookTargetContract> {
        if (target != "checkpoint-audit") return std::nullopt;
        return HookTargetContract{&tool, {"audit"}, ToolEffectClass::ReadOnly};
    };
    auto registry = std::make_shared<HookRegistry>(resolver, std::set<std::string>{"capture"});
    HookDefinitionData definition;
    definition.priority = 1;
    definition.phase = HookPhase::CheckpointPublished;
    definition.target_id = "checkpoint-audit";
    definition.required_capabilities = {"audit"};
    definition.effect = ToolEffectClass::ReadOnly;
    definition.input_mapper = {HookInputMapperKind::HostMapper, {}, json(), "capture"};
    registry->admit(HookDefinition::create(std::move(definition)));
    auto adapter = std::make_shared<NativeHookExecutionAdapter>(
        resolver, std::make_shared<ToolExecutionController>(),
        [&events](std::string_view, const RuntimeEvent& event) {
            events.push_back({{"owner_scope", event.owner_scope()}, {"run_id", event.run_id()},
                              {"data", event.data()}});
            return json::object();
        }, std::set<std::string>{"capture"});
    return std::make_shared<HookRuntime>(std::make_shared<MandatoryHookRunner>(
        std::make_shared<InMemoryHookJournal>(), std::move(registry), std::move(adapter)));
}

json graph_definition() {
    return {{"name", "checkpoint_hook_graph"},
            {"channels", {{"result", {{"reducer", "overwrite"}}}}},
            {"nodes", {{"first", {{"type", "checkpoint_hook_test"}}},
                       {"second", {{"type", "checkpoint_hook_test"}}}}},
            {"edges", {{{"from", "__start__"}, {"to", "first"}},
                       {{"from", "first"}, {"to", "second"}},
                       {{"from", "second"}, {"to", "__end__"}}}}};
}

void register_node() {
    NodeFactory::instance().register_type(
        "checkpoint_hook_test", [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<WriteNode>(name);
        });
}

TEST(CheckpointHookRuntime, PublishesSavedCheckpointWithRunAndThreadMetadataAcrossResume) {
    register_node();
    RecordingTool tool;
    std::vector<json> events;
    EngineConfig engine_config;
    engine_config.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
    engine_config.hook_runtime = checkpoint_runtime(tool, events);
    auto engine = GraphEngine::build(graph_definition(), std::move(engine_config));

    RunConfig run;
    run.thread_id = "thread-checkpoint";
    run.max_steps = 1;
    RunMetadata initial_metadata;
    initial_metadata.owner_scope = "owner-checkpoint";
    initial_metadata.run_id = "run-initial";
    initial_metadata.trace_id = "trace-initial";
    async::run_sync(engine->run_async(run, initial_metadata));

    RunConfig resume;
    resume.thread_id = "thread-checkpoint";
    RunMetadata resume_metadata;
    resume_metadata.owner_scope = "owner-checkpoint";
    resume_metadata.run_id = "run-resume";
    resume_metadata.trace_id = "trace-resume";
    async::run_sync(engine->resume_async(resume, {}, {}, resume_metadata));

    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0]["owner_scope"], "owner-checkpoint");
    EXPECT_EQ(events[0]["run_id"], "run-initial");
    EXPECT_FALSE(events[0]["data"]["checkpoint_id"].get<std::string>().empty());
    EXPECT_EQ(events[0]["data"]["thread_id"], "thread-checkpoint");
    EXPECT_EQ(events[0]["data"]["run_metadata"]["trace_id"], "trace-initial");
    EXPECT_EQ(events[1]["run_id"], "run-resume");
    EXPECT_EQ(events[1]["data"]["thread_id"], "thread-checkpoint");
    EXPECT_EQ(events[1]["data"]["run_metadata"]["trace_id"], "trace-resume");
    EXPECT_EQ(tool.arguments_.size(), 2u);
}

TEST(CheckpointHookRuntime, DoesNotPublishWhenAsyncSaveFails) {
    register_node();
    RecordingTool tool;
    std::vector<json> events;
    EngineConfig engine_config;
    engine_config.checkpoint_store = std::make_shared<FailingAsyncSaveStore>();
    engine_config.hook_runtime = checkpoint_runtime(tool, events);
    auto engine = GraphEngine::build(graph_definition(), std::move(engine_config));

    RunConfig run;
    run.thread_id = "thread-failed-save";
    EXPECT_THROW(async::run_sync(engine->run_async(run)), std::runtime_error);
    EXPECT_TRUE(events.empty());
    EXPECT_TRUE(tool.arguments_.empty());
}

} // namespace
