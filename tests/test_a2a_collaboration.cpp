#include <neograph/a2a/collaboration.h>

#ifdef NEOGRAPH_A2A_PROGRAM
#include <neograph/a2a/client.h>
#include <neograph/a2a/server.h>
#include <neograph/a2a/program_adapter.h>
#include <neograph/graph/node.h>
#include <neograph/program/program.h>
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <thread>
#include <stdexcept>
namespace {

using namespace neograph::a2a;
using neograph::json;

CollaborationLink make_link() {
    CollaborationLinkSpec spec;
    spec.link_id = "link-1";
    spec.sender_owner_scope = "owner-a";
    spec.receiver_owner_scope = "owner-b";
    spec.sender_agent_id = "coordinator-a";
    spec.receiver_agent_id = "executor-b";
    spec.task_scope = "bounded-coding-task";
    spec.capability_allowlist = {"code-read", "code-write"};
    spec.effect_allowlist = {"artifact-publish"};
    spec.artifact_allowlist = {"artifact-1"};
    spec.cancellation_rights = {"sender", "receiver"};
    spec.message_kind_allowlist = {
        "request", "progress", "artifact", "correction", "retry", "completed",
        "failed", "canceled", "rejected", "auth-required", "terminal",
        "vendor-terminal-unknown"};
    spec.expires_at_unix_ms = 4102444800000ULL;
    spec.max_retries = 2;
    spec.acknowledgement_timeout_ms = 1000;
    return CollaborationLink::create(std::move(spec));
}

CollaborationEnvelope make_envelope(const CollaborationLink& link,
                                     std::uint64_t sequence = 1,
                                     std::string kind = "progress",
                                     std::string idempotency_key = "idem-1") {
    return CollaborationEnvelope::create(link,
                                          "run-a",
                                          "run-b",
                                          "task-1",
                                          "context-1",
                                          "message-" + std::to_string(sequence),
                                          "correlation-1",
                                          sequence,
                                          std::move(kind),
                                          std::move(idempotency_key),
                                          json{{"percent", sequence}},
                                          {CollaborationArtifactReference{"artifact-1",
                                                                          "artifact://one",
                                                                          "application/json",
                                                                          42}});
}

#ifdef NEOGRAPH_A2A_PROGRAM
std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

neograph::program::ProgramVersion make_program_version() {
    using namespace neograph::program;
    RegistrySnapshotBuilder registry_builder;
    registry_builder.add_reducer(
        ExecutableManifest{{ExecutableKind::Reducer, "a2a-reducer", "1.0.0", digest('1')},
                           EffectMode::Brokered, "attestation:a2a", {}, {}},
        [](const json&, const json& incoming) { return json(incoming); });
    auto registry = std::move(registry_builder).build();

    AdmissionProfileBuilder admission_builder;
    admission_builder.id("a2a-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(1)
        .allow_source_kind(SourceKind::CanonicalJson)
        .allow_effect_mode(EffectMode::Brokered);
    auto admission = std::move(admission_builder).build();

    PolicySnapshotBuilder policy_builder;
    policy_builder.id("a2a-policy")
        .semantic_version("1.0.0")
        .owner_scope("owner-b")
        .admission_profile(admission)
        .budget_ceiling(BudgetLimits{1000, 1000, 1000, 1, 1, 20, 1, 1, 1});
    auto policy = std::move(policy_builder).build();
    return ProgramVersion(ProgramVersionData{
        digest('2'), admission, policy, {}, "owner-b",
        CoreMaterializationReceipt{"a2a-tests", registry.fingerprint(),
                                   {CorePlanIdentity{"main", digest('3')}}, {}}});
}
class AdapterCompletedNode final : public neograph::graph::GraphNode {
public:
    AdapterCompletedNode(std::string name, std::shared_ptr<std::atomic<unsigned>> starts)
        : name_(std::move(name)), starts_(std::move(starts)) {}

    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput) override {
        starts_->fetch_add(1, std::memory_order_relaxed);
        neograph::graph::NodeOutput output;
        output.writes.push_back(neograph::graph::ChannelWrite{"value", "completed"});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string                            name_;
    std::shared_ptr<std::atomic<unsigned>> starts_;
};

struct ProgramAdapterFixture {
    std::shared_ptr<std::atomic<unsigned>> starts;
    neograph::program::RegistrySnapshot registry;
    neograph::program::AdmissionProfile profile;
    neograph::program::PolicySnapshot policy;
    std::shared_ptr<neograph::program::InMemoryProgramStore> store;
    std::shared_ptr<neograph::program::EngineGenerationCache> engines;
    std::shared_ptr<neograph::program::ProgramCatalog> catalog;
    std::shared_ptr<neograph::graph::InMemoryCheckpointStore> checkpoints;
    std::shared_ptr<neograph::program::InMemoryProgramTransitionStore> transitions;
    std::shared_ptr<neograph::program::ProgramRuntime> runtime;
    std::optional<neograph::program::ProgramVersion> version;

    ProgramAdapterFixture()
        : starts(std::make_shared<std::atomic<unsigned>>(0)),
          registry(make_registry(starts)),
          profile(make_profile(registry)),
          policy(make_policy(profile)),
          store(std::make_shared<neograph::program::InMemoryProgramStore>()),
          engines(std::make_shared<neograph::program::EngineGenerationCache>()),
          catalog(std::make_shared<neograph::program::ProgramCatalog>(
              neograph::program::CatalogConfig{store, registry, engines,
                                                "a2a-adapter-test/v1"})),
          checkpoints(std::make_shared<neograph::graph::InMemoryCheckpointStore>()),
          transitions(std::make_shared<neograph::program::InMemoryProgramTransitionStore>()) {
        neograph::program::ProgramCompiler compiler(registry, {"a2a-adapter-test/v1"});
        const auto source = neograph::program::ProgramSource::from_cpp_builder(
            "test:a2a-adapter", 1, source_document());
        version.emplace(catalog->admit(
            compiler.compile(source),
            neograph::program::ProgramAdmission{"owner-b", profile, policy, {}}));
        restart_after_mailbox_publication();
    }
    const neograph::program::ProgramVersion& admitted_version() const { return *version; }

    void restart_after_mailbox_publication() {
        runtime.reset();
        engines = std::make_shared<neograph::program::EngineGenerationCache>();
        catalog = std::make_shared<neograph::program::ProgramCatalog>(
            neograph::program::CatalogConfig{store, registry, engines, "a2a-adapter-test/v1"});
        runtime = std::make_shared<neograph::program::ProgramRuntime>(
            neograph::program::RuntimeConfig{catalog, checkpoints, {}, transitions, 1});
    }

private:
    static neograph::program::RegistrySnapshot
    make_registry(const std::shared_ptr<std::atomic<unsigned>>& starts) {
        using namespace neograph::program;
        RegistrySnapshotBuilder builder;
        builder.add_node(
            ExecutableManifest{{ExecutableKind::Node, "a2a-adapter-node", "1.0.0", digest('4')},
                               EffectMode::Brokered, "attestation:a2a", {}, {}, {}},
            [starts](const std::string& name, const json&, const neograph::graph::NodeContext&) {
                return std::make_unique<AdapterCompletedNode>(name, starts);
            },
            json{{"type", "object"}}, json::object());
        builder.add_reducer(
            ExecutableManifest{{ExecutableKind::Reducer, "a2a-adapter-overwrite", "1.0.0", digest('5')},
                               EffectMode::Brokered, "attestation:a2a", {}, {}, {}},
            [](const json&, const json& incoming) { return json(incoming); });
        return std::move(builder).build();
    }

    static neograph::program::AdmissionProfile
    make_profile(const neograph::program::RegistrySnapshot& registry) {
        using namespace neograph::program;
        AdmissionProfileBuilder builder;
        builder.id("a2a-adapter-profile")
            .semantic_version("1.0.0")
            .registry(registry)
            .mode(AdmissionMode::MultiTenant)
            .max_program_schema_version(1)
            .allow_source_kind(SourceKind::CppBuilder)
            .allow_effect_mode(EffectMode::Brokered);
        for (const auto& identity : registry.identities()) builder.allow_executable(identity);
        return std::move(builder).build();
    }

    static neograph::program::PolicySnapshot
    make_policy(const neograph::program::AdmissionProfile& profile) {
        using namespace neograph::program;
        PolicySnapshotBuilder builder;
        builder.id("a2a-adapter-policy")
            .semantic_version("1.0.0")
            .owner_scope("owner-b")
            .admission_profile(profile)
            .budget_ceiling(BudgetLimits{1000, 1000, 1000, 1, 1, 20, 1, 1, 1});
        return std::move(builder).build();
    }

    static json source_document() {
        return json{
            {"program_schema_version", 1},
            {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
            {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
            {"root",
             json{{"op", "call_core"},
                  {"name", "main"},
                  {"definition",
                   json{{"schema_version", 1},
                        {"name", "main"},
                        {"channels",
                         json{{"value",
                               json{{"reducer", "a2a-adapter-overwrite"}, {"initial", ""}}}}},
                        {"nodes", json{{"work", json{{"type", "a2a-adapter-node"}}}}},
                        {"edges",
                         json::array({json{{"from", "__start__"}, {"to", "work"}},
                                      json{{"from", "work"}, {"to", "__end__"}}})},
                        {"conditional_edges", json::array()}}}}},
            {"declared_budget_requirements",
             json::array(
                 {json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 1000}},
                  json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 1000}},
                  json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 1000}},
                  json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
                  json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
                  json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
                  json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
                  json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
                  json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}}})}};
    }
};

CollaborationEnvelope make_program_envelope(
    const CollaborationLink& link, const neograph::program::ProgramVersion& version) {
    return CollaborationEnvelope::bind_program(
        CollaborationEnvelope::create(link, "run-a", "run-b", "run-b", "context-1",
                                      "message-1", "correlation-1", 1, "request",
                                      "idem-program-recovery", json{{"prompt", "persisted"}}),
        version);
}

neograph::program::ProgramInvocation persisted_invocation() {
    neograph::program::ProgramInvocation invocation;
    invocation.input = json{{"prompt", "persisted"}};
    invocation.budget = neograph::program::RunBudget{1000, 1000, 1000, 1, 1, 20, 0, 0, 0};
    invocation.trace_id = "a2a:run-b";
    invocation.requested_run_id = "run-b";
    return invocation;
}
#endif

TEST(A2ACollaboration, ConsentFreezesOwnerAndCapabilityBoundary) {
    const auto proposed = make_link();
    EXPECT_EQ(proposed.state(), CollaborationLinkState::Proposed);
    EXPECT_THROW(CollaborationEnvelope::create(proposed, "a", "b", "t", "c", "m", "x", 1,
                                               "progress", "i"), std::invalid_argument);

    const auto accepted = proposed.accept("executor-b", "consent-secret");
    EXPECT_EQ(accepted.state(), CollaborationLinkState::Accepted);
    EXPECT_TRUE(accepted.permits_capability("code-read"));
    EXPECT_TRUE(accepted.permits_effect("artifact-publish"));
    EXPECT_TRUE(accepted.permits_artifact("artifact-1"));
    EXPECT_FALSE(accepted.permits_capability("host-shell"));
    EXPECT_TRUE(accepted.permits_message_kind("progress"));
    EXPECT_FALSE(accepted.permits_message_kind("host-shell"));
    EXPECT_EQ(CollaborationLink::parse(accepted.serialize_canonical()).content_hash(),
              accepted.content_hash());
    EXPECT_THROW(proposed.accept("wrong-agent", "consent-secret"), std::invalid_argument);
    EXPECT_THROW(proposed.accept("executor-b", ""), std::invalid_argument);
}

TEST(A2ACollaboration, LinkFailsClosedOnUndeclaredMessageKinds) {
    const auto proposed = make_link();
    auto spec = proposed.spec();
    spec.message_kind_allowlist = {"progress"};
    const auto accepted = CollaborationLink::create(std::move(spec)).accept(
        "executor-b", "consent-secret");

    EXPECT_THROW(make_envelope(accepted, 1, "correction"), std::invalid_argument);

    CollaborationMailbox receiver("owner-b", "executor-b");
    receiver.accept_link(accepted, "consent-secret");
    auto forged = make_envelope(accepted);
    forged.kind = "correction";
    EXPECT_EQ(receiver.submit(std::move(forged)), CollaborationSubmitResult::Rejected);

    auto stored = json::parse(accepted.serialize_canonical());
    stored["spec"]["message_kind_allowlist"] = "not-an-array";
    EXPECT_THROW(CollaborationLink::parse(stored.dump()), std::invalid_argument);
}

TEST(A2ACollaboration, EnvelopeRoundTripsThroughA2AMessage) {
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    const auto envelope = make_envelope(accepted);
    const auto decoded = collaboration_from_message(collaboration_to_message(envelope));
    EXPECT_EQ(decoded.serialize_canonical(), envelope.serialize_canonical());
    EXPECT_EQ(decoded.content_hash(), envelope.content_hash());

    auto unknown = make_envelope(accepted, 1, "vendor-terminal-unknown", "idem-unknown");
    EXPECT_FALSE(unknown.is_terminal());
    EXPECT_EQ(CollaborationEnvelope::parse(unknown.serialize_canonical()).kind,
              "vendor-terminal-unknown");
    EXPECT_THROW(CollaborationEnvelope::create(
                     accepted, "run-a", "run-b", "task-1", "context-1", "message-2", "correlation-1",
                     2, "progress", "idem-2", json{{"ok", true}},
                     {CollaborationArtifactReference{"unlisted", "artifact://x", "text/plain", 1}}),
                 std::invalid_argument);
}

TEST(A2ACollaboration, MailboxIsOwnerScopedAndIdempotent) {
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    CollaborationMailbox receiver("owner-b", "executor-b");
    receiver.accept_link(accepted, "consent-secret");
    const auto first = make_envelope(accepted);
    EXPECT_EQ(receiver.submit(first), CollaborationSubmitResult::Accepted);
    EXPECT_EQ(receiver.submit(first), CollaborationSubmitResult::Duplicate);

    auto conflict = first;
    conflict.kind = "correction";
    EXPECT_EQ(receiver.submit(conflict), CollaborationSubmitResult::Conflict);

    auto second = make_envelope(accepted, 2, "artifact", "idem-2");
    EXPECT_EQ(receiver.submit(second), CollaborationSubmitResult::Accepted);
    EXPECT_TRUE(receiver.acknowledge("idem-1"));
    ASSERT_TRUE(receiver.get("idem-1").has_value());
    EXPECT_EQ(receiver.get("idem-1")->state, CollaborationRecordState::Acknowledged);
    EXPECT_TRUE(receiver.cancel("link-1", "correlation-1", "coordinator-a"));
    EXPECT_EQ(receiver.get("idem-2")->state, CollaborationRecordState::Canceled);

    CollaborationMailbox wrong_owner("owner-a", "coordinator-a");
    EXPECT_EQ(wrong_owner.submit(first), CollaborationSubmitResult::Rejected);
    EXPECT_FALSE(wrong_owner.get("idem-1").has_value());
}

TEST(A2ACollaboration, MailboxJournalReopensWithoutLosingCorrelation) {
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    CollaborationMailbox receiver("owner-b", "executor-b");
    receiver.accept_link(accepted, "consent-secret");
    EXPECT_EQ(receiver.submit(make_envelope(accepted)), CollaborationSubmitResult::Accepted);
    EXPECT_EQ(receiver.submit(make_envelope(accepted, 2, "retry", "idem-2")),
              CollaborationSubmitResult::Accepted);
    auto reopened = CollaborationMailbox::parse(receiver.serialize_canonical());
    ASSERT_EQ(reopened.snapshot().size(), 2U);
    EXPECT_EQ(reopened.snapshot()[0].envelope.sequence, 1U);
    EXPECT_EQ(reopened.snapshot()[1].envelope.sequence, 2U);
    EXPECT_EQ(reopened.submit(make_envelope(accepted, 2, "retry", "idem-2")),
              CollaborationSubmitResult::Duplicate);
    EXPECT_EQ(reopened.submit(make_envelope(accepted, 3, "correction", "idem-3")),
              CollaborationSubmitResult::Accepted);
}

TEST(A2ACollaboration, MailboxJournalRejectsIdentityTamperingOnReopen) {
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    CollaborationMailbox receiver("owner-b", "executor-b");
    receiver.accept_link(accepted, "consent-secret");
    EXPECT_EQ(receiver.submit(make_envelope(accepted)), CollaborationSubmitResult::Accepted);
    EXPECT_EQ(receiver.submit(make_envelope(accepted, 2, "retry", "idem-2")),
              CollaborationSubmitResult::Accepted);

    const auto stored = json::parse(receiver.serialize_canonical());

    auto duplicate_link = stored;
    duplicate_link["links"].push_back(duplicate_link["links"][0]);
    EXPECT_THROW(CollaborationMailbox::parse(duplicate_link.dump()), std::invalid_argument);

    auto duplicate_record = stored;
    duplicate_record["records"].push_back(duplicate_record["records"][0]);
    EXPECT_THROW(CollaborationMailbox::parse(duplicate_record.dump()), std::invalid_argument);

    auto unknown_link = stored;
    unknown_link["records"][0]["envelope"]["link_id"] = "link-missing";
    EXPECT_THROW(CollaborationMailbox::parse(unknown_link.dump()), std::invalid_argument);

    auto foreign_sender = stored;
    foreign_sender["records"][0]["envelope"]["sender_owner_scope"] = "owner-forged";
    EXPECT_THROW(CollaborationMailbox::parse(foreign_sender.dump()), std::invalid_argument);

    auto duplicate_sequence = stored;
    auto duplicate = duplicate_sequence["records"][0];
    duplicate["envelope"]["idempotency_key"] = "idem-forged";
    duplicate["envelope"]["message_id"] = "message-forged";
    duplicate_sequence["records"].push_back(std::move(duplicate));
    EXPECT_THROW(CollaborationMailbox::parse(duplicate_sequence.dump()), std::invalid_argument);

    auto sequence_gap = stored;
    auto records = json::array();
    records.push_back(sequence_gap["records"][1]);
    sequence_gap["records"] = std::move(records);
    EXPECT_THROW(CollaborationMailbox::parse(sequence_gap.dump()), std::invalid_argument);
}

#ifdef NEOGRAPH_A2A_PROGRAM
TEST(A2ACollaboration, MailboxRetainsTypedProgramRequestAcrossReconnect) {
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    auto version = make_program_version();
    CollaborationMailbox receiver("owner-b", "executor-b");
    receiver.accept_link(accepted, "consent-secret");

    auto envelope = CollaborationEnvelope::bind_program(make_envelope(accepted), version);
    neograph::program::ProgramInvocation invocation;
    invocation.input = json{{"prompt", "typed"}};
    invocation.budget = neograph::program::RunBudget{1000, 1000, 1000, 1, 1, 20, 0, 0, 0};
    invocation.trace_id = "a2a:run-b";
    invocation.requested_run_id = "run-b";

    EXPECT_EQ(receiver.submit_program(envelope, version, invocation),
              CollaborationSubmitResult::Accepted);
    EXPECT_TRUE(receiver.permits_artifact("link-1", "artifact-1"));
    EXPECT_FALSE(receiver.permits_artifact("link-1", "unlisted"));
    auto reopened = CollaborationMailbox::parse(receiver.serialize_canonical());
    const auto request = reopened.get_program_request("idem-1");
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(request->version);
    ASSERT_TRUE(request->invocation);
    EXPECT_EQ(request->version->id(), version.id());
    EXPECT_EQ(request->invocation->requested_run_id, "run-b");
    EXPECT_EQ(request->invocation->input, invocation.input);
    EXPECT_EQ(reopened.submit(envelope, version, invocation),
              CollaborationSubmitResult::Duplicate);
}

TEST(A2ACollaboration, DuplicateMailboxRequestRecoversCrashBeforeRuntimeStart) {
    ProgramAdapterFixture fixture;
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    const auto envelope = make_program_envelope(accepted, fixture.admitted_version());
    auto mailbox = std::make_shared<CollaborationMailbox>("owner-b", "executor-b");
    mailbox->accept_link(accepted, "consent-secret");
    ASSERT_EQ(mailbox->submit_program(envelope, fixture.admitted_version(), persisted_invocation()),
              CollaborationSubmitResult::Accepted);

    auto reopened = std::make_shared<CollaborationMailbox>(
        CollaborationMailbox::parse(mailbox->serialize_canonical()));
    fixture.restart_after_mailbox_publication();
    ProgramAgentAdapter adapter(fixture.runtime, fixture.admitted_version(), "owner-b", reopened);

    const auto recovered = adapter.start(collaboration_to_message(envelope), "run-b", "context-1");
    EXPECT_EQ(recovered.run_id(), "run-b");
    EXPECT_EQ(recovered.snapshot().invocation().input, persisted_invocation().input);
    EXPECT_EQ(recovered.wait().status(), neograph::program::ProgramTerminalStatus::Completed);
    EXPECT_EQ(fixture.starts->load(std::memory_order_relaxed), 1U);

    const auto retry = adapter.start(collaboration_to_message(envelope), "run-b", "context-1");
    EXPECT_EQ(retry.run_id(), "run-b");
    EXPECT_EQ(retry.wait().status(), neograph::program::ProgramTerminalStatus::Completed);
    EXPECT_EQ(fixture.starts->load(std::memory_order_relaxed), 1U);
}

TEST(A2ACollaboration, DuplicateMailboxRequestReconnectsPublishedRunWithoutRedispatch) {
    ProgramAdapterFixture fixture;
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    const auto envelope = make_program_envelope(accepted, fixture.admitted_version());
    auto mailbox = std::make_shared<CollaborationMailbox>("owner-b", "executor-b");
    mailbox->accept_link(accepted, "consent-secret");
    ProgramAgentAdapter original(fixture.runtime, fixture.admitted_version(), "owner-b", mailbox);

    const auto initial = original.start(collaboration_to_message(envelope), "run-b", "context-1");
    ASSERT_EQ(initial.wait().status(), neograph::program::ProgramTerminalStatus::Completed);
    ASSERT_EQ(fixture.starts->load(std::memory_order_relaxed), 1U);

    auto reopened = std::make_shared<CollaborationMailbox>(
        CollaborationMailbox::parse(mailbox->serialize_canonical()));
    fixture.restart_after_mailbox_publication();
    ProgramAgentAdapter adapter(fixture.runtime, fixture.admitted_version(), "owner-b", reopened);

    const auto retry = adapter.start(collaboration_to_message(envelope), "run-b", "context-1");
    EXPECT_EQ(retry.run_id(), "run-b");
    EXPECT_EQ(retry.wait().status(), neograph::program::ProgramTerminalStatus::Completed);
    EXPECT_EQ(fixture.starts->load(std::memory_order_relaxed), 1U);
}

TEST(A2ACollaboration, RecoveredTypedMailboxRequestStartsExactlyOnce) {
    ProgramAdapterFixture fixture;
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    const auto envelope = make_program_envelope(accepted, fixture.admitted_version());

    CollaborationMailbox original("owner-b", "executor-b");
    original.accept_link(accepted, "consent-secret");
    ASSERT_EQ(original.submit_program(envelope, fixture.admitted_version(), persisted_invocation()),
              CollaborationSubmitResult::Accepted);

    const auto persisted_mailbox = original.serialize_canonical();
    auto reopened = std::make_shared<CollaborationMailbox>(
        CollaborationMailbox::parse(persisted_mailbox));
    fixture.restart_after_mailbox_publication();

    ProgramAgentAdapter adapter(fixture.runtime, fixture.admitted_version(), "owner-b", reopened);
    const auto recovered = adapter.recover_pending();
    ASSERT_EQ(recovered.size(), 1U);
    EXPECT_EQ(recovered.front().run_id(), "run-b");
    EXPECT_EQ(recovered.front().program_version_id(), fixture.admitted_version().id());
    EXPECT_EQ(recovered.front().wait().status(), neograph::program::ProgramTerminalStatus::Completed);
    EXPECT_EQ(fixture.starts->load(std::memory_order_relaxed), 1U);

    const auto retry = adapter.start(collaboration_to_message(envelope), "run-b", "context-1");
    EXPECT_EQ(retry.run_id(), "run-b");
    EXPECT_EQ(retry.wait().status(), neograph::program::ProgramTerminalStatus::Completed);
    EXPECT_EQ(fixture.starts->load(std::memory_order_relaxed), 1U);
}

TEST(A2ACollaboration, RecoveryRejectsDuplicateRunBeforeAnyDispatch) {
    ProgramAdapterFixture fixture;
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    const auto first = make_program_envelope(accepted, fixture.admitted_version());
    const auto duplicate = CollaborationEnvelope::bind_program(
        CollaborationEnvelope::create(accepted, "run-a", "run-b", "run-b", "context-2",
                                      "message-2", "correlation-2", 1, "request",
                                      "idem-program-duplicate", json{{"prompt", "duplicate"}}),
        fixture.admitted_version());
    auto mailbox = std::make_shared<CollaborationMailbox>("owner-b", "executor-b");
    mailbox->accept_link(accepted, "consent-secret");
    ASSERT_EQ(mailbox->submit_program(first, fixture.admitted_version(), persisted_invocation()),
              CollaborationSubmitResult::Accepted);
    ASSERT_EQ(mailbox->submit_program(duplicate, fixture.admitted_version(), persisted_invocation()),
              CollaborationSubmitResult::Accepted);

    ProgramAgentAdapter adapter(fixture.runtime, fixture.admitted_version(), "owner-b", mailbox);
    EXPECT_THROW(adapter.recover_pending(), ProgramA2ARequestError);
    EXPECT_EQ(fixture.starts->load(std::memory_order_relaxed), 0U);
}

TEST(A2ACollaboration, RecoveredMailboxReconnectsPublishedRunWithoutRedispatch) {
    ProgramAdapterFixture fixture;
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    const auto envelope = make_program_envelope(accepted, fixture.admitted_version());
    auto mailbox = std::make_shared<CollaborationMailbox>("owner-b", "executor-b");
    mailbox->accept_link(accepted, "consent-secret");

    ProgramAgentAdapter original(fixture.runtime, fixture.admitted_version(), "owner-b", mailbox);
    const auto first = original.start(collaboration_to_message(envelope), "run-b", "context-1");
    ASSERT_EQ(first.wait().status(), neograph::program::ProgramTerminalStatus::Completed);
    ASSERT_EQ(fixture.starts->load(std::memory_order_relaxed), 1U);

    auto reopened = std::make_shared<CollaborationMailbox>(
        CollaborationMailbox::parse(mailbox->serialize_canonical()));
    fixture.restart_after_mailbox_publication();
    ProgramAgentAdapter recovered_adapter(
        fixture.runtime, fixture.admitted_version(), "owner-b", reopened);
    const auto recovered = recovered_adapter.recover_pending();

    ASSERT_EQ(recovered.size(), 1U);
    EXPECT_EQ(recovered.front().run_id(), "run-b");
    EXPECT_EQ(recovered.front().wait().status(), neograph::program::ProgramTerminalStatus::Completed);
    EXPECT_EQ(fixture.starts->load(std::memory_order_relaxed), 1U);
}

TEST(A2ACollaboration, ServerRestoresMailboxRequestBeforeAcceptingTasks) {
    ProgramAdapterFixture fixture;
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    const auto envelope = make_program_envelope(accepted, fixture.admitted_version());

    CollaborationMailbox original("owner-b", "executor-b");
    original.accept_link(accepted, "consent-secret");
    ASSERT_EQ(original.submit_program(envelope, fixture.admitted_version(), persisted_invocation()),
              CollaborationSubmitResult::Accepted);
    auto reopened = std::make_shared<CollaborationMailbox>(
        CollaborationMailbox::parse(original.serialize_canonical()));
    fixture.restart_after_mailbox_publication();

    AgentCard card;
    card.name = "recovery-test";
    card.description = "Program mailbox recovery test";
    card.url = "http://127.0.0.1:0/";
    card.version = "1.0.0";
    card.protocol_version = "0.3.0";
    card.preferred_transport = "JSONRPC";
    card.default_input_modes = {"text/plain"};
    card.default_output_modes = {"text/plain"};
    A2AServer server(
        fixture.runtime, fixture.admitted_version(), "owner-b", card, reopened,
        [](std::string_view authorization) -> std::optional<CollaborationPeerIdentity> {
            if (authorization == "Bearer coordinator-a") {
                return CollaborationPeerIdentity{"owner-a", "coordinator-a"};
            }
            return std::nullopt;
        });
    ASSERT_TRUE(server.start_async("127.0.0.1", 0));

    A2AClient client("http://127.0.0.1:" + std::to_string(server.port()));
    client.set_authorization_header("Bearer coordinator-a");
    Task task;
    for (int attempt = 0; attempt < 100; ++attempt) {
        task = client.get_task("run-b");
        if (task.status.state == TaskState::Completed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    server.stop();

    EXPECT_EQ(task.status.state, TaskState::Completed);
    EXPECT_EQ(fixture.starts->load(std::memory_order_relaxed), 1U);
    const auto record = reopened->get("idem-program-recovery");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->state, CollaborationRecordState::Acknowledged);
}

TEST(A2ACollaboration, MailboxJournalRejectsLostTypedProgramRequestOnReopen) {
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    auto version = make_program_version();
    CollaborationMailbox receiver("owner-b", "executor-b");
    receiver.accept_link(accepted, "consent-secret");

    auto envelope = CollaborationEnvelope::bind_program(make_envelope(accepted), version);
    neograph::program::ProgramInvocation invocation;
    invocation.input = json{{"prompt", "typed"}};
    invocation.budget = neograph::program::RunBudget{1000, 1000, 1000, 1, 1, 20, 0, 0, 0};
    invocation.trace_id = "a2a:run-b";
    invocation.requested_run_id = "run-b";
    EXPECT_EQ(receiver.submit_program(envelope, version, invocation),
              CollaborationSubmitResult::Accepted);

    auto stored = json::parse(receiver.serialize_canonical());
    stored["records"][0]["program_request"] = nullptr;
    EXPECT_THROW(CollaborationMailbox::parse(stored.dump()), std::invalid_argument);
}
TEST(A2ACollaboration, AuthenticatedPeerGatesProgramRequestAndTaskAccess) {
    ProgramAdapterFixture fixture;
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    const auto envelope = make_program_envelope(accepted, fixture.admitted_version());
    auto mailbox = std::make_shared<CollaborationMailbox>("owner-b", "executor-b");
    mailbox->accept_link(accepted, "consent-secret");

    auto adapter = std::make_shared<ProgramAgentAdapter>(
        fixture.runtime, fixture.admitted_version(), "owner-b", mailbox);
    AgentCard card;
    card.name = "authenticated-collaboration";
    card.description = "Authenticated Program collaboration test";
    card.url = "http://127.0.0.1:0/";
    card.version = "1.0.0";
    card.protocol_version = "0.3.0";
    card.preferred_transport = "JSONRPC";
    card.default_input_modes = {"text/plain"};
    card.default_output_modes = {"text/plain"};
    A2AServer server(
        adapter, card, [](std::string_view authorization)
                           -> std::optional<CollaborationPeerIdentity> {
            if (authorization == "Bearer coordinator-a") {
                return CollaborationPeerIdentity{"owner-a", "coordinator-a"};
            }
            if (authorization == "Bearer executor-b") {
                return CollaborationPeerIdentity{"owner-b", "executor-b"};
            }
            return std::nullopt;
        });
    ASSERT_TRUE(server.start_async("127.0.0.1", 0));

    A2AClient client("http://127.0.0.1:" + std::to_string(server.port()));
    MessageSendParams request;
    request.message = collaboration_to_message(envelope);

    const auto unauthenticated = client.send_message_sync(request);
    EXPECT_EQ(unauthenticated.status.state, TaskState::AuthRequired);
    EXPECT_EQ(fixture.starts->load(std::memory_order_relaxed), 0U);

    client.set_authorization_header("Bearer executor-b");

    const auto wrong_peer = client.send_message_sync(request);
    EXPECT_EQ(wrong_peer.status.state, TaskState::AuthRequired);
    EXPECT_EQ(fixture.starts->load(std::memory_order_relaxed), 0U);

    client.set_authorization_header("Bearer coordinator-a");
    const auto completed = client.send_message_sync(request);
    EXPECT_EQ(completed.status.state, TaskState::Completed);
    EXPECT_EQ(fixture.starts->load(std::memory_order_relaxed), 1U);
    const auto streamed = client.send_message_stream(
        request, [](const StreamEvent&) { return true; });
    EXPECT_EQ(streamed.status.state, TaskState::Completed);

    client.set_authorization_header({});
    EXPECT_THROW(client.get_task("run-b"), std::runtime_error);
    EXPECT_THROW(client.cancel_task("run-b"), std::runtime_error);

    client.set_authorization_header("Bearer executor-b");
    EXPECT_EQ(client.get_task("run-b").status.state, TaskState::Completed);
    client.set_authorization_header("Bearer coordinator-a");
    EXPECT_EQ(client.cancel_task("run-b").status.state, TaskState::Completed);
    server.stop();
}

#endif

}  // namespace
