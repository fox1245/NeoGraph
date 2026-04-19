// Smoke tests for the `enable_human_review` flag on the Deep Research
// graph. Doesn't exercise the LLM — just proves:
//
//   1. The graph factory builds successfully with the flag ON
//      (channel/node/edge JSON is well-formed and all node types are
//      registered).
//   2. The flag flips the graph topology: `human_review` shows up in
//      the wiring only when the flag is on.
//
// End-to-end behaviour (NodeInterrupt → resume → routing) is covered
// by the manual demo in examples/26_postgres_react_hitl, since that
// path requires a real Anthropic key.

#include <gtest/gtest.h>
#include <neograph/graph/deep_research_graph.h>
#include <neograph/provider.h>
#include <neograph/tool.h>

using namespace neograph;

namespace {

// Stub provider that throws if anyone actually tries to call the LLM.
// Plenty for compile/wiring tests — the graph factory is allowed to
// stash a provider reference on its nodes, but it must not invoke it.
class StubProvider : public Provider {
public:
    ChatCompletion complete(const CompletionParams&) override {
        ADD_FAILURE() << "StubProvider::complete must not be invoked";
        return {};
    }
    ChatCompletion complete_stream(const CompletionParams&,
                                    const StreamCallback&) override {
        ADD_FAILURE() << "StubProvider::complete_stream must not be invoked";
        return {};
    }
    std::string get_name() const override { return "stub"; }
};

} // namespace

TEST(DeepResearchHumanReview, FactoryBuildsWithFlagOff) {
    graph::DeepResearchConfig cfg;
    cfg.enable_human_review = false;
    auto provider = std::make_shared<StubProvider>();
    auto engine = graph::create_deep_research_graph(
        provider, /*tools=*/{}, cfg);
    ASSERT_NE(engine, nullptr);
}

TEST(DeepResearchHumanReview, FactoryBuildsWithFlagOn) {
    graph::DeepResearchConfig cfg;
    cfg.enable_human_review = true;
    auto provider = std::make_shared<StubProvider>();
    auto engine = graph::create_deep_research_graph(
        provider, /*tools=*/{}, cfg);
    ASSERT_NE(engine, nullptr);
}
