#include <neograph/program/gate.h>

#include "../src/program/canonical_json.h"
#include "../src/core/sha256.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace {

using neograph::json;
using namespace neograph::program;

std::string expected_slot_seed_hex(const GateTestSlot& slot) {
    constexpr char kHexDigits[] = "0123456789abcdef";
    const auto     digest       = neograph::detail::sha256_digest(slot.serialize_canonical());
    std::string    hex;
    hex.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        hex.push_back(kHexDigits[(byte >> 4) & 0x0fu]);
        hex.push_back(kHexDigits[byte & 0x0fu]);
    }
    return hex;
}

GateSandboxLimits make_limits() {
    return GateSandboxLimits{
        2000,
        50000,
        8u * 1024u * 1024u,
        256u * 1024u,
    };
}

GateTestSlotData make_sorting_slot_data() {
    GateTestSlotData data;
    data.slot_id     = "sort-integers-v1";
    data.owner_scope = "host-suite";
    data.public_examples = {
        GatePublicExample{"pub-1", json::array({3, 1, 2}), json::array({1, 2, 3})},
        GatePublicExample{"pub-2", json::array({5, -1, 5, 0}), json::array({-1, 0, 5, 5})},
    };
    data.hidden_cases = {
        GateHiddenCase{"hid-1", json::array({10, 9, 8, 7}), json::array({7, 8, 9, 10}), std::nullopt},
        GateHiddenCase{"hid-2", json::array({42, -100, 0, 7}), std::nullopt, "isSortedPermutation"},
    };
    data.property_module = GatePropertyModule{
        "export function isSortedPermutation(input, actual) {\n"
        "  if (!Array.isArray(input) || !Array.isArray(actual)) return false;\n"
        "  if (input.length !== actual.length) return false;\n"
        "  for (let i = 1; i < actual.length; ++i) {\n"
        "    if (typeof actual[i - 1] !== 'number' || typeof actual[i] !== 'number') return false;\n"
        "    if (actual[i - 1] > actual[i]) return false;\n"
        "  }\n"
        "  const copy = input.slice().sort((a, b) => a - b);\n"
        "  for (let i = 0; i < copy.length; ++i) {\n"
        "    if (copy[i] !== actual[i]) return false;\n"
        "  }\n"
        "  return true;\n"
        "}\n"};
    data.qualification_mutants = {
        GateQualificationMutant{
            "mutant-identity",
            "export function evaluate(input) { return input.slice(); }\n"},
        GateQualificationMutant{
            "mutant-descending",
            "export function evaluate(input) { return input.slice().sort((a, b) => b - a); }\n"},
        GateQualificationMutant{
            "mutant-lexicographic",
            "export function evaluate(input) { return input.slice().sort(); }\n"},
    };
    data.golden_candidates = {
        GateGoldenCandidate{
            "golden-numeric-sort",
            "export function evaluate(input) { return input.slice().sort((a, b) => a - b); }\n"},
    };
    return data;
}

ProgramGateVersion make_gate(const GateTestSlot& slot,
                             GateFeedbackMode    mode = GateFeedbackMode::PublicExample) {
    return ProgramGateVersion::create(ProgramGateData{
        slot.owner_scope(),
        "sort-gate",
        slot.digest(),
        make_limits(),
        GateFeedbackPolicy{mode},
    });
}

TEST(ProgramGateTest, SlotRoundTripDigestsAndValidation) {
    const auto slot_data = make_sorting_slot_data();
    const auto slot      = GateTestSlot::create(slot_data);
    const auto canonical = slot.serialize_canonical();
    const auto reparsed  = GateTestSlot::parse(canonical);

    EXPECT_EQ(reparsed, slot);
    EXPECT_EQ(reparsed.digest(), slot.digest());
    EXPECT_EQ(reparsed.public_digest(), slot.public_digest());
    EXPECT_EQ(reparsed.serialize_canonical(), canonical);
    EXPECT_EQ(reparsed.data(), slot_data);

    auto modified_hidden_data                        = slot_data;
    modified_hidden_data.hidden_cases[0].expected    = json::array({7, 8, 9, 11});
    const auto modified_hidden_slot                  = GateTestSlot::create(modified_hidden_data);
    EXPECT_NE(modified_hidden_slot.digest(), slot.digest());
    EXPECT_EQ(modified_hidden_slot.public_digest(), slot.public_digest());

    const auto gate_v1 = make_gate(slot);
    const auto gate_v2 = make_gate(modified_hidden_slot);
    EXPECT_NE(gate_v1.id(), gate_v2.id());
    EXPECT_EQ(ProgramGateVersion::parse(gate_v1.serialize_canonical()), gate_v1);
    EXPECT_EQ(ProgramGateVersion::parse(gate_v1.serialize_canonical()).id(), gate_v1.id());

    auto with_unknown_field               = json::parse(canonical);
    with_unknown_field["unexpected_flag"] = true;
    EXPECT_THROW(GateTestSlot::parse(with_unknown_field.dump()), std::invalid_argument);

    auto duplicate_case_data                    = slot_data;
    duplicate_case_data.hidden_cases[0].case_id = "pub-1";
    EXPECT_THROW(GateTestSlot::create(duplicate_case_data), std::invalid_argument);

    auto duplicate_hidden_data                    = slot_data;
    duplicate_hidden_data.hidden_cases[1].case_id = "hid-1";
    EXPECT_THROW(GateTestSlot::create(duplicate_hidden_data), std::invalid_argument);

    auto invalid_limits              = make_limits();
    invalid_limits.case_wall_time_ms = 0;
    EXPECT_THROW(
        ProgramGateVersion::create(ProgramGateData{
            slot.owner_scope(), "bad-gate", slot.digest(), invalid_limits,
            GateFeedbackPolicy{GateFeedbackMode::VerdictOnly}}),
        std::invalid_argument);

    auto missing_limit_json = json::parse(gate_v1.serialize_canonical());
    missing_limit_json["sandbox_limits"] = json{
        {"case_wall_time_ms", 2000},
        {"case_memory_limit_bytes", 8u * 1024u * 1024u},
        {"case_max_stack_bytes", 256u * 1024u},
    };
    EXPECT_THROW(ProgramGateVersion::parse(missing_limit_json.dump()), std::invalid_argument);

    const GateSandboxLimits large_stack_limits{
        500,
        1000,
        512u * 1024u,
        1024u * 1024u,
    };
    const auto large_stack_gate = ProgramGateVersion::create(ProgramGateData{
        slot.owner_scope(),
        "large-stack-gate",
        slot.digest(),
        large_stack_limits,
        GateFeedbackPolicy{GateFeedbackMode::VerdictOnly},
    });
    EXPECT_EQ(ProgramGateVersion::parse(large_stack_gate.serialize_canonical()), large_stack_gate);

    auto null_optional_slot               = json::parse(canonical);
    null_optional_slot["case_generator"]  = nullptr;
    EXPECT_THROW(GateTestSlot::parse(null_optional_slot.dump()), std::invalid_argument);

    auto null_property_module_slot                 = json::parse(canonical);
    null_property_module_slot["property_module"]   = nullptr;
    EXPECT_THROW(GateTestSlot::parse(null_property_module_slot.dump()), std::invalid_argument);

    auto null_example_input_slot                              = json::parse(canonical);
    null_example_input_slot["public_examples"][0]["input"]    = nullptr;
    EXPECT_THROW(GateTestSlot::parse(null_example_input_slot.dump()), std::invalid_argument);

    auto null_hidden_expected_slot                            = json::parse(canonical);
    null_hidden_expected_slot["hidden_cases"][0]["expected"]  = nullptr;
    EXPECT_THROW(GateTestSlot::parse(null_hidden_expected_slot.dump()), std::invalid_argument);

    const json omitted_mutants_slot{
        {"format", "neograph-program-gate-slot"},
        {"schema_version", 1},
        {"slot_id", "sort-integers-v1"},
        {"owner_scope", "host-suite"},
        {"public_examples", json::parse(canonical)["public_examples"]},
        {"hidden_cases", json::parse(canonical)["hidden_cases"]},
        {"property_module", json::parse(canonical)["property_module"]},
        {"golden_candidates", json::parse(canonical)["golden_candidates"]},
    };
    EXPECT_THROW(GateTestSlot::parse(omitted_mutants_slot.dump()), std::invalid_argument);

    const json omitted_goldens_slot{
        {"format", "neograph-program-gate-slot"},
        {"schema_version", 1},
        {"slot_id", "sort-integers-v1"},
        {"owner_scope", "host-suite"},
        {"public_examples", json::parse(canonical)["public_examples"]},
        {"hidden_cases", json::parse(canonical)["hidden_cases"]},
        {"property_module", json::parse(canonical)["property_module"]},
        {"qualification_mutants", json::parse(canonical)["qualification_mutants"]},
    };
    EXPECT_THROW(GateTestSlot::parse(omitted_goldens_slot.dump()), std::invalid_argument);

    const json raw_valid_slot{
        {"slot_id", "raw-slot-v1"},
        {"owner_scope", "host-suite"},
        {"schema_version", 1},
        {"format", "neograph-program-gate-slot"},
        {"public_examples",
         json::array({json{{"expected", json::array({1, 2})},
                           {"input", json::array({2, 1})},
                           {"case_id", "pub-raw"}}})},
        {"hidden_cases", json::array()},
        {"qualification_mutants", json::array()},
        {"golden_candidates", json::array()},
    };
    EXPECT_EQ(GateTestSlot::parse(raw_valid_slot.dump(2)).serialize_canonical(),
              neograph::detail::canonical_json_bytes(raw_valid_slot));
    EXPECT_EQ(GateTestSlot::parse(canonical).serialize_canonical(), canonical);
}

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)

void expect_diagnostic_code(const std::function<void()>& fn, std::string_view expected_code) {
    bool caught = false;
    try {
        fn();
    } catch (const ProgramDiagnosticError& error) {
        caught = true;
        EXPECT_EQ(error.diagnostic().code, expected_code);
    }
    EXPECT_TRUE(caught) << "Expected ProgramDiagnosticError with code " << expected_code;
}

TEST(ProgramGateTest, CorrectCandidatePassesAndMintsBindingTokenWhileWrongCountsAllFailures) {
    const auto slot = GateTestSlot::create(make_sorting_slot_data());
    const auto gate = make_gate(slot, GateFeedbackMode::PublicExample);

    const auto correct_source = ProgramSource::from_javascript(
        "candidate-sort",
        "export function evaluate(input) {\n"
        "  return input.slice().sort((a, b) => a - b);\n"
        "}\n");
    const auto one_byte_different_source = ProgramSource::from_javascript(
        "candidate-sort",
        "export function evaluate(input) {\n"
        "  return input.slice().sort((a, b) => a - b); \n"
        "}\n");

    GateEvaluator evaluator;
    const auto    pass_eval = evaluator.evaluate(gate, slot, correct_source, "sub-1", 1);
    EXPECT_TRUE(pass_eval.verdict.passed);
    EXPECT_EQ(pass_eval.verdict.total_cases, 4u);
    EXPECT_EQ(pass_eval.verdict.failed_cases, 0u);
    EXPECT_TRUE(pass_eval.verdict.failure_categories.empty());
    ASSERT_TRUE(pass_eval.token.has_value());
    EXPECT_TRUE(pass_eval.token->generator_seed_hex().empty());
    EXPECT_TRUE(pass_eval.token->binds(correct_source));
    EXPECT_TRUE(pass_eval.token->binds_value(json::parse(correct_source.serialize_canonical())));
    EXPECT_FALSE(pass_eval.token->binds(one_byte_different_source));
    EXPECT_FALSE(
        pass_eval.token->binds_value(json::parse(one_byte_different_source.serialize_canonical())));
    EXPECT_EQ(GateVerdictToken::parse(pass_eval.token->serialize_canonical()), *pass_eval.token);

    const auto wrong_source = ProgramSource::from_javascript(
        "candidate-wrong",
        "export function evaluate(input) {\n"
        "  return [999];\n"
        "}\n");
    const auto fail_eval = evaluator.evaluate(gate, slot, wrong_source, "sub-1", 2);
    EXPECT_FALSE(fail_eval.verdict.passed);
    EXPECT_EQ(fail_eval.verdict.total_cases, 4u);
    EXPECT_EQ(fail_eval.verdict.failed_cases, 4u);
    EXPECT_FALSE(fail_eval.token.has_value());
    ASSERT_TRUE(fail_eval.verdict.public_feedback.contains("public_example"));
    EXPECT_EQ(fail_eval.verdict.public_feedback["public_example"]["case_id"], "pub-1");
    EXPECT_EQ(fail_eval.verdict.public_feedback["public_example"]["actual"], json::array({999}));
}

TEST(ProgramGateTest, ModuleScopeStateIsIsolatedPerCaseAndInputIsDeepFrozen) {
    GateTestSlotData data;
    data.slot_id     = "isolation-slot";
    data.owner_scope = "host-suite";
    data.public_examples = {
        GatePublicExample{"c-1", json{{"nested", json{{"v", 10}}}}, json{{"calls", 1}, {"v", 10}}},
        GatePublicExample{"c-2", json{{"nested", json{{"v", 20}}}}, json{{"calls", 1}, {"v", 20}}},
    };
    data.hidden_cases = {
        GateHiddenCase{
            "c-3", json{{"nested", json{{"v", 30}}}}, json{{"calls", 1}, {"v", 30}}, std::nullopt},
    };
    const auto slot = GateTestSlot::create(data);
    const auto gate = make_gate(slot, GateFeedbackMode::FailedCount);

    const auto stateful_candidate = ProgramSource::from_javascript(
        "candidate-stateful",
        "let calls = 0;\n"
        "export function evaluate(input) {\n"
        "  calls += 1;\n"
        "  return { calls, v: input.nested.v };\n"
        "}\n");

    GateEvaluator evaluator;
    const auto    isolated_eval =
        evaluator.evaluate(gate, slot, stateful_candidate, "subject-iso", 1);
    EXPECT_TRUE(isolated_eval.verdict.passed);
    EXPECT_EQ(isolated_eval.verdict.total_cases, 3u);
    EXPECT_EQ(isolated_eval.verdict.failed_cases, 0u);

    const auto mutating_candidate = ProgramSource::from_javascript(
        "candidate-mutating",
        "export function evaluate(input) {\n"
        "  input.nested.v = 999;\n"
        "  return { calls: 1, v: input.nested.v };\n"
        "}\n");
    const auto mutating_eval =
        evaluator.evaluate(gate, slot, mutating_candidate, "subject-iso", 2);
    EXPECT_FALSE(mutating_eval.verdict.passed);
    EXPECT_EQ(mutating_eval.verdict.failed_cases, 3u);
    EXPECT_EQ(mutating_eval.verdict.failure_categories,
              std::vector<std::string>{"runtime_exception"});
}

TEST(ProgramGateTest, HiddenInputAndExceptionMessagesNeverLeakAcrossFeedbackModes) {
    const std::string secret_marker = "TOP_SECRET_HIDDEN_VECTOR_987654321";
    GateTestSlotData  data;
    data.slot_id     = "non-leak-slot";
    data.owner_scope = "host-suite";
    data.public_examples = {
        GatePublicExample{"pub-ok", json{{"k", "public-value"}}, json{{"echo", "public-value"}}},
    };
    data.hidden_cases = {
        GateHiddenCase{"hid-secret",
                       json{{"secret", secret_marker}},
                       json{{"echo", secret_marker}},
                       std::nullopt},
    };
    const auto slot = GateTestSlot::create(data);

    const auto leaking_candidate = ProgramSource::from_javascript(
        "candidate-leak",
        "export function evaluate(input) {\n"
        "  if (input.secret) {\n"
        "    throw new Error('LEAK:' + JSON.stringify(input));\n"
        "  }\n"
        "  return { echo: input.k };\n"
        "}\n");

    GateEvaluator evaluator;
    for (const auto mode : {GateFeedbackMode::VerdictOnly,
                            GateFeedbackMode::FailedCount,
                            GateFeedbackMode::PublicExample,
                            GateFeedbackMode::SanitizedSymptoms}) {
        const auto gate   = make_gate(slot, mode);
        const auto result = evaluator.evaluate(gate, slot, leaking_candidate, "subject-leak", 1);
        EXPECT_FALSE(result.verdict.passed);
        EXPECT_FALSE(result.token.has_value());
        EXPECT_EQ(result.verdict.total_cases, 2u);
        EXPECT_EQ(result.verdict.failed_cases, 1u);

        json encoded_verdict;
        to_json(encoded_verdict, result.verdict);
        const auto serialized_verdict  = encoded_verdict.dump();
        const auto serialized_feedback = result.verdict.public_feedback.dump();
        EXPECT_EQ(serialized_verdict.find(secret_marker), std::string::npos);
        EXPECT_EQ(serialized_verdict.find("LEAK:"), std::string::npos);
        EXPECT_EQ(serialized_verdict.find("hid-secret"), std::string::npos);
        EXPECT_EQ(serialized_feedback.find(secret_marker), std::string::npos);
        EXPECT_EQ(serialized_feedback.find("LEAK:"), std::string::npos);

        if (mode == GateFeedbackMode::VerdictOnly) {
            EXPECT_FALSE(result.verdict.public_feedback.contains("total_cases"));
            EXPECT_FALSE(result.verdict.public_feedback.contains("failed_cases"));
        }
        if (mode == GateFeedbackMode::FailedCount) {
            EXPECT_EQ(result.verdict.public_feedback["total_cases"], 2u);
            EXPECT_EQ(result.verdict.public_feedback["failed_cases"], 1u);
        }
        if (mode == GateFeedbackMode::PublicExample) {
            EXPECT_FALSE(result.verdict.public_feedback.contains("total_cases"));
            EXPECT_FALSE(result.verdict.public_feedback.contains("failed_cases"));
            EXPECT_TRUE(result.verdict.public_feedback["public_example"].is_null());
        }
        if (mode == GateFeedbackMode::SanitizedSymptoms) {
            EXPECT_FALSE(result.verdict.public_feedback.contains("total_cases"));
            EXPECT_FALSE(result.verdict.public_feedback.contains("failed_cases"));
            EXPECT_EQ(result.verdict.public_feedback["failure_categories"],
                      json::array({"runtime_exception"}));
        }
    }
}

namespace {

GateEvaluation evaluate_limit_case(const std::string& mode, const std::string& slot_id) {
    GateTestSlotData data;
    data.slot_id         = slot_id;
    data.owner_scope     = "host-suite";
    data.public_examples = {
        GatePublicExample{"case-" + mode, json{{"mode", mode}}, json{{"ok", true}}},
        GatePublicExample{"case-pass", json{{"mode", "ok"}}, json{{"ok", true}}},
    };
    const auto slot = GateTestSlot::create(data);
    const auto gate = ProgramGateVersion::create(ProgramGateData{
        slot.owner_scope(),
        "limits-gate",
        slot.digest(),
        // The memory and stack of ProgramRuntimeTest's native-memory limit tests, which every CI platform runs.
        GateSandboxLimits{500, 2000, 2u * 1024u * 1024u, 128u * 1024u},
        GateFeedbackPolicy{GateFeedbackMode::SanitizedSymptoms},
    });
    const auto candidate = ProgramSource::from_javascript(
        "candidate-limits",
        "export function evaluate(input) {\n"
        "  if (input.mode === 'loop') {\n"
        "    while (true) {}\n"
        "  }\n"
        "  if (input.mode === 'oom') {\n"
        "    const huge = [];\n"
        "    for (let i = 0; i < 1000000; ++i) {\n"
        "      huge.push('x'.repeat(1024));\n"
        "    }\n"
        "    return { ok: Boolean(huge.length) };\n"
        "  }\n"
        "  return { ok: true };\n"
        "}\n");
    GateEvaluator evaluator;
    return evaluator.evaluate(gate, slot, candidate, "subject-limits", 1);
}

}  // namespace

TEST(ProgramGateTest, TimeoutIsRecordedAndEvaluationContinues) {
    const auto result = evaluate_limit_case("loop", "limits-loop");
    EXPECT_FALSE(result.verdict.passed);
    EXPECT_EQ(result.verdict.total_cases, 2u);
    EXPECT_EQ(result.verdict.failed_cases, 1u);
    EXPECT_FALSE(result.token.has_value());
    EXPECT_EQ(result.verdict.failure_categories, (std::vector<std::string>{"timeout"}));
    EXPECT_EQ(result.verdict.public_feedback["failure_categories"], json::array({"timeout"}));
}

TEST(ProgramGateTest, MemoryLimitIsRecordedAndEvaluationContinues) {
    const auto result = evaluate_limit_case("oom", "limits-oom");
    EXPECT_FALSE(result.verdict.passed);
    EXPECT_EQ(result.verdict.total_cases, 2u);
    EXPECT_EQ(result.verdict.failed_cases, 1u);
    EXPECT_FALSE(result.token.has_value());
    EXPECT_EQ(result.verdict.failure_categories, (std::vector<std::string>{"memory_limit"}));
    EXPECT_EQ(result.verdict.public_feedback["failure_categories"], json::array({"memory_limit"}));
}

TEST(ProgramGateTest, ImportsAreRefusedAndSlotMismatchOrOwnerMismatchRejected) {
    const auto slot = GateTestSlot::create(make_sorting_slot_data());
    const auto gate = make_gate(slot);

    GateEvaluator evaluator;
    const auto    static_import_candidate = ProgramSource::from_javascript(
        "candidate-import",
        "import { helper } from './helper.js';\n"
        "export function evaluate(input) { return helper(input); }\n");
    expect_diagnostic_code(
        [&] { (void)evaluator.evaluate(gate, slot, static_import_candidate, "sub", 1); },
        "P_GATE_SANDBOX_IMPORT_DENIED");

    const auto dynamic_import_candidate = ProgramSource::from_javascript(
        "candidate-dynamic-import",
        "export function evaluate(input) { return import('./helper.js'); }\n");
    expect_diagnostic_code(
        [&] { (void)evaluator.evaluate(gate, slot, dynamic_import_candidate, "sub", 1); },
        "P_GATE_SANDBOX_IMPORT_DENIED");

    auto mismatched_slot_data                        = make_sorting_slot_data();
    mismatched_slot_data.hidden_cases[0].expected    = json::array({0, 1, 2, 3});
    const auto mismatched_slot                       = GateTestSlot::create(mismatched_slot_data);
    const auto valid_candidate = ProgramSource::from_javascript(
        "candidate-ok",
        "export function evaluate(input) { return input.slice().sort((a, b) => a - b); }\n");
    expect_diagnostic_code(
        [&] { (void)evaluator.evaluate(gate, mismatched_slot, valid_candidate, "sub", 1); },
        "P_GATE_SLOT_DIGEST_MISMATCH");

    auto foreign_slot_data        = make_sorting_slot_data();
    foreign_slot_data.owner_scope = "other-owner";
    const auto foreign_slot       = GateTestSlot::create(foreign_slot_data);
    const auto mismatched_owner_gate = ProgramGateVersion::create(ProgramGateData{
        "host-suite",
        "sort-gate",
        foreign_slot.digest(),
        make_limits(),
        GateFeedbackPolicy{GateFeedbackMode::VerdictOnly},
    });
    expect_diagnostic_code(
        [&] {
            (void)evaluator.evaluate(
                mismatched_owner_gate, foreign_slot, valid_candidate, "sub", 1);
        },
        "P_GATE_SLOT_OWNER_MISMATCH");
}

TEST(ProgramGateTest, GeneratedCasesAreDeterministicPerSeedAndDifferAcrossSeeds) {
    GateTestSlotData data;
    data.slot_id     = "gen-slot";
    data.owner_scope = "host-suite";
    data.property_module = GatePropertyModule{
        "export function matchesExpectedSeedA(input, actual) {\n"
        "  return actual === input.seed + ':' + String(input.index);\n"
        "}\n"};
    data.case_generator = GateCaseGenerator{
        "export function generate(seedHex, caseIndex) {\n"
        "  return { seed: seedHex, index: caseIndex };\n"
        "}\n",
        4,
        "matchesExpectedSeedA",
    };
    const auto slot = GateTestSlot::create(data);
    const auto gate = make_gate(slot, GateFeedbackMode::FailedCount);

    const auto seed_a_candidate = ProgramSource::from_javascript(
        "candidate-seed-a",
        "export function evaluate(input) {\n"
        "  if (input.seed !== '0011223344556677') return 'wrong-seed';\n"
        "  return input.seed + ':' + String(input.index);\n"
        "}\n");

    GateEvaluator evaluator;
    const auto    run_a1 =
        evaluator.evaluate(gate, slot, seed_a_candidate, "sub-gen", 1, "0011223344556677");
    const auto run_a2 =
        evaluator.evaluate(gate, slot, seed_a_candidate, "sub-gen", 1, "0011223344556677");
    const auto run_b =
        evaluator.evaluate(gate, slot, seed_a_candidate, "sub-gen", 1, "8899aabbccddeeff");

    EXPECT_TRUE(run_a1.verdict.passed);
    EXPECT_EQ(run_a1.verdict.total_cases, 4u);
    EXPECT_EQ(run_a1, run_a2);
    EXPECT_FALSE(run_b.verdict.passed);
    EXPECT_EQ(run_b.verdict.failed_cases, 4u);

    const auto general_candidate = ProgramSource::from_javascript(
        "candidate-any-seed",
        "export function evaluate(input) {\n"
        "  return input.seed + ':' + String(input.index);\n"
        "}\n");
    const auto token_run_a =
        evaluator.evaluate(gate, slot, general_candidate, "sub-gen", 1, "0011223344556677");
    const auto token_run_b =
        evaluator.evaluate(gate, slot, general_candidate, "sub-gen", 1, "8899aabbccddeeff");
    ASSERT_TRUE(token_run_a.token.has_value());
    ASSERT_TRUE(token_run_b.token.has_value());
    EXPECT_EQ(token_run_a.token->generator_seed_hex(), "0011223344556677");
    EXPECT_EQ(token_run_b.token->generator_seed_hex(), "8899aabbccddeeff");
    EXPECT_NE(token_run_a.token->id(), token_run_b.token->id());
    EXPECT_NE(token_run_a.token->serialize_canonical(), token_run_b.token->serialize_canonical());
    EXPECT_EQ(GateVerdictToken::parse(token_run_a.token->serialize_canonical()),
              *token_run_a.token);

    expect_diagnostic_code(
        [&] { (void)evaluator.evaluate(gate, slot, seed_a_candidate, "sub-gen", 1, ""); },
        "P_GATE_SEED_REQUIRED");

    for (int target_index = 0; target_index < 4; ++target_index) {
        const auto single_index_pass_candidate = ProgramSource::from_javascript(
            "candidate-only-index-" + std::to_string(target_index),
            "export function evaluate(input) {\n"
            "  if (input.index === " +
                std::to_string(target_index) +
                ") return input.seed + ':' + String(input.index);\n"
                "  return 'wrong';\n"
                "}\n");
        const auto run_only_idx = evaluator.evaluate(
            gate, slot, single_index_pass_candidate, "sub-gen", 1, "0011223344556677");
        EXPECT_FALSE(run_only_idx.verdict.passed);
        EXPECT_EQ(run_only_idx.verdict.total_cases, 4u);
        EXPECT_EQ(run_only_idx.verdict.failed_cases, 3u);

        const auto single_index_fail_candidate = ProgramSource::from_javascript(
            "candidate-index-" + std::to_string(target_index),
            "export function evaluate(input) {\n"
            "  if (input.index === " +
                std::to_string(target_index) +
                ") return 'bad';\n"
                "  return input.seed + ':' + String(input.index);\n"
                "}\n");
        const auto run_idx = evaluator.evaluate(
            gate, slot, single_index_fail_candidate, "sub-gen", 1, "0011223344556677");
        EXPECT_FALSE(run_idx.verdict.passed);
        EXPECT_EQ(run_idx.verdict.total_cases, 4u);
        EXPECT_EQ(run_idx.verdict.failed_cases, 1u);
    }

    GateTestSlotData throwing_gen_data = data;
    throwing_gen_data.slot_id          = "gen-throw-slot";
    throwing_gen_data.case_generator   = GateCaseGenerator{
        "export function generate(seedHex, caseIndex) {\n"
        "  if (caseIndex === 1) throw new Error('generator boom');\n"
        "  return { seed: seedHex, index: caseIndex };\n"
        "}\n",
        3,
        "matchesExpectedSeedA",
    };
    const auto throwing_gen_slot = GateTestSlot::create(throwing_gen_data);
    for (const auto mode : {GateFeedbackMode::VerdictOnly,
                            GateFeedbackMode::FailedCount,
                            GateFeedbackMode::PublicExample,
                            GateFeedbackMode::SanitizedSymptoms}) {
        const auto throwing_gen_gate = make_gate(throwing_gen_slot, mode);
        const auto throwing_gen_run  = evaluator.evaluate(
            throwing_gen_gate, throwing_gen_slot, seed_a_candidate, "sub-gen", 1, "0011223344556677");
        EXPECT_FALSE(throwing_gen_run.verdict.passed);
        EXPECT_EQ(throwing_gen_run.verdict.total_cases, 3u);
        EXPECT_EQ(throwing_gen_run.verdict.failed_cases, 1u);
        EXPECT_EQ(throwing_gen_run.verdict.failure_categories,
                  std::vector<std::string>{"runtime_exception"});

        json encoded_throwing_verdict;
        to_json(encoded_throwing_verdict, throwing_gen_run.verdict);
        EXPECT_EQ(encoded_throwing_verdict.dump().find("generator boom"), std::string::npos);
        EXPECT_EQ(throwing_gen_run.verdict.public_feedback.dump().find("generator boom"),
                  std::string::npos);
    }

    const auto throwing_gen_gate = make_gate(throwing_gen_slot, GateFeedbackMode::FailedCount);
    const auto fail_case_two_candidate = ProgramSource::from_javascript(
        "candidate-fail-case-2",
        "export function evaluate(input) {\n"
        "  if (input.index === 2) return 'wrong-at-2';\n"
        "  return input.seed + ':' + String(input.index);\n"
        "}\n");
    const auto throwing_gen_run_2 = evaluator.evaluate(
        throwing_gen_gate,
        throwing_gen_slot,
        fail_case_two_candidate,
        "sub-gen",
        2,
        "0011223344556677");
    EXPECT_FALSE(throwing_gen_run_2.verdict.passed);
    EXPECT_EQ(throwing_gen_run_2.verdict.total_cases, 3u);
    EXPECT_EQ(throwing_gen_run_2.verdict.failed_cases, 2u);
    EXPECT_EQ(throwing_gen_run_2.verdict.failure_categories,
              (std::vector<std::string>{"runtime_exception", "assertion_failed"}));
}

TEST(ProgramGateTest, QualificationPassesForQualifiedSlotAndRejectsSurvivingMutantOrFailingGolden) {
    const auto    qualified_slot = GateTestSlot::create(make_sorting_slot_data());
    const auto    qualified_gate = make_gate(qualified_slot);
    GateEvaluator evaluator;

    const auto receipt = evaluator.qualify(qualified_gate, qualified_slot);
    EXPECT_TRUE(receipt.golden_passed());
    EXPECT_EQ(receipt.total_mutants(), 3u);
    EXPECT_EQ(receipt.killed_mutants(), 3u);
    EXPECT_EQ(receipt.gate_version_id(), qualified_gate.id());
    EXPECT_EQ(receipt.test_slot_digest(), qualified_slot.digest());
    EXPECT_EQ(receipt.generator_seed_hex(), expected_slot_seed_hex(qualified_slot));
    EXPECT_EQ(GateSlotQualificationReceipt::parse(receipt.serialize_canonical()), receipt);

    auto empty_mutants_data = make_sorting_slot_data();
    empty_mutants_data.qualification_mutants.clear();
    const auto empty_mutants_slot = GateTestSlot::create(empty_mutants_data);
    const auto empty_mutants_gate = make_gate(empty_mutants_slot);
    expect_diagnostic_code(
        [&] { (void)evaluator.qualify(empty_mutants_gate, empty_mutants_slot); },
        "P_GATE_SLOT_NO_MUTANTS");

    // A slot that rejects everything would kill every mutant; without a golden candidate it does not qualify.
    auto no_golden_data = make_sorting_slot_data();
    no_golden_data.golden_candidates.clear();
    const auto no_golden_slot = GateTestSlot::create(no_golden_data);
    const auto no_golden_gate = make_gate(no_golden_slot);
    expect_diagnostic_code(
        [&] { (void)evaluator.qualify(no_golden_gate, no_golden_slot); },
        "P_GATE_SLOT_NO_GOLDEN");

    auto generated_slot_data           = make_sorting_slot_data();
    generated_slot_data.case_generator = GateCaseGenerator{
        "export function generate(seedHex, caseIndex) {\n"
        "  const base = seedHex.charCodeAt(caseIndex % seedHex.length);\n"
        "  return [base + 3, base - 2, base + 1, -base];\n"
        "}\n",
        3,
        "isSortedPermutation",
    };
    const auto generated_slot    = GateTestSlot::create(generated_slot_data);
    const auto generated_gate    = make_gate(generated_slot);
    const auto generated_receipt = evaluator.qualify(generated_gate, generated_slot);
    EXPECT_TRUE(generated_receipt.golden_passed());
    EXPECT_EQ(generated_receipt.total_mutants(), 3u);
    EXPECT_EQ(generated_receipt.killed_mutants(), 3u);
    EXPECT_EQ(generated_receipt.generator_seed_hex(), expected_slot_seed_hex(generated_slot));
    EXPECT_NE(generated_receipt.generator_seed_hex(), receipt.generator_seed_hex());
    EXPECT_EQ(GateSlotQualificationReceipt::parse(generated_receipt.serialize_canonical()),
              generated_receipt);

    auto throwing_generator_slot_data           = make_sorting_slot_data();
    throwing_generator_slot_data.case_generator = GateCaseGenerator{
        "export function generate(seedHex, caseIndex) {\n"
        "  if (caseIndex === 1) throw new Error('boom');\n"
        "  return [3, 1, 2];\n"
        "}\n",
        3,
        "isSortedPermutation",
    };
    const auto throwing_generator_slot = GateTestSlot::create(throwing_generator_slot_data);
    const auto throwing_generator_gate = make_gate(throwing_generator_slot);
    expect_diagnostic_code(
        [&] { (void)evaluator.qualify(throwing_generator_gate, throwing_generator_slot); },
        "P_GATE_SLOT_GENERATOR_FAILED");

    auto no_golden_broken_gen_data = throwing_generator_slot_data;
    no_golden_broken_gen_data.golden_candidates.clear();
    no_golden_broken_gen_data.qualification_mutants = {
        GateQualificationMutant{
            "mutant-actually-correct",
            "export function evaluate(input) { return [...input].sort((a, b) => a - b); }\n"},
    };
    const auto no_golden_broken_gen_slot = GateTestSlot::create(no_golden_broken_gen_data);
    const auto no_golden_broken_gen_gate = make_gate(no_golden_broken_gen_slot);
    expect_diagnostic_code(
        [&] { (void)evaluator.qualify(no_golden_broken_gen_gate, no_golden_broken_gen_slot); },
        "P_GATE_SLOT_GENERATOR_FAILED");

    auto import_gen_data = no_golden_broken_gen_data;
    import_gen_data.case_generator = GateCaseGenerator{
        "import { x } from './forbidden.js';\n"
        "export function generate(seedHex, caseIndex) { return [x, caseIndex]; }\n",
        1,
        "isSortedPermutation",
    };
    const auto import_gen_slot = GateTestSlot::create(import_gen_data);
    const auto import_gen_gate = make_gate(import_gen_slot);
    expect_diagnostic_code(
        [&] { (void)evaluator.qualify(import_gen_gate, import_gen_slot); },
        "P_GATE_SANDBOX_IMPORT_DENIED");

    auto surviving_mutant_data = make_sorting_slot_data();
    surviving_mutant_data.qualification_mutants.push_back(GateQualificationMutant{
        "mutant-equivalent",
        "export function evaluate(input) { return [...input].sort((a, b) => a - b); }\n",
    });
    const auto surviving_slot = GateTestSlot::create(surviving_mutant_data);
    const auto surviving_gate = make_gate(surviving_slot);
    expect_diagnostic_code(
        [&] { (void)evaluator.qualify(surviving_gate, surviving_slot); },
        "P_GATE_SLOT_MUTANT_SURVIVED");

    auto failing_golden_data = make_sorting_slot_data();
    failing_golden_data.golden_candidates.push_back(GateGoldenCandidate{
        "golden-broken",
        "export function evaluate(input) { return []; }\n",
    });
    const auto failing_golden_slot = GateTestSlot::create(failing_golden_data);
    const auto failing_golden_gate = make_gate(failing_golden_slot);
    expect_diagnostic_code(
        [&] { (void)evaluator.qualify(failing_golden_gate, failing_golden_slot); },
        "P_GATE_SLOT_GOLDEN_FAILED");
}

#endif

}  // namespace
