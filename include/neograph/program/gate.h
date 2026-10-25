/**
 * @file program/gate.h
 * @brief Immutable Program gates with hidden test slots, deterministic sandbox
 *        evaluation, and content-addressed verdict tokens.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>
#include <neograph/program/diagnostic.h>
#include <neograph/program/source.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

/** One public input/output example declared by a gate test slot. */
struct GatePublicExample {
    std::string case_id;
    json        input;
    json        expected;

    bool operator==(const GatePublicExample&) const = default;
};

/**
 * One hidden test case declared by a gate test slot.
 *
 * Exactly one of `expected` or `property_entrypoint` must be set.
 */
struct GateHiddenCase {
    std::string                case_id;
    json                       input;
    std::optional<json>        expected{};
    std::optional<std::string> property_entrypoint{};

    bool operator==(const GateHiddenCase&) const = default;
};

/** Pure JavaScript module exporting property predicates `(input, actual) => boolean`. */
struct GatePropertyModule {
    std::string source;

    bool operator==(const GatePropertyModule&) const = default;
};

/** Deterministic JavaScript case generator exporting `generate(seedHex, caseIndex)`. */
struct GateCaseGenerator {
    std::string   source;
    std::uint64_t generated_case_count = 0;
    std::string   property_entrypoint;

    bool operator==(const GateCaseGenerator&) const = default;
};

/** Mutant candidate that a qualified test slot must kill on at least one case. */
struct GateQualificationMutant {
    std::string mutant_id;
    std::string source;

    bool operator==(const GateQualificationMutant&) const = default;
};

/** Golden reference candidate that must pass every case of a qualified test slot. */
struct GateGoldenCandidate {
    std::string candidate_id;
    std::string source;

    bool operator==(const GateGoldenCandidate&) const = default;
};

/** Deeply owned data of a host-owned gate test slot. */
struct GateTestSlotData {
    std::string                          slot_id;
    std::string                          owner_scope;
    std::vector<GatePublicExample>       public_examples;
    std::vector<GateHiddenCase>          hidden_cases;
    std::optional<GatePropertyModule>    property_module{};
    std::optional<GateCaseGenerator>     case_generator{};
    std::vector<GateQualificationMutant> qualification_mutants{};
    std::vector<GateGoldenCandidate>     golden_candidates{};

    bool operator==(const GateTestSlotData&) const = default;
};

/**
 * Immutable content-addressed host test slot.
 *
 * `digest()` covers the entire canonical slot envelope under domain
 * `"program-gate-test-slot/v1"`. `public_digest()` covers only `schema_version`
 * and `public_examples` under domain `"program-gate-public-slot/v1"`.
 */
class NEOGRAPH_PROGRAM_API GateTestSlot final {
public:
    static constexpr std::uint32_t SCHEMA_VERSION = 1;

    static GateTestSlot create(GateTestSlotData data);
    static GateTestSlot parse(std::string_view canonical_bytes);

    const GateTestSlotData&                    data() const noexcept;
    const std::string&                         digest() const noexcept;
    const std::string&                         id() const noexcept;
    const std::string&                         public_digest() const noexcept;
    const std::string&                         slot_id() const noexcept;
    const std::string&                         owner_scope() const noexcept;
    const std::vector<GatePublicExample>&      public_examples() const noexcept;
    const std::vector<GateHiddenCase>&         hidden_cases() const noexcept;
    const std::optional<GatePropertyModule>&   property_module() const noexcept;
    const std::optional<GateCaseGenerator>&    case_generator() const noexcept;
    const std::vector<GateQualificationMutant>& qualification_mutants() const noexcept;
    const std::vector<GateGoldenCandidate>&    golden_candidates() const noexcept;
    std::string                                serialize_canonical() const;

    bool operator==(const GateTestSlot& other) const noexcept;

private:
    struct Impl;
    explicit GateTestSlot(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

/**
 * Per-case QuickJS resource limits for gate evaluation.
 *
 * Every field is required and must be positive; there are no defaults and no
 * built-in constants.
 */
struct GateSandboxLimits {
    std::uint64_t case_wall_time_ms;
    std::uint64_t case_interrupt_polls;
    std::size_t   case_memory_limit_bytes;
    std::size_t   case_max_stack_bytes;

    bool operator==(const GateSandboxLimits&) const = default;
};

/** Closed feedback disclosure policy for a Program gate. */
enum class GateFeedbackMode : std::uint8_t {
    VerdictOnly,
    FailedCount,
    PublicExample,
    SanitizedSymptoms,
};

/** Closed failure symptom categories reported by deterministic gate evaluation. */
enum class GateFailureCategory : std::uint8_t {
    Timeout,
    MemoryLimit,
    SchemaMismatch,
    AssertionFailed,
    RuntimeException,
};

struct GateFeedbackPolicy {
    GateFeedbackMode mode;

    bool operator==(const GateFeedbackPolicy&) const = default;
};

NEOGRAPH_PROGRAM_API std::string_view    to_string(GateFeedbackMode mode) noexcept;
NEOGRAPH_PROGRAM_API GateFeedbackMode    gate_feedback_mode_from_string(std::string_view value);
NEOGRAPH_PROGRAM_API std::string_view    to_string(GateFailureCategory category) noexcept;
NEOGRAPH_PROGRAM_API GateFailureCategory gate_failure_category_from_string(std::string_view value);

NEOGRAPH_PROGRAM_API void to_json(json& value, const GatePublicExample& example);
NEOGRAPH_PROGRAM_API void from_json(const json& value, GatePublicExample& example);
NEOGRAPH_PROGRAM_API void to_json(json& value, const GateHiddenCase& hidden_case);
NEOGRAPH_PROGRAM_API void from_json(const json& value, GateHiddenCase& hidden_case);
NEOGRAPH_PROGRAM_API void to_json(json& value, const GatePropertyModule& module);
NEOGRAPH_PROGRAM_API void from_json(const json& value, GatePropertyModule& module);
NEOGRAPH_PROGRAM_API void to_json(json& value, const GateCaseGenerator& generator);
NEOGRAPH_PROGRAM_API void from_json(const json& value, GateCaseGenerator& generator);
NEOGRAPH_PROGRAM_API void to_json(json& value, const GateQualificationMutant& mutant);
NEOGRAPH_PROGRAM_API void from_json(const json& value, GateQualificationMutant& mutant);
NEOGRAPH_PROGRAM_API void to_json(json& value, const GateGoldenCandidate& candidate);
NEOGRAPH_PROGRAM_API void from_json(const json& value, GateGoldenCandidate& candidate);
NEOGRAPH_PROGRAM_API void to_json(json& value, const GateSandboxLimits& limits);
NEOGRAPH_PROGRAM_API void from_json(const json& value, GateSandboxLimits& limits);
NEOGRAPH_PROGRAM_API void to_json(json& value, const GateFeedbackPolicy& policy);
NEOGRAPH_PROGRAM_API void from_json(const json& value, GateFeedbackPolicy& policy);

/** Declared configuration of a versioned Program gate. */
struct ProgramGateData {
    std::string        owner_scope;
    std::string        gate_name;
    std::string        test_slot_digest;
    GateSandboxLimits  sandbox_limits;
    GateFeedbackPolicy feedback_policy;

    bool operator==(const ProgramGateData&) const = default;
};

/**
 * Immutable content-addressed Program gate version.
 *
 * `id()` is computed under domain `"program-gate-version/v1"`; changing the
 * bound `test_slot_digest` produces a distinct gate version identity.
 */
class NEOGRAPH_PROGRAM_API ProgramGateVersion final {
public:
    static constexpr std::uint32_t SCHEMA_VERSION = 1;

    static ProgramGateVersion create(ProgramGateData data);
    static ProgramGateVersion parse(std::string_view canonical_bytes);

    const ProgramGateData&    data() const noexcept;
    const std::string&        id() const noexcept;
    const std::string&        owner_scope() const noexcept;
    const std::string&        gate_name() const noexcept;
    const std::string&        test_slot_digest() const noexcept;
    const GateSandboxLimits&  sandbox_limits() const noexcept;
    const GateFeedbackPolicy& feedback_policy() const noexcept;
    std::string               serialize_canonical() const;

    bool operator==(const ProgramGateVersion& other) const noexcept;

private:
    struct Impl;
    explicit ProgramGateVersion(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

/** Sanitized outcome of evaluating a candidate against a gate test slot. */
struct GateVerdict {
    bool                     passed       = false;
    std::uint64_t            total_cases  = 0;
    std::uint64_t            failed_cases = 0;
    json                     public_feedback = json::object();
    std::vector<std::string> failure_categories;

    bool operator==(const GateVerdict&) const = default;
};

NEOGRAPH_PROGRAM_API void to_json(json& value, const GateVerdict& verdict);
NEOGRAPH_PROGRAM_API void from_json(const json& value, GateVerdict& verdict);

/**
 * Payload of a content-addressed verdict token minted on full gate pass.
 *
 * `generator_seed_hex` records the seed used to generate cases for this
 * evaluation, and is empty when the test slot has no `case_generator`.
 */
struct GateVerdictTokenData {
    std::string   owner_scope;
    std::string   subject_id;
    std::string   gate_version_id;
    std::string   test_slot_digest;
    std::string   checked_value_digest;
    std::uint64_t attempt = 0;
    std::string   generator_seed_hex{};

    bool operator==(const GateVerdictTokenData&) const = default;
};

/**
 * Immutable content-addressed token proving that a specific candidate value
 * passed every case of a gate version's test slot.
 */
class NEOGRAPH_PROGRAM_API GateVerdictToken final {
public:
    static constexpr std::uint32_t SCHEMA_VERSION = 1;

    static std::string      compute_value_digest(const ProgramSource& candidate);
    static std::string      compute_value_digest(const json& value);
    static GateVerdictToken create(GateVerdictTokenData data);
    static GateVerdictToken parse(std::string_view canonical_bytes);

    const GateVerdictTokenData& data() const noexcept;
    const std::string&          id() const noexcept;
    const std::string&          owner_scope() const noexcept;
    const std::string&          subject_id() const noexcept;
    const std::string&          gate_version_id() const noexcept;
    const std::string&          test_slot_digest() const noexcept;
    const std::string&          checked_value_digest() const noexcept;
    std::uint64_t               attempt() const noexcept;
    const std::string&          generator_seed_hex() const noexcept;
    bool                        binds(const ProgramSource& candidate) const;
    bool                        binds_value(const json& value) const;
    std::string                 serialize_canonical() const;

    bool operator==(const GateVerdictToken& other) const noexcept;

private:
    struct Impl;
    explicit GateVerdictToken(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

/**
 * Payload of a content-addressed slot qualification receipt.
 *
 * `generator_seed_hex` records the deterministic qualification seed derived as
 * the lowercase hex SHA-256 of the slot's canonical bytes
 * (`slot.serialize_canonical()`). It is passed to `case_generator` when the
 * slot declares one, and is always included in the receipt's canonical body
 * and `id`.
 */
struct GateSlotQualificationReceiptData {
    std::string   gate_version_id;
    std::string   test_slot_digest;
    std::string   mutants_digest;
    std::uint64_t total_mutants  = 0;
    std::uint64_t killed_mutants = 0;
    bool          golden_passed  = false;
    std::string   generator_seed_hex{};

    bool operator==(const GateSlotQualificationReceiptData&) const = default;
};

/**
 * Content-addressed receipt proving that every golden candidate passed every
 * case and every qualification mutant failed at least one case.
 */
class NEOGRAPH_PROGRAM_API GateSlotQualificationReceipt final {
public:
    static constexpr std::uint32_t SCHEMA_VERSION = 1;

    static GateSlotQualificationReceipt create(GateSlotQualificationReceiptData data);
    static GateSlotQualificationReceipt parse(std::string_view canonical_bytes);

    const GateSlotQualificationReceiptData& data() const noexcept;
    const std::string&                      id() const noexcept;
    const std::string&                      gate_version_id() const noexcept;
    const std::string&                      test_slot_digest() const noexcept;
    const std::string&                      mutants_digest() const noexcept;
    std::uint64_t                           total_mutants() const noexcept;
    std::uint64_t                           killed_mutants() const noexcept;
    bool                                    golden_passed() const noexcept;
    const std::string&                      generator_seed_hex() const noexcept;
    std::string                             serialize_canonical() const;

    bool operator==(const GateSlotQualificationReceipt& other) const noexcept;

private:
    struct Impl;
    explicit GateSlotQualificationReceipt(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

/** Combined verdict and optional verdict token returned by `GateEvaluator::evaluate`. */
struct GateEvaluation {
    GateVerdict                     verdict;
    std::optional<GateVerdictToken> token;

    bool operator==(const GateEvaluation&) const = default;
};

/** Stateless evaluator for deterministic Program gates and test-slot qualification. */
class NEOGRAPH_PROGRAM_API GateEvaluator final {
public:
    GateEvaluator() = default;

    /**
     * Evaluates `candidate` against every case in `slot`. When `slot` declares
     * a `case_generator`, `generator_seed_hex` must be non-empty or evaluation
     * fails with diagnostic code `P_GATE_SEED_REQUIRED`.
     */
    GateEvaluation evaluate(const ProgramGateVersion& gate,
                            const GateTestSlot&       slot,
                            const ProgramSource&      candidate,
                            std::string_view          subject_id,
                            std::uint64_t             attempt,
                            std::string_view          generator_seed_hex = {}) const;

    /**
     * Qualifies `slot` against `gate`: requires at least one qualification
     * mutant (`P_GATE_SLOT_NO_MUTANTS`), validates slot modules and generated
     * cases (`P_GATE_SLOT_GENERATOR_FAILED`), verifies that every golden
     * candidate passes (`P_GATE_SLOT_GOLDEN_FAILED`) and every mutant fails at
     * least one case (`P_GATE_SLOT_MUTANT_SURVIVED`), and records
     * `generator_seed_hex` derived from `slot.serialize_canonical()`.
     */
    GateSlotQualificationReceipt qualify(const ProgramGateVersion& gate,
                                         const GateTestSlot&       slot) const;
};

}  // namespace neograph::program
