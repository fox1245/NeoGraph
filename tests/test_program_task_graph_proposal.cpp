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
#include "fixtures/store_handle.h"

using neograph::program::test_support::OwnedFragmentStore;

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

// Schema 2 states every budget dimension the template can spend.
json valid_proposal_v2() {
    auto document = valid_proposal();
    document["schema_version"] = 2;
    document["tasks"][0]["budget"] = json{{"wall_time_ms", 30000},
                                          {"model_tokens", 4000},
                                          {"monetary_microunits", 400000},
                                          {"output_bytes", 2048}};
    document["tasks"][1]["budget"] = json{{"wall_time_ms", 60000},
                                          {"model_tokens", 12000},
                                          {"monetary_microunits", 1000000},
                                          {"output_bytes", 4096}};
    return document;
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

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    EXPECT_TRUE(input.is_open()) << "cannot open " << path;
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

json without_source_map(const json& document) {
    json result = json::object();
    for (const auto& [key, value] : document.items()) {
        if (key != "source_map") result[key] = value;
    }
    return result;
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

// #412. The aggregate must count what each launched task receives. A task that leaves a
// dimension at 0 is counted as 0 by the sums but, in a schema-1 fragment, is launched with the
// link's whole ceiling. Schema 2 requires the dimension to be stated.
TEST(TaskGraphProposalTest, SchemaOneKeepsItsCanonicalBytesAndIdentity) {
    const auto proposal = TaskGraphProposal::parse(valid_proposal(), proposal_options());

    EXPECT_EQ(proposal.schema_version(), 1U);
    // Derived independently of the library (sorted-key compact JSON, then the identity digest).
    EXPECT_EQ(
        proposal.serialize_canonical(),
        R"({"join":{"kind":"all"},"schema_version":1,"tasks":[{"budget":{"model_tokens":12000,"monetary_microunits":0,"output_bytes":0,"wall_time_ms":60000},"depends_on":[],"id":"research-api","input_bindings":[],"template":"researcher/v3"},{"budget":{"model_tokens":4000,"monetary_microunits":0,"output_bytes":0,"wall_time_ms":30000},"depends_on":["research-api"],"id":"review-security","input_bindings":[{"from":{"artifact":"findings","field":"/summary","task":"research-api"},"to":{"field":"/candidate_summary"}}],"template":"security-reviewer/v2"}]})");
    EXPECT_EQ(proposal.id(),
              "sha256:eca11e4afc04106a713c8a61fe82e63d3774f1924bf544bf764427249a3d9e1a");
}

TEST(TaskGraphProposalTest, SchemaTwoAcceptsExplicitBudgetsAndRecordsItsVersion) {
    const auto proposal = TaskGraphProposal::parse(valid_proposal_v2(), proposal_options());

    EXPECT_EQ(proposal.schema_version(), 2U);
    EXPECT_EQ(proposal.to_json()["schema_version"], 2);
    EXPECT_NE(proposal.id(),
              TaskGraphProposal::parse(valid_proposal(), proposal_options()).id());
    EXPECT_EQ(proposal.tasks()[0].id, "research-api");
    EXPECT_EQ(proposal.tasks()[0].budget, budget(60000, 12000, 1000000, 4096));
}

TEST(TaskGraphProposalTest, SchemaTwoRequiresEveryDimensionTheTemplateCanSpend) {
    auto document              = valid_proposal();  // monetary_microunits and output_bytes unset
    document["schema_version"] = 2;

    const auto diagnostics = proposal_errors(std::move(document));

    std::vector<std::string> pointers;
    for (const auto& diagnostic : diagnostics) {
        EXPECT_EQ(diagnostic.code, "P_PROPOSAL_BUDGET_ZERO");
        pointers.push_back(diagnostic.primary.json_pointer);
    }
    std::sort(pointers.begin(), pointers.end());
    EXPECT_EQ(pointers,
              (std::vector<std::string>{"/tasks/0/budget/monetary_microunits",
                                        "/tasks/0/budget/output_bytes",
                                        "/tasks/1/budget/monetary_microunits",
                                        "/tasks/1/budget/output_bytes"}));
    EXPECT_EQ(diagnostics[0].witness["template_ceiling"].is_number_unsigned(), true);
}

TEST(TaskGraphProposalTest, SchemaTwoAcceptsZeroWhereTheTemplateCeilingIsZero) {
    auto options = proposal_options();
    for (auto& contract : options.template_allowlist)
        contract.budget_ceiling.monetary_microunits = 0;
    auto document = valid_proposal_v2();
    document["tasks"][0]["budget"]["monetary_microunits"] = 0;
    document["tasks"][1]["budget"]["monetary_microunits"] = 0;

    EXPECT_NO_THROW((void)TaskGraphProposal::parse(document, options));

    // Stating an amount the template cannot spend is still a ceiling violation.
    document["tasks"][0]["budget"]["monetary_microunits"] = 1;
    EXPECT_TRUE(contains_code(proposal_errors(document, options), "P_PROPOSAL_LIMIT_TASK_BUDGET"));
}

TEST(TaskGraphProposalTest, UnsetDimensionsHideAnAggregateOverrunOnlyInSchemaOne) {
    // Three tasks may each spend up to 1,000,000 monetary microunits, and the plan may spend
    // 1,500,000. Leaving the dimension unset sums to 0 under schema 1.
    auto options = proposal_options();
    options.limits.max_tasks = 4;
    auto document            = json{{"schema_version", 1},
                                    {"tasks", json::array()},
                                    {"join", json{{"kind", "all"}}}};
    for (const auto* id : {"a", "b", "c"}) {
        document["tasks"].push_back(json{{"id", id},
                                         {"template", "researcher/v3"},
                                         {"input_bindings", json::array()},
                                         {"depends_on", json::array()},
                                         {"budget", json{{"wall_time_ms", 1000}, {"model_tokens", 10}}}});
    }

    const auto legacy = TaskGraphProposal::parse(document, options);
    TaskGraphBudget counted;
    for (const auto& task : legacy.tasks()) counted.monetary_microunits += task.budget.monetary_microunits;
    EXPECT_EQ(counted.monetary_microunits, 0U);  // schema 1 is unchanged: the hole #412 describes

    document["schema_version"] = 2;
    const auto unset = proposal_errors(document, options);
    ASSERT_EQ(unset.size(), 6U);  // monetary_microunits and output_bytes of three tasks
    for (const auto& diagnostic : unset) EXPECT_EQ(diagnostic.code, "P_PROPOSAL_BUDGET_ZERO");

    for (std::size_t index = 0; index < document["tasks"].size(); ++index) {
        document["tasks"][index]["budget"]["monetary_microunits"] = 1000000;
        document["tasks"][index]["budget"]["output_bytes"]        = 1024;
    }
    const auto stated = proposal_errors(document, options);
    ASSERT_EQ(stated.size(), 1U);
    EXPECT_EQ(stated[0].code, "P_PROPOSAL_LIMIT_TOTAL_BUDGET");  // 3,000,000 > 1,500,000
    EXPECT_EQ(stated[0].witness["resource"], "monetary_microunits");
}

TEST(TaskGraphProposalTest, HostMinimumSchemaRefusesSchemaOne) {
    auto options                     = proposal_options();
    options.limits.min_schema_version = 2;

    const auto diagnostics = proposal_errors(valid_proposal(), options);
    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics[0].code, "P_PROPOSAL_SCHEMA_VERSION");
    EXPECT_EQ(diagnostics[0].witness["minimum"], 2);

    EXPECT_NO_THROW((void)TaskGraphProposal::parse(valid_proposal_v2(), options));

    options.limits.min_schema_version = 3;
    EXPECT_THROW((void)TaskGraphProposal::parse(valid_proposal_v2(), options),
                 std::invalid_argument);
}

TEST(TaskGraphProposalTest, UnknownSchemaVersionIsRefused) {
    auto document              = valid_proposal();
    document["schema_version"] = 3;
    const auto diagnostics     = proposal_errors(std::move(document));
    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics[0].code, "P_PROPOSAL_SCHEMA_VERSION");
    EXPECT_EQ(diagnostics[0].witness["supported"], json::array({1, 2}));
}

// ---- Schema 2: literal inputs and attempts ----

namespace {

TaskGraphProposalOptions literal_options() {
    auto options = proposal_options();
    options.limits.max_attempts            = 3;
    options.limits.max_literal_bytes       = 64;
    options.limits.max_total_literal_bytes = 100;
    return options;
}

json literal_proposal() {
    return json{
        {"schema_version", 2},
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
                  {"budget", json{{"wall_time_ms", 20000}, {"model_tokens", 3000},
                                  {"monetary_microunits", 300000}, {"output_bytes", 1024}}}},
             json{{"id", "research-api"},
                  {"template", "researcher/v3"},
                  {"input_bindings", json::array()},
                  {"depends_on", json::array()},
                  {"budget", json{{"wall_time_ms", 30000}, {"model_tokens", 4000},
                                  {"monetary_microunits", 400000}, {"output_bytes", 2048}}},
                  {"literals", json::array({json{{"to", json{{"field", "/question"}}},
                                                 {"value", "What changed?"}}})},
                  {"max_attempts", 2}},
         })},
        {"join", json{{"kind", "all"}}},
    };
}

}  // namespace

TEST(TaskGraphProposalTest, LiteralsAndAttemptsHaveACanonicalFormAndIdentity) {
    const auto proposal = TaskGraphProposal::parse(literal_proposal(), literal_options());

    EXPECT_EQ(proposal.schema_version(), 2U);
    ASSERT_EQ(proposal.tasks().size(), 2U);
    EXPECT_EQ(proposal.tasks()[0].id, "research-api");
    EXPECT_EQ(proposal.tasks()[0].max_attempts, 2U);
    ASSERT_EQ(proposal.tasks()[0].literals.size(), 1U);
    EXPECT_EQ(proposal.tasks()[0].literals[0].to.field, "/question");
    EXPECT_EQ(proposal.tasks()[0].literals[0].value, json("What changed?"));
    EXPECT_EQ(proposal.tasks()[1].max_attempts, 1U);
    // Derived independently of the library (sorted-key compact JSON, then the identity digest).
    EXPECT_EQ(
        proposal.serialize_canonical(),
        R"({"join":{"kind":"all"},"schema_version":2,"tasks":[{"budget":{"model_tokens":4000,"monetary_microunits":400000,"output_bytes":2048,"wall_time_ms":30000},"depends_on":[],"id":"research-api","input_bindings":[],"literals":[{"to":{"field":"/question"},"value":"What changed?"}],"max_attempts":2,"template":"researcher/v3"},{"budget":{"model_tokens":3000,"monetary_microunits":300000,"output_bytes":1024,"wall_time_ms":20000},"depends_on":["research-api"],"id":"review-security","input_bindings":[{"from":{"artifact":"findings","field":"/summary","task":"research-api"},"to":{"field":"/candidate_summary"}}],"template":"security-reviewer/v2"}]})");
    EXPECT_EQ(proposal.id(), "sha256:366f001849e308ae5663257fb238b34bd5f72f6cbadc50d6af7a8df04932dd38");
    // The canonical form parses back to itself.
    EXPECT_EQ(TaskGraphProposal::parse(proposal.to_json(), literal_options()).id(), proposal.id());
}

TEST(TaskGraphProposalTest, LiteralsAreOrderedByTargetAndMayHoldAnyJsonValue) {
    auto options = literal_options();
    options.template_allowlist[0].input_fields = {"/question", "/limit", "/filters", "/flag"};
    auto document = literal_proposal();
    json literals = json::array();
    for (const auto& pair : std::vector<std::pair<std::string, json>>{
             {"/question", "q"}, {"/limit", 3}, {"/filters", json::array({1, json{{"a", nullptr}}})},
             {"/flag", nullptr}}) {
        literals.push_back(json{{"to", json{{"field", pair.first}}}, {"value", pair.second}});
    }
    document["tasks"][1]["literals"] = literals;
    json reversed = json::array();
    for (std::size_t index = literals.size(); index-- > 0;) reversed.push_back(literals[index]);
    auto other = document;
    other["tasks"][1]["literals"] = reversed;

    const auto first = TaskGraphProposal::parse(document, options);
    EXPECT_EQ(first.id(), TaskGraphProposal::parse(other, options).id());
    const auto& stored = first.tasks()[0].literals;
    ASSERT_EQ(stored.size(), 4U);
    EXPECT_EQ(stored[0].to.field, "/filters");
    EXPECT_EQ(stored[1].to.field, "/flag");
    EXPECT_TRUE(stored[1].value.is_null());
    EXPECT_EQ(stored[2].to.field, "/limit");
    EXPECT_EQ(stored[3].to.field, "/question");
}

TEST(TaskGraphProposalTest, SchemaTwoMembersUnderSchemaOneAreASchemaVersionError) {
    auto document              = literal_proposal();
    document["schema_version"] = 1;
    for (std::size_t index = 0; index < document["tasks"].size(); ++index) {
        document["tasks"][index]["budget"] = json{{"wall_time_ms", 1000}, {"model_tokens", 1}};
    }
    const auto diagnostics = proposal_errors(document, literal_options());

    std::vector<std::string> pointers;
    for (const auto& diagnostic : diagnostics) {
        EXPECT_EQ(diagnostic.code, "P_PROPOSAL_SCHEMA_VERSION");
        pointers.push_back(diagnostic.primary.json_pointer);
    }
    std::sort(pointers.begin(), pointers.end());
    EXPECT_EQ(pointers, (std::vector<std::string>{"/tasks/1/literals", "/tasks/1/max_attempts"}));
}

TEST(TaskGraphProposalTest, ALiteralTargetMustBeADeclaredInputThatHasNoOtherValue) {
    auto document = literal_proposal();
    // Not an input of the template.
    document["tasks"][1]["literals"][0]["to"]["field"] = "/unapproved";
    EXPECT_TRUE(contains_code(proposal_errors(document, literal_options()),
                              "P_PROPOSAL_LITERAL_TARGET"));

    // Also the target of a binding.
    document = literal_proposal();
    document["tasks"][0]["literals"] =
        json::array({json{{"to", json{{"field", "/candidate_summary"}}}, {"value", "x"}}});
    const auto bound = proposal_errors(document, literal_options());
    EXPECT_TRUE(contains_code(bound, "P_PROPOSAL_LITERAL_DUPLICATE_TARGET"));
    EXPECT_EQ(diagnostic_with_code(bound, "P_PROPOSAL_LITERAL_DUPLICATE_TARGET").primary.json_pointer,
              "/tasks/0/literals/0/to/field");

    // Two literals for one field.
    document = literal_proposal();
    document["tasks"][1]["literals"].push_back(
        json{{"to", json{{"field", "/question"}}}, {"value", "again"}});
    EXPECT_TRUE(contains_code(proposal_errors(document, literal_options()),
                              "P_PROPOSAL_LITERAL_DUPLICATE_TARGET"));
}

TEST(TaskGraphProposalTest, LiteralSizesAreBoundedPerLiteralAndInTotal) {
    auto document = literal_proposal();
    document["tasks"][1]["literals"][0]["value"] = std::string(80, 'x');
    const auto single = proposal_errors(document, literal_options());
    ASSERT_TRUE(contains_code(single, "P_PROPOSAL_LITERAL_SIZE"));
    EXPECT_EQ(diagnostic_with_code(single, "P_PROPOSAL_LITERAL_SIZE").primary.json_pointer,
              "/tasks/1/literals/0/value");

    auto options = literal_options();
    options.limits.max_total_literal_bytes = 10;
    const auto total = proposal_errors(literal_proposal(), options);
    ASSERT_TRUE(contains_code(total, "P_PROPOSAL_LITERAL_SIZE"));
    EXPECT_EQ(diagnostic_with_code(total, "P_PROPOSAL_LITERAL_SIZE").primary.json_pointer, "/tasks");

    // A host that sets no literal limit accepts no literal.
    auto none = proposal_options();
    none.limits.max_attempts = 3;
    EXPECT_TRUE(contains_code(proposal_errors(literal_proposal(), none), "P_PROPOSAL_LITERAL_SIZE"));
}

TEST(TaskGraphProposalTest, AttemptsAreBoundedByTheHostCeiling) {
    for (const auto& attempts : {json(0), json(4), json(5000000000ULL)}) {
        auto document = literal_proposal();
        document["tasks"][1]["max_attempts"] = attempts;
        const auto diagnostics = proposal_errors(document, literal_options());
        ASSERT_TRUE(contains_code(diagnostics, "P_PROPOSAL_ATTEMPTS")) << attempts.dump();
        EXPECT_EQ(diagnostic_with_code(diagnostics, "P_PROPOSAL_ATTEMPTS").primary.json_pointer,
                  "/tasks/1/max_attempts");
    }
    auto text = literal_proposal();
    text["tasks"][1]["max_attempts"] = "2";
    EXPECT_TRUE(contains_code(proposal_errors(text, literal_options()), "P_PROPOSAL_TYPE"));

    // The default ceiling is one attempt: no retries unless the host allows them.
    auto limits = literal_options();
    limits.limits.max_attempts = 1;
    EXPECT_TRUE(contains_code(proposal_errors(literal_proposal(), limits), "P_PROPOSAL_ATTEMPTS"));
}

TEST(TaskGraphProposalTest, TheAggregateReservesTheBudgetOncePerAttempt) {
    // Both tasks fit the total once; the task with two attempts does not fit twice.
    auto options = literal_options();
    options.limits.total_budget_ceiling = budget(60000, 16000, 1500000, 8192);
    const auto diagnostics = proposal_errors(literal_proposal(), options);
    ASSERT_TRUE(contains_code(diagnostics, "P_PROPOSAL_LIMIT_TOTAL_BUDGET"));
    const auto& wall = diagnostic_with_code(diagnostics, "P_PROPOSAL_LIMIT_TOTAL_BUDGET");
    EXPECT_EQ(wall.witness["resource"], "wall_time_ms");
    EXPECT_EQ(wall.witness["requested"], 80000);  // 2 x 30000 + 20000

    auto single = literal_proposal();
    single["tasks"][1]["max_attempts"] = 1;
    EXPECT_NO_THROW((void)TaskGraphProposal::parse(single, options));
}

TEST(TaskGraphProposalTest, LiteralsAndAttemptsKeepTheirAuthoredCoordinates) {
    auto options = literal_options();
    options.source_map.push_back({"/tasks/1/literals/0/value",
                                  {"planner:turn-9", "/steps/0/params/question", std::nullopt}});
    options.source_map.push_back({"/tasks/1/max_attempts",
                                  {"planner:turn-9", "/steps/0/retry/max_attempts", std::nullopt}});
    const auto proposal = TaskGraphProposal::parse(literal_proposal(), options);

    const auto authored = [&](std::string_view generated) -> std::string {
        for (const auto& entry : proposal.source_map())
            if (entry.generated_pointer == generated) return entry.authored.json_pointer;
        return "<none>";
    };
    // research-api is the first canonical task although it is declared second.
    EXPECT_EQ(authored("/tasks/0/literals/0/value"), "/steps/0/params/question");
    EXPECT_EQ(authored("/tasks/0/max_attempts"), "/steps/0/retry/max_attempts");
    EXPECT_EQ(authored("/tasks/0/literals/0/to/field"), "/tasks/1/literals/0/to/field");
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
    OwnedFragmentStore<InMemoryTaskGraphFragmentStore> store;

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
TEST(TaskGraphFragmentTest, RecoversPublishedFragmentByParentLineage) {
    const auto fragment = compiled_fragment();
    OwnedFragmentStore<InMemoryTaskGraphFragmentStore> store;
    ASSERT_EQ(store.publish(fragment_record(fragment)), TaskGraphPublishResult::Published);

    const auto recovered = store.find_published(
        fragment.owner_scope(), fragment.parent_run_id(), fragment.expansion_operation_id(),
        fragment.proposal_hash());
    ASSERT_TRUE(recovered.has_value());
    EXPECT_TRUE(recovered->published);
    EXPECT_EQ(recovered->fragment, fragment);

    EXPECT_FALSE(store.find_published(
        fragment.owner_scope(), fragment.parent_run_id(), "op-other", fragment.proposal_hash()));
}

TEST(TaskGraphFragmentTest, RejectsRecordTaskSetOrOperationMismatches) {
    auto record = fragment_record(compiled_fragment());
    record.tasks[0].operation_id = "sha256:" + std::string(64, 'e');
    EXPECT_THROW(TaskGraphFragmentRecord::parse(record.serialize_canonical()),
                 std::invalid_argument);
}

TEST(TaskGraphFragmentTest, SchemaOneFragmentKeepsItsStoredBytesAndIdentity) {
    // tests/fixtures/task_graph_fragment/v1_fragment.json was derived independently of the
    // library (sorted-key compact JSON and the identity digests), not captured from compile().
    const auto stored = read_text_file(std::filesystem::path(__FILE__).parent_path() /
                                       "fixtures" / "task_graph_fragment" / "v1_fragment.json");
    const auto parsed = CompiledTaskGraphFragment::parse(stored);
    const auto fragment = compiled_fragment();

    EXPECT_EQ(parsed.schema_version(), 1U);
    EXPECT_EQ(fragment.schema_version(), 1U);
    EXPECT_EQ(fragment.fragment_id(), parsed.fragment_id());
    EXPECT_EQ(fragment.fragment_id(),
              "sha256:35a9f1a77b82038ee62a60e4ad9afd89be8d13d005e0ab1eca2efbbe133d609f");
    ASSERT_EQ(fragment.tasks().size(), parsed.tasks().size());
    for (std::size_t index = 0; index < parsed.tasks().size(); ++index)
        EXPECT_EQ(fragment.tasks()[index], parsed.tasks()[index]);
    EXPECT_EQ(without_source_map(fragment.to_json()), without_source_map(parsed.to_json()));
}

TEST(TaskGraphFragmentTest, SchemaTwoProposalCompilesToSchemaTwoFragmentWithExactReservations) {
    const auto proposal = TaskGraphProposal::parse(valid_proposal_v2(), proposal_options());
    const auto fragment = TaskGraphFragmentCompiler::compile(proposal, fragment_compile_options());

    EXPECT_EQ(fragment.schema_version(), 2U);
    EXPECT_NE(fragment.fragment_id(), compiled_fragment().fragment_id());
    TaskGraphBudget reserved;
    for (const auto& task : fragment.tasks()) {
        EXPECT_EQ(task.reserved_budget, task.requested_budget);
        reserved.wall_time_ms += task.reserved_budget.wall_time_ms;
        reserved.model_tokens += task.reserved_budget.model_tokens;
        reserved.monetary_microunits += task.reserved_budget.monetary_microunits;
        reserved.output_bytes += task.reserved_budget.output_bytes;
    }
    EXPECT_EQ(fragment.aggregate_budget(), reserved);
    const auto parsed = CompiledTaskGraphFragment::parse(fragment.serialize_canonical());
    EXPECT_EQ(parsed, fragment);
    EXPECT_EQ(parsed.schema_version(), 2U);
}

TEST(TaskGraphFragmentTest, CompileRefusesASchemaTwoTaskThatLeavesASpendableDimensionAtZero) {
    // Parse under a template that cannot spend money, then compile under one that can: the
    // compiler repeats the check against the trusted list it is given.
    auto parse_options = proposal_options();
    for (auto& contract : parse_options.template_allowlist)
        contract.budget_ceiling.monetary_microunits = 0;
    auto document = valid_proposal_v2();
    document["tasks"][0]["budget"]["monetary_microunits"] = 0;
    document["tasks"][1]["budget"]["monetary_microunits"] = 0;
    const auto proposal = TaskGraphProposal::parse(document, parse_options);

    EXPECT_THROW((void)TaskGraphFragmentCompiler::compile(proposal, fragment_compile_options()),
                 std::invalid_argument);
}

TEST(TaskGraphFragmentTest, CompilingLiteralsAndAttemptsReservesTheBudgetOncePerAttempt) {
    auto proposal_opts = literal_options();
    const auto proposal = TaskGraphProposal::parse(literal_proposal(), proposal_opts);
    auto compile = fragment_compile_options();
    compile.limits = proposal_opts.limits;
    compile.remaining_budget.wall_time_ms = 200000;
    compile.remaining_budget.monetary_microunits = 3000000;
    const auto fragment = TaskGraphFragmentCompiler::compile(proposal, compile);

    EXPECT_EQ(fragment.schema_version(), 2U);
    ASSERT_EQ(fragment.tasks().size(), 2U);
    const auto& research = fragment.tasks()[0];
    EXPECT_EQ(research.task_id, "research-api");
    EXPECT_EQ(research.max_attempts, 2U);
    EXPECT_EQ(research.requested_budget, budget(30000, 4000, 400000, 2048));
    EXPECT_EQ(research.reserved_budget, budget(60000, 8000, 800000, 4096));
    ASSERT_EQ(research.input_literals.size(), 1U);
    EXPECT_EQ(research.input_literals[0].to.field, "/question");
    const auto& review = fragment.tasks()[1];
    EXPECT_EQ(review.max_attempts, 1U);
    EXPECT_EQ(review.reserved_budget, review.requested_budget);
    EXPECT_EQ(fragment.aggregate_budget(), budget(80000, 11000, 1100000, 5120));

    const auto parsed = CompiledTaskGraphFragment::parse(fragment.serialize_canonical());
    EXPECT_EQ(parsed, fragment);
    EXPECT_EQ(parsed.tasks()[0].input_literals, research.input_literals);
    EXPECT_EQ(parsed.tasks()[0].max_attempts, 2U);
}

TEST(TaskGraphFragmentTest, CompileChecksLiteralsAndAttemptsAgainstItsOwnLimits) {
    auto proposal_opts = literal_options();
    const auto proposal = TaskGraphProposal::parse(literal_proposal(), proposal_opts);
    auto compile = fragment_compile_options();
    compile.limits = proposal_opts.limits;
    compile.remaining_budget.wall_time_ms = 200000;
    compile.remaining_budget.monetary_microunits = 3000000;
    EXPECT_NO_THROW((void)TaskGraphFragmentCompiler::compile(proposal, compile));

    auto fewer_attempts = compile;
    fewer_attempts.limits.max_attempts = 1;
    EXPECT_THROW((void)TaskGraphFragmentCompiler::compile(proposal, fewer_attempts), std::invalid_argument);

    auto smaller_literals = compile;
    smaller_literals.limits.max_literal_bytes = 4;
    EXPECT_THROW((void)TaskGraphFragmentCompiler::compile(proposal, smaller_literals),
                 std::invalid_argument);

    auto other_template = compile;
    other_template.template_allowlist[0].input_fields = {"/other"};
    EXPECT_THROW((void)TaskGraphFragmentCompiler::compile(proposal, other_template),
                 std::invalid_argument);
}

TEST(TaskGraphFragmentTest, StoredFragmentsRefuseSchemaTwoMembersTheyCannotHold) {
    auto proposal_opts = literal_options();
    const auto proposal = TaskGraphProposal::parse(literal_proposal(), proposal_opts);
    auto compile = fragment_compile_options();
    compile.limits = proposal_opts.limits;
    compile.remaining_budget.wall_time_ms = 200000;
    compile.remaining_budget.monetary_microunits = 3000000;
    const auto document = TaskGraphFragmentCompiler::compile(proposal, compile).to_json();

    const auto refusal = [](const json& tampered) {
        try {
            (void)CompiledTaskGraphFragment::parse(tampered.dump());
        } catch (const std::invalid_argument& error) {
            return std::string(error.what());
        }
        return std::string("<accepted>");
    };

    // A reservation that is not the budget once per attempt.
    auto wrong_reservation = document;
    wrong_reservation["tasks"][0]["reserved_budget"]["wall_time_ms"] = 30000;
    EXPECT_NE(refusal(wrong_reservation).find("once per attempt"), std::string::npos);

    // A schema-1 fragment cannot carry them.
    auto downgraded = document;
    downgraded["schema_version"] = 1;
    EXPECT_NE(refusal(downgraded).find("schema-2 task members"), std::string::npos);

    // A literal for a field that is also bound.
    auto doubled = document;
    doubled["tasks"][1]["input_literals"] = json::array(
        {json{{"to", json{{"field", "/candidate_summary"}}}, {"value", "x"}}});
    EXPECT_NE(refusal(doubled).find("also a binding target"), std::string::npos);

    // A literal for a field the template does not accept.
    auto stray = document;
    stray["tasks"][0]["input_literals"][0]["to"]["field"] = "/stray";
    EXPECT_NE(refusal(stray).find("not an input field"), std::string::npos);

    EXPECT_EQ(refusal(document), "<accepted>");
}
