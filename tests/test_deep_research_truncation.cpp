// A reasoning model can burn its whole `max_tokens` budget on hidden
// reasoning and return an empty completion with stop reason `max_tokens`.
// The Deep Research graph must retry that with a larger budget and must
// never record the empty/cut-off text as a successful report.

#include <gtest/gtest.h>
#include <neograph/graph/deep_research_graph.h>
#include <neograph/graph/engine.h>
#include <neograph/provider.h>
#include <neograph/tool.h>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

using namespace neograph;

namespace {

enum class Role { Brief, Supervisor, Researcher, Compress, FinalReport };

Role classify(const CompletionParams& p) {
    const std::string& sys = p.messages.front().content;
    if (sys.find("research brief") != std::string::npos
        && sys.find("convert") != std::string::npos) return Role::Brief;
    if (sys.find("lead research supervisor") != std::string::npos) return Role::Supervisor;
    if (sys.find("focused researcher") != std::string::npos) return Role::Researcher;
    if (sys.find("Compress raw research notes") != std::string::npos) return Role::Compress;
    return Role::FinalReport;
}

struct Reply {
    std::string content;
    std::string stop = "end_turn";
    std::vector<ToolCall> calls;
};

// Serves per-role scripted replies and records every request's max_tokens.
class RoleProvider : public Provider {
public:
    void script(Role r, Reply reply) {
        std::lock_guard<std::mutex> g(mu_);
        scripts_[static_cast<int>(r)].push_back(std::move(reply));
    }
    std::vector<int> budgets(Role r) const {
        std::lock_guard<std::mutex> g(mu_);
        return budgets_[static_cast<int>(r)];
    }
    std::string last_user(Role r) const {
        std::lock_guard<std::mutex> g(mu_);
        return last_user_[static_cast<int>(r)];
    }

    ChatCompletion complete(const CompletionParams& p) override {
        std::lock_guard<std::mutex> g(mu_);
        const int r = static_cast<int>(classify(p));
        budgets_[r].push_back(p.max_tokens);
        last_user_[r] = p.messages.back().content;
        auto& q = scripts_[r];
        Reply reply = q.front();  // the last scripted reply repeats
        if (q.size() > 1) q.pop_front();
        ChatCompletion c;
        c.message.role = "assistant";
        c.message.content = reply.content;
        c.message.tool_calls = reply.calls;
        c.stop_reason = reply.stop;
        return c;
    }
    ChatCompletion complete_stream(const CompletionParams& p,
                                   const StreamCallback&) override {
        return complete(p);
    }
    std::string get_name() const override { return "role-scripted"; }

private:
    mutable std::mutex mu_;
    std::deque<Reply> scripts_[5];
    std::vector<int> budgets_[5];
    std::string last_user_[5];
};

ToolCall call(const std::string& id, const std::string& name, const std::string& args) {
    ToolCall tc;
    tc.id = id; tc.name = name; tc.arguments = args;
    return tc;
}

// brief -> supervisor(conduct_research) -> researcher -> compress ->
// supervisor(research_complete) -> final_report.
std::shared_ptr<RoleProvider> happy_path_provider() {
    auto p = std::make_shared<RoleProvider>();
    p->script(Role::Brief, {"brief text"});
    p->script(Role::Supervisor,
        {"", "tool_use", {call("c1", "conduct_research", R"({"research_topic":"topic A"})")}});
    p->script(Role::Supervisor,
        {"", "tool_use", {call("c2", "research_complete", "{}")}});
    p->script(Role::Researcher, {"raw findings"});
    p->script(Role::Compress, {"COMPRESSED-A"});
    return p;
}

graph::RunResult run_graph(const std::shared_ptr<RoleProvider>& p) {
    graph::DeepResearchConfig cfg;
    cfg.model = "test-model";
    auto engine = graph::create_deep_research_graph(p, {}, cfg);
    // The graph's default policy re-runs a failing LLM node after a 15-45 s
    // backoff (meant for transient 429/5xx). Disable it here so the tests
    // observe exactly one pass through the output-budget ladder.
    engine->set_node_retry_policy("final_report", graph::RetryPolicy{});
    graph::RunConfig rc;
    rc.thread_id = "t";
    rc.max_steps = 40;
    rc.input = {{"user_query", "q"}};
    return engine->run(rc);
}

std::string report_of(const graph::RunResult& r) {
    return r.output["channels"]["final_report"]["value"].get<std::string>();
}

}  // namespace

TEST(DeepResearchTruncation, FinalReportRetriesEmptyTruncationWithLargerBudget) {
    auto p = happy_path_provider();
    p->script(Role::FinalReport, {"", "max_tokens"});
    p->script(Role::FinalReport, {"## Real report", "end_turn"});

    auto result = run_graph(p);

    EXPECT_EQ(report_of(result), "## Real report");
    EXPECT_EQ(p->budgets(Role::FinalReport), (std::vector<int>{4096, 8192}));
}

TEST(DeepResearchTruncation, FinalReportEmptyAfterRetriesIsAnErrorNotAnEmptyReport) {
    auto p = happy_path_provider();
    p->script(Role::FinalReport, {"", "max_tokens"});  // repeats forever

    try {
        run_graph(p);
        FAIL() << "an empty truncated final report must not complete successfully";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("final_report"), std::string::npos) << e.what();
        EXPECT_NE(std::string(e.what()).find("max_tokens"), std::string::npos) << e.what();
    }
    EXPECT_EQ(p->budgets(Role::FinalReport), (std::vector<int>{4096, 8192, 16384}));
}

TEST(DeepResearchTruncation, PartialTruncatedReportIsFlaggedIncomplete) {
    auto p = happy_path_provider();
    p->script(Role::FinalReport, {"## Partial", "max_tokens"});

    auto result = run_graph(p);

    const std::string report = report_of(result);
    EXPECT_EQ(report.rfind("## Partial", 0), 0u);
    EXPECT_NE(report.find("Incomplete"), std::string::npos) << report;
    EXPECT_EQ(p->budgets(Role::FinalReport).size(), 1u) << "visible text is not retried";
}

TEST(DeepResearchTruncation, CompressedFindingsRetryEmptyTruncation) {
    auto q = std::make_shared<RoleProvider>();
    q->script(Role::Brief, {"brief text"});
    q->script(Role::Supervisor,
        {"", "tool_use", {call("c1", "conduct_research", R"({"research_topic":"topic A"})")}});
    q->script(Role::Supervisor,
        {"", "tool_use", {call("c2", "research_complete", "{}")}});
    q->script(Role::Researcher, {"raw findings"});
    q->script(Role::Compress, {"", "max_tokens"});
    q->script(Role::Compress, {"COMPRESSED-A", "end_turn"});
    q->script(Role::FinalReport, {"## ok", "end_turn"});

    auto result = run_graph(q);

    EXPECT_EQ(q->budgets(Role::Compress), (std::vector<int>{2048, 4096}));
    EXPECT_NE(q->last_user(Role::FinalReport).find("COMPRESSED-A"), std::string::npos);
    EXPECT_EQ(report_of(result), "## ok");
}
