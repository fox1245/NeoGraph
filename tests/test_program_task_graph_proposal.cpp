#include <neograph/program/program.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using neograph::json;
using namespace neograph::program;

std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

TaskGraphBudget budget(std::uint64_t wall_time_ms,
                       std::uint64_t model_tokens,
                       std::uint64_t monetary_microunits,
                       std::uint64_t output_bytes) {
    return {wall_time_ms, model_tokens, monetary_microunits, output_bytes};
}

TaskGraphTemplateContract template_contract(std::string                        template_id,
                                            char                               identity,
                                            std::vector<std::string>           input_fields,
                                            std::vector<TaskGraphArtifactContract> output_artifacts) {
    TaskGraphTemplateContract contract;
    contract.template_id       = std::move(template_id);
    contract.content_identity  = digest(identity);
    contract.input_fields      = std::move(input_fields);
    contract.output_artifacts  = std::move(output_artifacts);
    contract.budget_ceiling    = budget(60000, 12000, 1000000, 4096);
    return contract;
}

TaskGraphProposalOptions proposal_options() {
    TaskGraphProposalOptions options;
    options.source_id = "planner:proposal";
    options.limits.max_tasks                    = 4;
    options.limits.max_edges                    = 4;
    options.limits.max_depth                    = 3;
    options.limits.per_task_budget_ceiling      = budget(60000, 12000, 1000000, 4096);
    options.limits.total_budget_ceiling         = budget(90000, 16000, 1500000, 8192);
    options.template_allowlist = {
        template_contract("researcher/v3", 'a', {"/question"},
                          {{"findings", {"/summary", "/sources"}}}),
        template_contract("security-reviewer/v2", 'b', {"/candidate_summary"},
                          {{"review", {"/decision"}}}),
    };
    return options;
}

json valid_proposal() {
    return json{
        {"schema_version", 1},
        {"tasks",
         json::array({
             json{{"id", "review-security"},
                  {"template", "security-reviewer/v2"},
                  {"input_bindings",
                   json::array({json{{"from",
                                      json{{"task", "research-api"},
                                           {"artifact", "findings"},
                                           {"field", "/summary"}}},
                                      {"to", json{{"field", "/candidate_summary"}}}}})},
                  {"depends_on", json::array({"research-api"})},
                  {"budget", json{{"wall_time_ms", 30000}, {"model_tokens", 4000}}}},
             json{{"id", "research-api"},
                  {"template", "researcher/v3"},
                  {"input_bindings", json::array()},
                  {"depends_on", json::array()},
                  {"budget", json{{"wall_time_ms", 60000}, {"model_tokens", 12000}}}},
         })},
        {"join", json{{"kind", "all"}}},
    };
}

std::filesystem::path proposal_corpus_dir() {
    return std::filesystem::path(__FILE__).parent_path() / "fixtures" / "task_graph_proposal";
}

json load_json(const std::filesystem::path& path) {
    std::ifstream input(path);
    EXPECT_TRUE(input.is_open()) << "cannot open " << path;
    return json::parse(std::string((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>()));
}

std::vector<std::string> diagnostic_codes(const std::vector<Diagnostic>& diagnostics) {
    std::vector<std::string> codes;
    codes.reserve(diagnostics.size());
    for (const auto& diagnostic : diagnostics)
        codes.push_back(diagnostic.code);
    std::sort(codes.begin(), codes.end());
    return codes;
}

std::vector<Diagnostic> proposal_errors(json document,
                                        TaskGraphProposalOptions options = proposal_options()) {
    try {
        (void)TaskGraphProposal::parse(document, std::move(options));
    } catch (const TaskGraphProposalError& error) {
        return error.diagnostics();
    }
    ADD_FAILURE() << "Expected TaskGraphProposalError";
    return {};
}

bool contains_code(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [code](const auto& diagnostic) {
        return diagnostic.code == code;
    });
}

const Diagnostic& diagnostic_with_code(const std::vector<Diagnostic>& diagnostics,
                                       std::string_view                code) {
    const auto found = std::find_if(diagnostics.begin(), diagnostics.end(),
                                    [code](const auto& diagnostic) {
                                        return diagnostic.code == code;
                                    });
    EXPECT_NE(found, diagnostics.end()) << "Missing diagnostic " << code;
    return *found;
}

}  // namespace

TEST(TaskGraphProposalTest, CanonicalizesEquivalentAcyclicProposals) {
    auto first_document = valid_proposal();
    auto second_document = json::object();
    second_document["join"] = first_document["join"];
    second_document["tasks"] = json::array({first_document["tasks"][1], first_document["tasks"][0]});
    second_document["schema_version"] = 1;

    const auto first  = TaskGraphProposal::parse(first_document, proposal_options());
    const auto second = TaskGraphProposal::parse(second_document, proposal_options());

    EXPECT_EQ(first.id(), second.id());
    EXPECT_EQ(first.serialize_canonical(), second.serialize_canonical());
    EXPECT_EQ(first.schema_version(), TaskGraphProposal::SCHEMA_VERSION);
    ASSERT_EQ(first.tasks().size(), 2U);
    EXPECT_EQ(first.tasks()[0].id, "research-api");
    EXPECT_EQ(first.tasks()[1].id, "review-security");
    EXPECT_EQ(first.tasks()[1].depends_on, std::vector<std::string>({"research-api"}));
    EXPECT_EQ(first.join().kind, TaskGraphJoinKind::All);
    EXPECT_EQ(first.source_id(), "planner:proposal");
    EXPECT_EQ(first.to_json()["tasks"][0]["id"], "research-api");
}

TEST(TaskGraphProposalTest, ValidatesProposalCorpus) {
    const auto directory = proposal_corpus_dir();
    const auto manifest = load_json(directory / "manifest.json");
    const auto& expected_cases = manifest.at("expected");
    for (auto entry = expected_cases.begin(); entry != expected_cases.end(); ++entry) {
        const auto filename = entry.key();
        const auto expected = entry.value().get<std::vector<std::string>>();
        SCOPED_TRACE(filename);
        if (expected.empty()) {
            EXPECT_NO_THROW((void)TaskGraphProposal::parse(load_json(directory / filename),
                                                            proposal_options()));
            continue;
        }

        try {
            (void)TaskGraphProposal::parse(load_json(directory / filename), proposal_options());
            ADD_FAILURE() << "Expected TaskGraphProposalError";
        } catch (const TaskGraphProposalError& error) {
            EXPECT_EQ(diagnostic_codes(error.diagnostics()), expected);
        }
    }
}

TEST(TaskGraphProposalTest, RemapsSourceMapToCanonicalTaskOrder) {
    auto options = proposal_options();
    options.source_map.push_back(
        {"/tasks/1/id", {"planner:turn-6", "/response/research-id", std::nullopt}});
    const auto proposal = TaskGraphProposal::parse(valid_proposal(), std::move(options));

    const auto entry = std::find_if(proposal.source_map().begin(), proposal.source_map().end(),
                                    [](const auto& candidate) {
                                        return candidate.generated_pointer == "/tasks/0/id";
                                    });
    ASSERT_NE(entry, proposal.source_map().end());
    EXPECT_EQ(entry->authored.source_id, "planner:turn-6");
    EXPECT_EQ(entry->authored.json_pointer, "/response/research-id");
}

TEST(TaskGraphProposalTest, RemapsSourceMapToCanonicalBindingOrder) {
    auto document = valid_proposal();
    document["tasks"][0]["input_bindings"].push_back(
        json{{"from",
              json{{"task", "research-api"}, {"artifact", "findings"}, {"field", "/sources"}}},
             {"to", json{{"field", "/candidate_sources"}}}});

    auto options = proposal_options();
    options.template_allowlist[1].input_fields.push_back("/candidate_sources");
    options.source_map.push_back({"/tasks/0/input_bindings/1/from/field",
                                  {"planner:turn-6", "/response/sources-field", std::nullopt}});
    const auto proposal = TaskGraphProposal::parse(std::move(document), std::move(options));

    const auto entry = std::find_if(proposal.source_map().begin(), proposal.source_map().end(),
                                    [](const auto& candidate) {
                                        return candidate.generated_pointer ==
                                               "/tasks/1/input_bindings/0/from/field";
                                    });
    ASSERT_NE(entry, proposal.source_map().end());
    EXPECT_EQ(entry->authored.source_id, "planner:turn-6");
    EXPECT_EQ(entry->authored.json_pointer, "/response/sources-field");
}

TEST(TaskGraphProposalTest, RejectsUnapprovedTemplateAtMappedSourceCoordinate) {
    auto document = valid_proposal();
    document["tasks"][0]["template"] = "unapproved/v1";

    auto options = proposal_options();
    options.source_map.push_back(
        {"/tasks/0/template", {"planner:turn-7", "/proposal/template", std::nullopt}});
    const auto diagnostics = proposal_errors(std::move(document), std::move(options));

    const auto& diagnostic = diagnostic_with_code(diagnostics, "P_PROPOSAL_TEMPLATE");
    EXPECT_EQ(diagnostic.primary.source_id, "planner:turn-7");
    EXPECT_EQ(diagnostic.primary.json_pointer, "/proposal/template");
}

TEST(TaskGraphProposalTest, RejectsUntypedBindingsAndPlannerAuthorityFields) {
    auto document = valid_proposal();
    document["tasks"][0]["endpoint"] = "https://untrusted.example/run";
    document["tasks"][0]["input_bindings"][0]["from"]["field"] = "/unapproved";
    document["tasks"][0]["input_bindings"][0]["to"]["field"] = "/unapproved";

    const auto diagnostics = proposal_errors(std::move(document));

    EXPECT_TRUE(contains_code(diagnostics, "P_PROPOSAL_UNKNOWN_FIELD"));
    EXPECT_TRUE(contains_code(diagnostics, "P_PROPOSAL_BINDING_OUTPUT"));
    EXPECT_TRUE(contains_code(diagnostics, "P_PROPOSAL_BINDING_TARGET"));
}

TEST(TaskGraphProposalTest, RejectsCyclesAndBudgetOrTopologyLimitViolations) {
    auto document = valid_proposal();
    document["tasks"][0]["depends_on"] = json::array({"research-api"});
    document["tasks"][1]["depends_on"] = json::array({"review-security"});
    document["tasks"][1]["budget"] = json{{"wall_time_ms", 60001}, {"model_tokens", 12000}};

    auto options = proposal_options();
    options.limits.max_depth = 1;
    const auto diagnostics = proposal_errors(std::move(document), std::move(options));

    EXPECT_TRUE(contains_code(diagnostics, "P_PROPOSAL_CYCLE"));
    EXPECT_TRUE(contains_code(diagnostics, "P_PROPOSAL_LIMIT_TASK_BUDGET"));
}

TEST(TaskGraphProposalTest, RejectsOverflowingAggregateBudget) {
    auto document = valid_proposal();
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    document["tasks"][0]["budget"] = json{{"wall_time_ms", maximum}};
    document["tasks"][1]["budget"] = json{{"wall_time_ms", maximum}};

    auto options = proposal_options();
    options.limits.per_task_budget_ceiling.wall_time_ms = maximum;
    options.limits.total_budget_ceiling.wall_time_ms = maximum;
    const auto diagnostics = proposal_errors(std::move(document), std::move(options));

    EXPECT_TRUE(contains_code(diagnostics, "P_PROPOSAL_LIMIT_TOTAL_BUDGET"));
}

TEST(TaskGraphProposalTest, PreservesOriginalBindingCoordinatesAfterMalformedEntries) {
    auto document = valid_proposal();
    const auto valid_binding = document["tasks"][0]["input_bindings"][0];
    document["tasks"][0]["input_bindings"] = json::array({json::object(), valid_binding});
    document["tasks"][0]["input_bindings"][1]["from"]["field"] = "/unapproved";

    auto options = proposal_options();
    options.source_map.push_back({"/tasks/0/input_bindings/1/from/field",
                                  {"planner:turn-8", "/response/binding", std::nullopt}});
    const auto diagnostics = proposal_errors(std::move(document), std::move(options));

    const auto& diagnostic = diagnostic_with_code(diagnostics, "P_PROPOSAL_BINDING_OUTPUT");
    EXPECT_EQ(diagnostic.primary.source_id, "planner:turn-8");
    EXPECT_EQ(diagnostic.primary.json_pointer, "/response/binding");
}


namespace {

TaskGraphFragmentCompileOptions fragment_compile_options() {
    const auto base = proposal_options();
    TaskGraphFragmentCompileOptions options;
    options.owner_scope                  = "tenant-a";
    options.parent_run_id                = "run-parent";
    options.expansion_operation_id       = "op-expand";
    options.parent_program_version_id    = "version-parent";
    options.child_depth                  = 0;
    options.remaining_budget.wall_time_ms = 90000;
    options.remaining_budget.model_tokens = 16000;
    options.remaining_budget.monetary_microunits = 1500000;
    options.remaining_budget.max_child_depth = 4;
    options.remaining_budget.max_total_children = 4;
    options.limits = base.limits;
    options.template_allowlist = base.template_allowlist;
    for (std::size_t index = 0; index < options.template_allowlist.size(); ++index) {
        options.template_allowlist[index].child_binding =
            "child-" + std::to_string(index);
        options.template_allowlist[index].executable_identity =
            "worker-" + std::to_string(index);
        options.template_allowlist[index].kind = "node";
    }
    return options;
}

CompiledTaskGraphFragment compiled_fragment() {
    const auto proposal = TaskGraphProposal::parse(valid_proposal(), proposal_options());
    return TaskGraphFragmentCompiler::compile(proposal, fragment_compile_options());
}

TaskGraphFragmentRecord fragment_record(const CompiledTaskGraphFragment& fragment) {
    TaskGraphFragmentRecord record{fragment, {}, 0, false, false};
    for (const auto& task : fragment.tasks()) {
        record.tasks.push_back(
            TaskGraphTaskRecord{task.task_id, task.operation_id, TaskGraphTaskState::Pending, 0,
                                 std::nullopt, std::nullopt});
    }
    return record;
}

}  // namespace

TEST(TaskGraphFragmentTest, RoundTripsCanonicalFragmentAndPreservesSourceMetadata) {
    const auto fragment = compiled_fragment();

    EXPECT_EQ(fragment.parent_program_version_id(), "version-parent");
    EXPECT_EQ(fragment.child_depth(), 0U);
    const auto parsed = CompiledTaskGraphFragment::parse(fragment.serialize_canonical());
    EXPECT_EQ(parsed, fragment);
    EXPECT_EQ(parsed.serialize_canonical(), fragment.serialize_canonical());
}

TEST(TaskGraphFragmentTest, RejectsTamperedOperationIdentity) {
    auto tampered = compiled_fragment().to_json();
    tampered["tasks"][0]["operation_id"] = "sha256:" + std::string(64, 'f');
    EXPECT_THROW(CompiledTaskGraphFragment::parse(tampered.dump()), std::invalid_argument);
}

TEST(TaskGraphFragmentTest, PublishesAndUpdatesOnlyThroughDurableCompareAndSwap) {
    const auto fragment = compiled_fragment();
    const auto record = fragment_record(fragment);
    InMemoryTaskGraphFragmentStore store;

    EXPECT_EQ(store.publish(record), TaskGraphPublishResult::Published);
    const auto first = store.load(fragment.fragment_id());
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(first->published);
    EXPECT_EQ(first->revision, 1U);
    EXPECT_EQ(store.publish(record), TaskGraphPublishResult::AlreadyPresent);
    auto retry = *first;
    retry.tasks[0].state = TaskGraphTaskState::Completed;
    retry.tasks[0].output = json{{"summary", "done"}};
    retry.terminal = true;
    EXPECT_EQ(store.publish(retry), TaskGraphPublishResult::AlreadyPresent);
    const auto after_retry = store.load(fragment.fragment_id());
    ASSERT_TRUE(after_retry.has_value());
    EXPECT_EQ(after_retry->revision, 1U);
    EXPECT_FALSE(after_retry->terminal);

    auto next = *first;
    next.tasks[0].state = TaskGraphTaskState::Completed;
    next.tasks[0].output = json{{"summary", "done"}};
    next.terminal = true;
    EXPECT_EQ(store.compare_update(fragment.fragment_id(), 1, next),
              TaskGraphPublishResult::Published);
    EXPECT_EQ(store.compare_update(fragment.fragment_id(), 1, next),
              TaskGraphPublishResult::Conflict);

    const auto updated = store.load(fragment.fragment_id());
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->revision, 2U);
    EXPECT_TRUE(updated->terminal);
    EXPECT_EQ(updated->tasks[0].state, TaskGraphTaskState::Completed);
    EXPECT_EQ(TaskGraphFragmentRecord::parse(updated->serialize_canonical()), *updated);
}

TEST(TaskGraphFragmentTest, RejectsRecordTaskSetOrOperationMismatches) {
    auto record = fragment_record(compiled_fragment());
    record.tasks[0].operation_id = "sha256:" + std::string(64, 'e');
    EXPECT_THROW(TaskGraphFragmentRecord::parse(record.serialize_canonical()),
                 std::invalid_argument);
}