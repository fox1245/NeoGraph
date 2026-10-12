// A tool that knows its call did not run: the service hosting it refused the
// request before executing it. The outcome is known, so the controller does
// not ask the model to reconcile an effect, whatever the tool's policy says
// (reference, "Tool failure contract").

#include <gtest/gtest.h>

#include <neograph/neograph.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/types.h>
#include <neograph/tool_dispatch.h>
#include <neograph/tool_execution.h>

#include <memory>
#include <stdexcept>
#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

enum class Refusal { Rejected, RejectedRetryable, OwnBase, OrdinaryFailure };

// An exception type with a base of its own that also says the call did not run.
class OwnBaseRefusal final : public std::range_error, public ToolRequestRefusal {
public:
    explicit OwnBaseRefusal(const std::string& message) : std::range_error(message) {}
    bool retryable() const noexcept override { return false; }
};

class RefusingTool final : public Tool {
public:
    explicit RefusingTool(Refusal refusal) : refusal_(refusal) {}
    ChatTool get_definition() const override {
        return {"refuser", "refuses its requests", json{{"type", "object"}}};
    }
    std::string get_name() const override { return "refuser"; }
    std::string execute(const json&) override {
        switch (refusal_) {
            case Refusal::Rejected: throw ToolRequestRejected("arguments refused");
            case Refusal::RejectedRetryable: throw ToolRequestRejected("try again later", true);
            case Refusal::OwnBase: throw OwnBaseRefusal("refused by a type with its own base");
            case Refusal::OrdinaryFailure: throw std::runtime_error("the tool ran and failed");
        }
        return {};
    }

private:
    Refusal refusal_;
};

std::shared_ptr<ToolExecutionController> controller_declaring(ToolEffectClass effect) {
    auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
    ToolExecutionPolicy policy;
    policy.effect = effect;
    policies->upsert("refuser", policy);
    return std::make_shared<ToolExecutionController>(
        ToolExecutionControllerConfig{.policies = policies});
}

ToolExecutionResult run_with(Refusal refusal, std::shared_ptr<ToolExecutionController> controller) {
    RefusingTool tool(refusal);
    ToolExecutionContext context;
    context.controller = controller;
    return neograph::async::run_sync(controller->execute_result_async(tool, json::object(), context));
}

}  // namespace

TEST(ToolRequestRefusal, IsAPlainFailureWhateverThePolicySays) {
    // The default policy (no declaration) and a declared write: the call may
    // have changed something for any other exception, but not for a refusal.
    for (const auto& controller :
         {std::make_shared<ToolExecutionController>(), controller_declaring(ToolEffectClass::WorkspaceWrite),
          controller_declaring(ToolEffectClass::ReadOnly)}) {
        const auto refused = run_with(Refusal::Rejected, controller);
        EXPECT_EQ(refused.status, ToolTerminalStatus::Failed);
        EXPECT_EQ(refused.error, "arguments refused");
        EXPECT_FALSE(refused.effect_uncertain);
        EXPECT_FALSE(refused.retryable) << "the same request is refused again";

        EXPECT_TRUE(run_with(Refusal::RejectedRetryable, controller).retryable);

        const auto own_base = run_with(Refusal::OwnBase, controller);
        EXPECT_EQ(own_base.status, ToolTerminalStatus::Failed);
        EXPECT_FALSE(own_base.effect_uncertain);
    }
}

TEST(ToolRequestRefusal, AnyOtherFailureOfAStartedCallStaysUncertain) {
    EXPECT_EQ(run_with(Refusal::OrdinaryFailure, std::make_shared<ToolExecutionController>()).status,
              ToolTerminalStatus::ReconciliationRequired);
    EXPECT_EQ(run_with(Refusal::OrdinaryFailure, controller_declaring(ToolEffectClass::WorkspaceWrite)).status,
              ToolTerminalStatus::ReconciliationRequired);
    EXPECT_EQ(run_with(Refusal::OrdinaryFailure, controller_declaring(ToolEffectClass::ReadOnly)).status,
              ToolTerminalStatus::Failed);
}

TEST(ToolRequestRefusal, TheModelReadsAPlainFailureWithTheMessage) {
    RefusingTool tool(Refusal::Rejected);
    std::vector<Tool*> tools{&tool};
    const auto messages = neograph::async::run_sync(
        dispatch_tool_calls({{"call-1", "refuser", "{}"}}, tools));
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].tool_status, "failed");
    EXPECT_FALSE(messages[0].tool_effect_uncertain);
    EXPECT_FALSE(messages[0].tool_retryable);
    const auto content = json::parse(messages[0].content);
    EXPECT_EQ(content.at("error").get<std::string>(), "arguments refused");
    EXPECT_EQ(content.at("status").get<std::string>(), "failed");
    EXPECT_FALSE(content.at("retryable").get<bool>());
    EXPECT_FALSE(content.at("effect_uncertain").get<bool>());
}

// The failure projection every protocol host uses (graph::classify_failure): a
// refusal is an ordinary failure, retried only when it says a retry may help.
TEST(ToolRequestRefusal, TheNodeFailureClassifierReadsItAsAnOrdinaryFailure) {
    const auto classify = [](auto make) {
        try {
            throw make();
        } catch (...) {
            return classify_failure(std::current_exception());
        }
    };
    const auto refused = classify([] { return ToolRequestRejected("refused"); });
    EXPECT_EQ(refused.failure_class, NodeFailureClass::Ordinary);
    EXPECT_EQ(refused.code, "NG.TaskFailed");
    EXPECT_FALSE(refused.retryable);
    EXPECT_TRUE(refused.routable);

    const auto transient = classify([] { return ToolRequestRejected("later", true); });
    EXPECT_EQ(transient.failure_class, NodeFailureClass::Ordinary);
    EXPECT_TRUE(transient.retryable);

    const auto own_base = classify([] { return OwnBaseRefusal("own base"); });
    EXPECT_EQ(own_base.failure_class, NodeFailureClass::Ordinary);
    EXPECT_FALSE(own_base.retryable);

    // Unchanged: an ordinary runtime_error is retried by the node retry policy.
    const auto ordinary = classify([] { return std::runtime_error("failed"); });
    EXPECT_EQ(ordinary.failure_class, NodeFailureClass::Ordinary);
    EXPECT_TRUE(ordinary.retryable);
}
