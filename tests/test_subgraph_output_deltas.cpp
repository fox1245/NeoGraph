#include <gtest/gtest.h>

#include <neograph/neograph.h>
#include <codecs/responses.h>
#include <codecs/responses_request.h>
#include <core/native.h>
#include <descriptor/descriptor.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

class WritesNode final : public GraphNode {
public:
    WritesNode(std::string name,
               std::vector<ChannelWrite> writes,
               std::vector<ChannelWrite> command_updates = {},
               std::atomic<int>* calls = nullptr,
               std::atomic<int>* failures_remaining = nullptr)
        : name_(std::move(name)),
          writes_(std::move(writes)),
          command_updates_(std::move(command_updates)),
          calls_(calls),
          failures_remaining_(failures_remaining) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        if (calls_) calls_->fetch_add(1, std::memory_order_relaxed);
        if (failures_remaining_ &&
            failures_remaining_->fetch_sub(1, std::memory_order_relaxed) > 0) {
            throw std::runtime_error("transient child failure");
        }

        NodeOutput out;
        out.writes = writes_;
        if (!command_updates_.empty()) {
            Command command;
            command.updates = command_updates_;
            out.command = std::move(command);
        }
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    std::vector<ChannelWrite> writes_;
    std::vector<ChannelWrite> command_updates_;
    std::atomic<int>* calls_;
    std::atomic<int>* failures_remaining_;
};

class FailOnceNode final : public GraphNode {
public:
    FailOnceNode(std::string name, std::atomic<int>* calls)
        : name_(std::move(name)), calls_(calls) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        const int call = calls_->fetch_add(1, std::memory_order_relaxed);
        if (call == 0) throw std::runtime_error("fail parent super-step once");
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    std::atomic<int>* calls_;
};

class FanoutNode final : public GraphNode {
public:
    explicit FanoutNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        out.sends.push_back(Send{"worker", {{"slot", "zero"}}});
        out.sends.push_back(Send{"worker", {{"slot", "one"}}});
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class SendValueNode final : public GraphNode {
public:
    explicit SendValueNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{
            "results", json::array({in.state.get("slot")})});
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

json one_node_graph(const std::string& name,
                    const std::string& node_type,
                    json channels,
                    json retry_policy = json()) {
    json graph = {
        {"name", name},
        {"channels", std::move(channels)},
        {"nodes", {{"node", {{"type", node_type}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "node"}},
            {{"from", "node"}, {"to", "__end__"}},
        })},
    };
    if (!retry_policy.is_null()) graph["retry_policy"] = std::move(retry_policy);
    return graph;
}

std::shared_ptr<GraphEngine> shared_engine(const json& definition,
                                           std::shared_ptr<CheckpointStore> store = nullptr) {
    return std::shared_ptr<GraphEngine>(
        GraphEngine::compile(definition, NodeContext{}, std::move(store)).release());
}

void register_writer(const std::string& type,
                     std::vector<ChannelWrite> writes,
                     std::vector<ChannelWrite> command_updates = {},
                     std::atomic<int>* calls = nullptr,
                     std::atomic<int>* failures_remaining = nullptr) {
    NodeFactory::instance().register_type(
        type,
        [writes = std::move(writes),
         command_updates = std::move(command_updates),
         calls,
         failures_remaining](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<WritesNode>(
                name, writes, command_updates, calls, failures_remaining);
        });
}

void register_subgraph(const std::string& type,
                       std::shared_ptr<GraphEngine> child,
                       std::map<std::string, std::string> input_map = {},
                       std::map<std::string, std::string> output_map = {}) {
    NodeFactory::instance().register_type(
        type,
        [child = std::move(child),
         input_map = std::move(input_map),
         output_map = std::move(output_map)](
            const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SubgraphNode>(
                name, child, input_map, output_map);
        });
}

json message(const std::string& role, const std::string& content) {
    return {{"role", role}, {"content", content}};
}

json append_channels() {
    return {{"messages", {{"reducer", "append"}}}};
}

}  // namespace

TEST(SubgraphOutputDeltas, IdentityMappingDoesNotReduceInheritedAppendStateTwice) {
    register_writer("issue235_identity_writer", {
        ChannelWrite{"messages", json::array({message("assistant", "child reply")})},
    });
    auto child = shared_engine(one_node_graph(
        "identity_child", "issue235_identity_writer", append_channels()));
    register_subgraph("issue235_identity_subgraph", child);

    auto parent = GraphEngine::compile(one_node_graph(
        "identity_parent", "issue235_identity_subgraph", append_channels()),
        NodeContext{});

    RunConfig config;
    config.input = {{"messages", json::array({message("user", "hello")})}};
    const auto result = parent->run(config);

    const auto messages = result.channel<json>("messages");
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0], message("user", "hello"));
    EXPECT_EQ(messages[1], message("assistant", "child reply"));
}

TEST(SubgraphOutputDeltas, ExplicitMappingAppliesAppendCustomAndOverwriteDeterministically) {
    ReducerRegistry::instance().register_reducer(
        "issue235_decimal",
        [](const json& current, const json& incoming) {
            return json(current.get<int>() * 10 + incoming.get<int>());
        });

    const json child_channels = {
        {"child_append", {{"reducer", "append"}}},
        {"child_custom", {{"reducer", "issue235_decimal"}, {"initial", 0}}},
        {"child_scalar", {{"reducer", "overwrite"}}},
    };
    register_writer("issue235_explicit_writer", {
        ChannelWrite{"child_append", json::array({"child"})},
        ChannelWrite{"child_custom", 3},
        ChannelWrite{"child_scalar", "after"},
    });
    auto child = shared_engine(one_node_graph(
        "explicit_child", "issue235_explicit_writer", child_channels));
    register_subgraph(
        "issue235_explicit_subgraph", child,
        {{"parent_append", "child_append"},
         {"parent_custom", "child_custom"},
         {"parent_scalar", "child_scalar"}},
        {{"child_append", "parent_append"},
         {"child_custom", "parent_custom"},
         {"child_scalar", "parent_scalar"}});

    const json parent_channels = {
        {"parent_append", {{"reducer", "append"}}},
        {"parent_custom", {{"reducer", "issue235_decimal"}, {"initial", 0}}},
        {"parent_scalar", {{"reducer", "overwrite"}}},
    };
    auto parent = GraphEngine::compile(one_node_graph(
        "explicit_parent", "issue235_explicit_subgraph", parent_channels),
        NodeContext{});

    RunConfig config;
    config.input = {
        {"parent_append", json::array({"parent"})},
        {"parent_custom", 2},
        {"parent_scalar", "before"},
    };
    const auto result = parent->run(config);

    EXPECT_EQ(result.channel<json>("parent_append"), json::array({"parent", "child"}));
    EXPECT_EQ(result.channel<int>("parent_custom"), 23);
    EXPECT_EQ(result.channel<std::string>("parent_scalar"), "after");
}

TEST(SubgraphOutputDeltas, ChildOverwriteModeCrossesTheBoundary) {
    ChannelWrite replace{"messages", json::array({message("assistant", "reset")})};
    replace.mode = ChannelWrite::Mode::Overwrite;
    register_writer("issue235_overwrite_writer", {replace});
    auto child = shared_engine(one_node_graph(
        "overwrite_child", "issue235_overwrite_writer", append_channels()));
    register_subgraph("issue235_overwrite_subgraph", child);

    auto parent = GraphEngine::compile(one_node_graph(
        "overwrite_parent", "issue235_overwrite_subgraph", append_channels()),
        NodeContext{});
    RunConfig config;
    config.input = {{"messages", json::array({message("user", "discard me")})}};

    EXPECT_EQ(parent->run(config).channel<json>("messages"),
              json::array({message("assistant", "reset")}));
}

TEST(SubgraphOutputDeltas, PreservesWriteThenCommandUpdateOrder) {
    register_writer(
        "issue235_command_writer",
        {ChannelWrite{"messages", json::array({"write"})}},
        {ChannelWrite{"messages", json::array({"command"})}});
    auto child = shared_engine(one_node_graph(
        "command_child", "issue235_command_writer", append_channels()));
    register_subgraph("issue235_command_subgraph", child);

    auto parent = GraphEngine::compile(one_node_graph(
        "command_parent", "issue235_command_subgraph", append_channels()),
        NodeContext{});

    EXPECT_EQ(parent->run(RunConfig{}).channel<json>("messages"),
              json::array({"write", "command"}));
}

TEST(SubgraphOutputDeltas, NestedIdentityMappingDoesNotAmplifyInheritedHistory) {
    register_writer("issue235_nested_leaf_writer", {
        ChannelWrite{"messages", json::array({message("assistant", "leaf")})},
    });
    auto leaf = shared_engine(one_node_graph(
        "nested_leaf", "issue235_nested_leaf_writer", append_channels()));
    register_subgraph("issue235_nested_middle_subgraph", leaf);
    auto middle = shared_engine(one_node_graph(
        "nested_middle", "issue235_nested_middle_subgraph", append_channels()));
    register_subgraph("issue235_nested_outer_subgraph", middle);
    auto parent = GraphEngine::compile(one_node_graph(
        "nested_parent", "issue235_nested_outer_subgraph", append_channels()),
        NodeContext{});

    RunConfig config;
    config.input = {{"messages", json::array({message("user", "root")})}};
    const auto messages = parent->run(config).channel<json>("messages");

    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0], message("user", "root"));
    EXPECT_EQ(messages[1], message("assistant", "leaf"));
}

TEST(SubgraphOutputDeltas, RetryExportsOnlyTheSuccessfulAttempt) {
    std::atomic<int> calls{0};
    std::atomic<int> failures_remaining{1};
    register_writer(
        "issue235_retry_writer",
        {ChannelWrite{"messages", json::array({"success"})}},
        {}, &calls, &failures_remaining);
    auto child = shared_engine(one_node_graph(
        "retry_child", "issue235_retry_writer", append_channels(),
        {{"max_retries", 1}, {"initial_delay_ms", 0}}));
    register_subgraph("issue235_retry_subgraph", child);
    auto parent = GraphEngine::compile(one_node_graph(
        "retry_parent", "issue235_retry_subgraph", append_channels()),
        NodeContext{});

    EXPECT_EQ(parent->run(RunConfig{}).channel<json>("messages"),
              json::array({"success"}));
    EXPECT_EQ(calls.load(), 2);
}

TEST(SubgraphOutputDeltas, MultiSendWritesCrossInDeterministicFanInOrder) {
    NodeFactory::instance().register_type(
        "issue235_fanout",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<FanoutNode>(name);
        });
    NodeFactory::instance().register_type(
        "issue235_send_value",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SendValueNode>(name);
        });

    const json child_definition = {
        {"name", "send_child"},
        {"channels", {
            {"slot", {{"reducer", "overwrite"}}},
            {"results", {{"reducer", "append"}}},
        }},
        {"nodes", {
            {"fanout", {{"type", "issue235_fanout"}}},
            {"worker", {{"type", "issue235_send_value"}}},
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "fanout"}},
            {{"from", "fanout"}, {"to", "__end__"}},
            {{"from", "worker"}, {"to", "__end__"}},
        })},
    };
    auto child = shared_engine(child_definition);
    child->set_worker_count(2);
    register_subgraph(
        "issue235_send_subgraph", child, {}, {{"results", "results"}});
    auto parent = GraphEngine::compile(one_node_graph(
        "send_parent", "issue235_send_subgraph",
        {{"results", {{"reducer", "append"}}}}), NodeContext{});

    EXPECT_EQ(parent->run(RunConfig{}).channel<json>("results"),
              json::array({"zero", "one"}));
}

TEST(SubgraphOutputDeltas, ParentPendingWriteReplayDoesNotRerunOrDuplicateChild) {
    std::atomic<int> child_calls{0};
    std::atomic<int> sibling_calls{0};
    register_writer("issue235_replay_setup", {
        ChannelWrite{"setup", true},
    });
    register_writer(
        "issue235_replay_child_writer",
        {ChannelWrite{"messages", json::array({"child"})}},
        {}, &child_calls);
    NodeFactory::instance().register_type(
        "issue235_replay_fail_once",
        [&sibling_calls](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<FailOnceNode>(name, &sibling_calls);
        });

    const json child_channels = append_channels();
    auto child = shared_engine(one_node_graph(
        "replay_child", "issue235_replay_child_writer", child_channels));
    register_subgraph("issue235_replay_subgraph", child);

    const json parent = {
        {"name", "replay_parent"},
        {"channels", {
            {"setup", {{"reducer", "overwrite"}}},
            {"messages", {{"reducer", "append"}}},
        }},
        {"nodes", {
            {"setup", {{"type", "issue235_replay_setup"}}},
            {"child", {{"type", "issue235_replay_subgraph"}}},
            {"fail", {{"type", "issue235_replay_fail_once"}}},
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "setup"}},
            {{"from", "setup"}, {"to", "child"}},
            {{"from", "setup"}, {"to", "fail"}},
            {{"from", "child"}, {"to", "__end__"}},
            {{"from", "fail"}, {"to", "__end__"}},
        })},
    };
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(parent, NodeContext{}, store);
    engine->set_worker_count(2);

    RunConfig config;
    config.thread_id = "issue235-parent-replay";
    config.input = {{"messages", json::array({"parent"})}};
    EXPECT_THROW(engine->run(config), NodeExecutionError);
    EXPECT_EQ(child_calls.load(), 1);

    const auto resumed = engine->resume(config.thread_id);
    EXPECT_EQ(resumed.channel<json>("messages"), json::array({"parent", "child"}));
    EXPECT_EQ(child_calls.load(), 1);
    EXPECT_EQ(sibling_calls.load(), 2);
}

TEST(SubgraphOutputDeltas, MappedCapturedHistoryRetainsNativePreflightAcrossFreshChildren) {
    auto admitted = sp::descriptor::load(R"({"descriptor_version":1,"revision":1,
        "id":"mapped-native","family":"openai.responses",
        "connection":{"base_url":"https://fixture.invalid",
        "paths":{"buffered":"/v1/responses","streaming":"/v1/responses"}}})");
    ASSERT_TRUE(std::holds_alternative<sp::descriptor::ValidatedDescriptor>(admitted));
    const auto descriptor = std::get<sp::descriptor::ValidatedDescriptor>(std::move(admitted));
    sp::responses::Request request;
    request.model = "fixture-model";
    request.account_scope = "mapped-native";
    request.max_output_tokens = 128;
    request.messages = {sp::Message{"", sp::Role::User, {sp::Text{"question"}}}};
    const auto encoded = sp::responses::encode(descriptor, request, false);
    ASSERT_TRUE(std::holds_alternative<sp::responses::EncodedRequest>(encoded));
    sp::Accumulator accumulator;
    sp::responses::Codec codec(descriptor, sp::responses::Mode::Buffered, accumulator,
        std::get<sp::responses::EncodedRequest>(encoded).context);
    ASSERT_TRUE(codec.buffered(R"({"id":"mapped-response","object":"response","created_at":1,
        "model":"fixture-model","status":"completed","output":[
        {"id":"reason","type":"reasoning","status":"completed",
         "summary":[{"type":"summary_text","text":"consider"}],"encrypted_content":"mapped-private-fixture"},
        {"id":"answer","type":"message","status":"completed","role":"assistant",
         "content":[{"type":"output_text","text":"answer","annotations":[]}]}],
        "usage":{"input_tokens":2,"output_tokens":3,"total_tokens":5},
        "incomplete_details":null,"error":null})", {}));
    ASSERT_TRUE(accumulator.outcome());
    const auto captured = std::get<sp::Completion>(*accumulator.outcome()).messages.front();
    ASSERT_TRUE(captured.native && captured.native->complete() && captured.wire_output);
    auto client = std::make_shared<sp::runtime::Client>(descriptor);
    std::vector<sp::Message> history = request.messages;
    history.push_back(captured);
    history.push_back(sp::Message{"", sp::Role::User, {sp::Text{"next"}}});

    auto projection_only = request;
    projection_only.messages.reserve(history.size());
    for (std::size_t index = 1; index < history.size(); ++index)
        projection_only.messages.push_back(portable_message(project_message(history[index])));
    const auto rejected = client->prepare(std::move(projection_only));
    ASSERT_TRUE(rejected.error());
    EXPECT_EQ(rejected.error()->kind, sp::ErrorKind::ReplayIneligible);

    class NativePreparationNode final : public GraphNode {
    public:
        NativePreparationNode(std::shared_ptr<sp::runtime::Client> client,
                              sp::responses::Request request,
                              std::shared_ptr<const sp::NativeReplay> seal,
                              std::shared_ptr<std::atomic<unsigned>> admitted)
            : client_(std::move(client)), request_(std::move(request)),
              seal_(std::move(seal)), admitted_(std::move(admitted)) {}
        std::string get_name() const override { return "native-preparation"; }
        asio::awaitable<NodeOutput> run(NodeInput input) override {
            auto request = request_;
            request.messages = input.state.get_provider_messages();
            if (request.messages.size() < 3 || request.messages[1].native != seal_)
                throw std::logic_error("Mapped history lost genuine native custody");
            const auto prepared = client_->prepare(std::move(request));
            if (!prepared.valid())
                throw std::logic_error("Mapped native history cannot prepare its continuation");
            ++*admitted_;
            NodeOutput output;
            output.writes.push_back(provider_messages_write(std::vector<sp::Message>{
                sp::Message{"", sp::Role::User, {sp::Text{"local-observation"}}}}));
            co_return output;
        }
    private:
        std::shared_ptr<sp::runtime::Client> client_;
        sp::responses::Request request_;
        std::shared_ptr<const sp::NativeReplay> seal_;
        std::shared_ptr<std::atomic<unsigned>> admitted_;
    };
    const auto prepared_calls = std::make_shared<std::atomic<unsigned>>(0);
    NodeFactory::instance().register_type("mapped_native_preparer",
        [client, request, seal = captured.native, prepared_calls](const auto&, const json&, const NodeContext&) {
            return std::make_unique<NativePreparationNode>(client, request, seal, prepared_calls);
        });
    const auto child = shared_engine(one_node_graph(
        "mapped_native_child", "mapped_native_preparer", append_channels()));
    NodeFactory::instance().register_type("mapped_native_child",
        [child](const auto& name, const json&, const NodeContext&) {
            return std::make_unique<SubgraphNode>(name, child,
                std::map<std::string, std::string>{{"history", "messages"}},
                std::map<std::string, std::string>{{"messages", "history"}},
                SubgraphPersistence::Stateless);
        });
    auto seed = provider_messages_write(std::move(history));
    seed.channel = "history";
    std::vector<ChannelWrite> seed_writes;
    seed_writes.push_back(std::move(seed));
    register_writer("mapped_native_seed", std::move(seed_writes));
    const json definition = {
        {"name", "mapped_native_parent"},
        {"channels", {{"history", {{"reducer", "append"}}}}},
        {"nodes", {{"seed", {{"type", "mapped_native_seed"}}},
                   {"first", {{"type", "mapped_native_child"}}},
                   {"second", {{"type", "mapped_native_child"}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", "seed"}},
            {{"from", "seed"}, {"to", "first"}}, {{"from", "first"}, {"to", "second"}},
            {{"from", "second"}, {"to", "__end__"}}})}};
    auto parent = GraphEngine::compile(definition, NodeContext{});
    RunConfig config;
    config.thread_id = "mapped-native-parent";
    const auto result = parent->run(config);
    EXPECT_EQ(prepared_calls->load(), 2u);
    const auto actual = result.channel<json>("history");
    ASSERT_EQ(actual.size(), 5u);
    EXPECT_EQ(actual[0]["content"], "question");
    EXPECT_EQ(actual[2]["content"], "next");
    EXPECT_EQ(actual[3]["content"], "local-observation");
    EXPECT_EQ(actual[4]["content"], "local-observation");
}

TEST(SubgraphOutputDeltas, GenericMessagesChannelDoesNotBecomeModelHistoryAtCompletion) {
    const json value = json::array({{{"role", "assistant"},
        {"content", {{"state", json::array({1, true, nullptr})}}}}});
    register_writer("generic_messages_object", {ChannelWrite{"messages", value}});
    auto engine = GraphEngine::compile(one_node_graph(
        "generic_messages_object", "generic_messages_object", append_channels()), NodeContext{});
    const auto result = engine->run(RunConfig{});
    EXPECT_EQ(result.channel<json>("messages"), value);
    EXPECT_TRUE(result.native_messages.empty());
    EXPECT_FALSE(result.output.contains("final_response"));
}
