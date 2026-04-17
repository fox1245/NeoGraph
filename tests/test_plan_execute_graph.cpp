#include <gtest/gtest.h>
#include <neograph/graph/plan_execute_graph.h>
#include <neograph/provider.h>
#include <neograph/tool.h>
#include <neograph/types.h>
#include <neograph/json.h>

#include <mutex>
#include <queue>
#include <string>

using namespace neograph;
using namespace neograph::graph;

// --------------------------------------------------------------------------
// Scripted provider: returns a queue of canned responses, one per complete()
// call. Tests use this to validate that the Plan & Execute graph walks
// planner -> executor (N times) -> responder in order.
// --------------------------------------------------------------------------
class ScriptedProvider : public Provider {
public:
    void push_response(const std::string& content) {
        ChatMessage m;
        m.role = "assistant";
        m.content = content;
        std::lock_guard<std::mutex> g(mu_);
        queue_.push(std::move(m));
    }
    std::vector<std::string> call_log() const {
        std::lock_guard<std::mutex> g(mu_);
        return log_;
    }
    int call_count() const {
        std::lock_guard<std::mutex> g(mu_);
        return static_cast<int>(log_.size());
    }

    ChatCompletion complete(const CompletionParams& params) override {
        std::lock_guard<std::mutex> g(mu_);
        // Log the first message content so we can assert prompts if needed.
        std::string first;
        if (!params.messages.empty()) first = params.messages.front().content;
        log_.push_back(first);

        ChatCompletion c;
        if (!queue_.empty()) {
            c.message = std::move(queue_.front());
            queue_.pop();
        } else {
            c.message.role = "assistant";
            c.message.content = "<exhausted>";
        }
        return c;
    }
    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback&) override {
        return complete(params);
    }
    std::string get_name() const override { return "scripted"; }

private:
    mutable std::mutex mu_;
    std::queue<ChatMessage> queue_;
    std::vector<std::string> log_;
};

TEST(PlanExecuteGraph, WalksPlannerExecutorResponderAndCollectsResults) {
    auto provider = std::make_shared<ScriptedProvider>();

    // 1) planner: returns 3 steps as a JSON array
    provider->push_response(R"(["step A", "step B", "step C"])");
    // 2-4) executor: one response per step, no tool calls
    provider->push_response("did A -> result A");
    provider->push_response("did B -> result B");
    provider->push_response("did C -> result C");
    // 5) responder: final synthesis
    provider->push_response("FINAL: handled A/B/C");

    auto engine = create_plan_execute_graph(
        provider,
        {},  // no tools
        "You are a planner. Reply with a JSON array of steps.",
        "You are an executor. Do exactly this step.",
        "You are a responder. Summarise the completed work.",
        "gpt-test",
        /*max_step_iterations=*/3);

    RunConfig cfg;
    cfg.input = json::object();
    cfg.input["messages"] = json::array({
        json{{"role", "user"}, {"content", "help me do ABC"}}
    });
    cfg.max_steps = 20;

    auto result = engine->run(cfg);

    EXPECT_EQ(provider->call_count(), 5) << "planner + 3 executors + responder";

    ASSERT_TRUE(result.output.contains("channels"));
    auto channels = result.output["channels"];

    ASSERT_TRUE(channels.contains("plan"));
    auto plan_val = channels["plan"]["value"];
    // Plan should be empty after all steps consumed.
    EXPECT_TRUE(plan_val.is_array());
    EXPECT_EQ(plan_val.size(), 0u);

    ASSERT_TRUE(channels.contains("past_steps"));
    auto past = channels["past_steps"]["value"];
    ASSERT_TRUE(past.is_array());
    EXPECT_EQ(past.size(), 3u);
    EXPECT_EQ(past[0]["step"].get<std::string>(), "step A");
    EXPECT_EQ(past[0]["result"].get<std::string>(), "did A -> result A");
    EXPECT_EQ(past[2]["step"].get<std::string>(), "step C");

    ASSERT_TRUE(channels.contains("final_response"));
    EXPECT_EQ(channels["final_response"]["value"].get<std::string>(),
              "FINAL: handled A/B/C");

    // Execution trace hits planner, executor x3, responder (names in order).
    std::vector<std::string> expected = {
        "planner", "executor", "executor", "executor", "responder"};
    EXPECT_EQ(result.execution_trace, expected);
}

TEST(PlanExecuteGraph, EmptyPlanSkipsExecutorGoesStraightToResponder) {
    auto provider = std::make_shared<ScriptedProvider>();

    // Planner returns no usable steps — should skip executor entirely.
    provider->push_response("I have no plan. Cannot parse a list.");
    provider->push_response("Nothing to report.");

    auto engine = create_plan_execute_graph(
        provider, {},
        "planner", "executor", "responder",
        "", 5);

    RunConfig cfg;
    cfg.input = json::object();
    cfg.input["messages"] = json::array({
        json{{"role", "user"}, {"content", "empty case"}}
    });
    cfg.max_steps = 5;

    auto result = engine->run(cfg);

    EXPECT_EQ(provider->call_count(), 2);
    std::vector<std::string> expected = {"planner", "responder"};
    EXPECT_EQ(result.execution_trace, expected);
}

TEST(PlanExecuteGraph, AcceptsFencedJsonAndNumberedListFallbacks) {
    auto provider = std::make_shared<ScriptedProvider>();

    provider->push_response(
        "Here is the plan:\n```json\n[\"one\", \"two\"]\n```\nDone.");
    provider->push_response("r1");
    provider->push_response("r2");
    provider->push_response("final");

    auto engine = create_plan_execute_graph(
        provider, {}, "p", "e", "r", "", 3);

    RunConfig cfg;
    cfg.input["messages"] = json::array({
        json{{"role", "user"}, {"content", "go"}}
    });
    cfg.max_steps = 10;

    auto result = engine->run(cfg);
    auto past = result.output["channels"]["past_steps"]["value"];
    ASSERT_TRUE(past.is_array());
    EXPECT_EQ(past.size(), 2u);
    EXPECT_EQ(past[0]["step"].get<std::string>(), "one");
    EXPECT_EQ(past[1]["step"].get<std::string>(), "two");
}
