#include <gtest/gtest.h>

#include <neograph/controlled_provider.h>
#include <neograph/runtime_interposition_controller.h>
#include <neograph/runtime_turn_assembler.h>
#include <neograph/strict_runtime.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include "fixtures/typed_provider.h"
#include <neograph/provider_outcome_codec.h>
#include <atomic>
#include <chrono>
#include <utility>
#include <vector>

#include <condition_variable>
#include <future>
#include <mutex>

using namespace neograph;

namespace {

std::string sha(char value) { return "sha256:" + std::string(64, value); }

ContextAssemblyReceipt assembly(const PreparedProviderRequest& request) {
    ContextEpochData epoch_data;
    epoch_data.run_id = "run";
    epoch_data.sequence = 1;
    epoch_data.raw_window_digest = sha('a');
    const auto epoch = ContextEpoch::create(std::move(epoch_data));
    ContextAssemblyReceiptData data;
    data.context_epoch_id = epoch.id();
    data.normalized_request_digest = RuntimeTurnAssembler::normalized_request_digest(request);
    data.message_window_digest = sha('c');
    return ContextAssemblyReceipt::create(std::move(data), epoch, {});
}

ContextAssemblyReceipt assembly(Provider& provider, ProviderRequest request) {
    auto prepared = provider.prepare(std::move(request));
    if (!prepared.valid()) throw std::invalid_argument("fixture request must prepare");
    return assembly(prepared);
}

RuntimeHistoryRecord history() {
    RuntimeHistoryRecordData data;
    data.feed_id = "feed";
    data.sequence = 1;
    data.message_id = "message";
    data.trust = RuntimeTrustClass::UntrustedInput;
    data.message = test::message("hello", sp::Role::User);
    return RuntimeHistoryRecord::create(std::move(data));
}

ContextEpoch admitted_epoch(ContextStore& store, RuntimeGuaranteeProfile profile) {
    const ContextStoreFeed feed{"owner", "feed"};
    const auto record = history();
    EXPECT_EQ(store.append_history(feed, record, std::nullopt), ContextStoreAppendResult::Appended);
    const auto raw = store.snapshot_history(feed, 1, 1);
    ContextEpochData data;
    data.run_id = "run";
    data.sequence = 1;
    data.feed_id = "feed";
    data.raw_from_sequence = 1;
    data.raw_through_sequence = 1;
    data.raw_window_digest = raw.digest;
    data.guarantee_profile = profile;
    return ContextEpoch::create(std::move(data));
}

class RecordingStore final : public DurableProviderDispatchReceiptStore {
public:
    ProviderDispatchReceiptPutResult persist(const ProviderDispatchReceipt& value) override {
        receipt = value.serialize_canonical();
        return result;
    }
    ProviderDispatchReceiptPutResult result = ProviderDispatchReceiptPutResult::Stored;
    std::string receipt;
    std::string terminal;
    ProviderDispatchOutcomePutResult settle(
        std::string_view,
        const ProviderDispatchOutcomeReceipt& value) override {
        if (!terminal.empty()) {
            return terminal == value.serialize_canonical()
                       ? ProviderDispatchOutcomePutResult::AlreadyPresent
                       : ProviderDispatchOutcomePutResult::Conflict;
        }
        terminal = value.serialize_canonical();
        return ProviderDispatchOutcomePutResult::Stored;
    }
    std::optional<ProviderDispatchOutcomeReceipt> outcome(
        std::string_view,
        std::string_view) const override {
        if (terminal.empty()) return std::nullopt;
        return ProviderDispatchOutcomeReceipt::parse(terminal);
    }
};

class SettlementStorageUnavailable final : public std::runtime_error {
public:
    SettlementStorageUnavailable() : std::runtime_error("fixture settlement storage unavailable") {}
};

// Dispatch admission uses a real idempotent journal. Only the post-effect
// storage operation is unavailable, as after a durable connection/commit loss.
class SettlementFaultStore final : public ProviderDispatchReceiptStore,
                                   public ProviderDispatchOutcomeStore {
public:
    SettlementFaultStore(std::shared_ptr<InMemoryProviderDispatchReceiptStore> journal,
                         std::exception_ptr settlement_failure = {})
        : journal_(std::move(journal)), settlement_failure_(std::move(settlement_failure)) {}
    ProviderDispatchReceiptPutResult persist(const ProviderDispatchReceipt& receipt) override {
        return journal_->persist(receipt);
    }
    ProviderDispatchReceiptPutResult persist(std::string_view owner,
                                             const ProviderDispatchReceipt& receipt) override {
        return journal_->persist(owner, receipt);
    }
    ProviderDispatchState state(std::string_view dispatch) const override {
        return journal_->state(dispatch);
    }
    ProviderDispatchState state(std::string_view owner, std::string_view dispatch) const override {
        return journal_->state(owner, dispatch);
    }
    ProviderDispatchOutcomePutResult settle(std::string_view owner,
                                            const ProviderDispatchOutcomeReceipt& outcome) override {
        if (settlement_failure_) std::rethrow_exception(settlement_failure_);
        return journal_->settle(owner, outcome);
    }
    std::optional<ProviderDispatchOutcomeReceipt> outcome(
        std::string_view owner, std::string_view dispatch) const override {
        return journal_->outcome(owner, dispatch);
    }
private:
    std::shared_ptr<InMemoryProviderDispatchReceiptStore> journal_;
    std::exception_ptr settlement_failure_;
};

struct RecordingProviderState {
    int calls = 0;
    ProviderMode mode = ProviderMode::Collect;
    std::vector<sp::Message> messages;
    const std::string* persisted_receipt = nullptr;
    bool receipt_precedes_call = false;
    bool fail = false;
};

class RecordingProvider final : public test::LocalProvider {
public:
    RecordingProvider() : RecordingProvider(std::make_shared<RecordingProviderState>()) {}
    std::shared_ptr<RecordingProviderState> state;
private:
    explicit RecordingProvider(std::shared_ptr<RecordingProviderState> probe)
        : LocalProvider(
            [probe](ProviderRequest request, const PreparedProviderRequest& prepared,
                    const EventCallback&) -> asio::awaitable<sp::runtime::Result> {
                ++probe->calls;
                probe->mode = prepared.mode();
                probe->messages = std::get<sp::chat::Request>(request.payload).canonical_messages;
                probe->receipt_precedes_call = probe->persisted_receipt && !probe->persisted_receipt->empty();
                if (probe->fail) throw std::runtime_error("provider outcome is unknown");
                co_return test::success("ok");
            }, "test-provider"), state(std::move(probe)) {}
};

class BlockingContextStore final : public ContextStore {
public:
    ContextStoreAppendResult append_history(const ContextStoreFeed& feed,
        const RuntimeHistoryRecord& record, const std::optional<std::string>& head) override {
        return inner.append_history(feed, record, head);
    }
    ContextStoreHead history_head(const ContextStoreFeed& feed) const override {
        return inner.history_head(feed);
    }
    ContextHistoryRange snapshot_history(const ContextStoreFeed& feed, std::uint64_t from,
        std::uint64_t through) const override {
        return inner.snapshot_history(feed, from, through);
    }
    std::string hydrate_history(const ContextHistoryRange& range) const override {
        std::unique_lock lock(mutex);
        hydrating = true;
        ready.notify_one();
        proceed.wait(lock, [this] { return released; });
        return inner.hydrate_history(range);
    }
    std::vector<RuntimeHistoryRecord> hydrate_records(const ContextHistoryRange& range) const override {
        (void)hydrate_history(range);
        return inner.hydrate_records(range);
    }
    ContextArtifactPutResult put_artifact(std::string_view owner,
        const ContextArtifact& artifact) override {
        return inner.put_artifact(owner, artifact);
    }
    std::optional<ContextArtifact> get_artifact(std::string_view owner,
        std::string_view id) const override {
        return inner.get_artifact(owner, id);
    }

    void wait_until_hydrating() {
        std::unique_lock lock(mutex);
        ready.wait(lock, [this] { return hydrating; });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        proceed.notify_one();
    }

    InMemoryContextStore inner;
private:
    mutable std::mutex mutex;
    mutable std::condition_variable ready;
    mutable std::condition_variable proceed;
    mutable bool hydrating = false;
    mutable bool released = false;
};

class DurableMemoryContextStore final : public DurableContextStore {
public:
    ContextStoreAppendResult append_history(
        const ContextStoreFeed& feed, const RuntimeHistoryRecord& record,
        const std::optional<std::string>& head) override {
        return inner.append_history(feed, record, head);
    }
    ContextStoreHead history_head(const ContextStoreFeed& feed) const override {
        return inner.history_head(feed);
    }
    ContextHistoryRange snapshot_history(
        const ContextStoreFeed& feed, std::uint64_t from,
        std::uint64_t through) const override {
        return inner.snapshot_history(feed, from, through);
    }
    std::string hydrate_history(const ContextHistoryRange& range) const override {
        return inner.hydrate_history(range);
    }
    std::vector<RuntimeHistoryRecord> hydrate_records(const ContextHistoryRange& range) const override {
        return inner.hydrate_records(range);
    }
    ContextArtifactPutResult put_artifact(
        std::string_view owner, const ContextArtifact& artifact) override {
        return inner.put_artifact(owner, artifact);
    }
    std::optional<ContextArtifact> get_artifact(
        std::string_view owner, std::string_view id) const override {
        return inner.get_artifact(owner, id);
    }
    InMemoryContextStore inner;
};

std::shared_ptr<HookRuntime> empty_hook_runtime() {
    HookTargetResolver resolver = [](std::string_view)
        -> std::optional<HookTargetContract> { return std::nullopt; };
    auto registry = std::make_shared<HookRegistry>(resolver);
    auto adapter = std::make_shared<NativeHookExecutionAdapter>(
        resolver, std::make_shared<ToolExecutionController>());
    return std::make_shared<HookRuntime>(std::make_shared<MandatoryHookRunner>(
        std::make_shared<InMemoryHookJournal>(), std::move(registry),
        std::move(adapter)));
}

}  // namespace

TEST(ControlledProvider, PersistsDistinctDispatchReceiptBeforeStreamDispatch) {
    auto provider = std::make_shared<RecordingProvider>();
    auto store = std::make_shared<RecordingStore>();
    ControlledProvider controlled(provider, store, sha('f'));
    provider->state->persisted_receipt = &store->receipt;
    auto request = test::request("model", "hello", ProviderMode::Stream);
    auto prepared = provider->prepare(request);
    ASSERT_TRUE(prepared.valid());
    const auto assembled = assembly(prepared);
    auto result = controlled.dispatch_prepared("dispatch-1", assembled, std::move(prepared));
    EXPECT_EQ(test::text(result), "ok");
    EXPECT_EQ(provider->state->calls, 1);
    EXPECT_EQ(provider->state->mode, ProviderMode::Stream);
    EXPECT_TRUE(provider->state->receipt_precedes_call);
    ASSERT_FALSE(store->receipt.empty());
    const auto receipt = ProviderDispatchReceipt::parse(store->receipt);
    EXPECT_NE(receipt.id(), assembled.id());
    EXPECT_EQ(receipt.assembly_receipt_id(), assembled.id());
    EXPECT_EQ(receipt.dispatch_id(), "dispatch-1");
    EXPECT_EQ(receipt.provider_binding_identity(), sha('f'));
    EXPECT_EQ(receipt.model(), "model");
    ASSERT_FALSE(store->terminal.empty());
    const auto terminal = ProviderDispatchOutcomeReceipt::parse(store->terminal);
    EXPECT_EQ(terminal.dispatch_receipt_id(), receipt.id());
    EXPECT_EQ(terminal.state(), ProviderDispatchState::Succeeded);
    EXPECT_TRUE(terminal.error().empty());
    EXPECT_FALSE(terminal.response_digest().empty());
}

TEST(ControlledProvider, DoesNotCallProviderWhenReceiptPersistenceConflicts) {
    auto provider = std::make_shared<RecordingProvider>();
    auto store = std::make_shared<RecordingStore>();
    store->result = ProviderDispatchReceiptPutResult::Conflict;
    ControlledProvider controlled(provider, store, sha('f'));
    auto request = test::request("model");
    EXPECT_THROW(controlled.dispatch("dispatch-2", assembly(*provider, request), request),
                 std::runtime_error);
    EXPECT_EQ(provider->state->calls, 0);
}

TEST(ControlledProvider, RejectsRequestThatDoesNotMatchAssemblyReceipt) {
    auto provider = std::make_shared<RecordingProvider>();
    auto store = std::make_shared<RecordingStore>();
    ControlledProvider controlled(provider, store, sha('f'));
    auto assembled_request = test::request("model");
    auto altered_request = test::request("other-model");
    EXPECT_THROW(controlled.dispatch("dispatch-3", assembly(*provider, assembled_request),
                                      altered_request), std::invalid_argument);
    EXPECT_TRUE(store->receipt.empty());
    EXPECT_EQ(provider->state->calls, 0);
}

TEST(ControlledProvider, RejectsDuplicateDispatch) {
    auto provider = std::make_shared<RecordingProvider>();
    auto store = std::make_shared<InMemoryProviderDispatchReceiptStore>();
    ControlledProvider controlled(provider, store, sha('f'));
    auto request = test::request("model");
    request.cancel_token = std::make_shared<graph::CancelToken>();
    const auto assembled = assembly(*provider, request);
    EXPECT_EQ(test::text(controlled.dispatch("dispatch-once", assembled, request)), "ok");
    EXPECT_THROW(controlled.dispatch("dispatch-once", assembled, request), std::runtime_error);
    EXPECT_EQ(store->state("dispatch-once"), ProviderDispatchState::Succeeded);
    EXPECT_EQ(provider->state->calls, 1);
    EXPECT_EQ(provider->state->mode, ProviderMode::Collect);

    auto changed_request = test::request("other-model");
    const auto changed_assembly = assembly(*provider, changed_request);
    EXPECT_THROW(controlled.dispatch("dispatch-once", changed_assembly, changed_request),
                 std::runtime_error);
    EXPECT_EQ(provider->state->calls, 1);
}

TEST(ControlledProvider, PersistsReconciliationWhenProviderOutcomeIsUnknown) {
    auto provider = std::make_shared<RecordingProvider>();
    provider->state->fail = true;
    auto store = std::make_shared<InMemoryProviderDispatchReceiptStore>();
    ControlledProvider controlled(provider, store, sha('f'));
    auto request = test::request("model");
    EXPECT_THROW(controlled.dispatch("dispatch-unknown", assembly(*provider, request), request),
                 std::runtime_error);
    EXPECT_EQ(store->state("dispatch-unknown"),
              ProviderDispatchState::ReconciliationRequired);
    const auto terminal = store->outcome({}, "dispatch-unknown");
    ASSERT_TRUE(terminal);
    EXPECT_EQ(terminal->state(), ProviderDispatchState::ReconciliationRequired);
    EXPECT_FALSE(terminal->error().empty());
}

TEST(ProviderDispatchOutcomeReceipt, RejectsForgedAndNonTerminalOutcomes) {
    const auto success = ProviderDispatchOutcomeReceipt::create(
        {"dispatch", sha('a'), ProviderDispatchState::Succeeded, sha('b'), {}});
    EXPECT_EQ(ProviderDispatchOutcomeReceipt::parse(success.serialize_canonical()).id(),
              success.id());
    EXPECT_THROW(ProviderDispatchOutcomeReceipt::create(
                     {"dispatch", sha('a'), ProviderDispatchState::AdmittedPending,
                      sha('b'), {}}),
                 std::invalid_argument);
    EXPECT_THROW(ProviderDispatchOutcomeReceipt::create(
                     {"dispatch", sha('a'), ProviderDispatchState::Succeeded,
                      {}, {}}),
                 std::invalid_argument);
}

TEST(ControlledProvider, ValidatesDispatchAndBindingIdentities) {
    auto provider = std::make_shared<RecordingProvider>();
    auto store = std::make_shared<RecordingStore>();
    EXPECT_THROW(ControlledProvider(provider, store, "not-an-identity"), std::invalid_argument);
    ControlledProvider controlled(provider, store, sha('f'));
    auto request = test::request("model");
    EXPECT_THROW(controlled.dispatch("", assembly(*provider, request), request), std::invalid_argument);
}

TEST(ControlledProvider, PreCancelledDispatchWritesNoReceiptAndCallsNoProvider) {
    auto provider = std::make_shared<RecordingProvider>();
    auto store = std::make_shared<RecordingStore>();
    ControlledProvider controlled(provider, store, sha('f'));
    auto request = test::request("model");
    const auto assembled = assembly(*provider, request);
    request.cancel_token = std::make_shared<graph::CancelToken>();
    request.cancel_token->cancel();
    const auto result = controlled.dispatch("cancelled", assembled, request);
    ASSERT_TRUE(result);
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    EXPECT_EQ(std::get<sp::Failure>(*result).error.kind, sp::ErrorKind::Cancelled);
    EXPECT_EQ(std::get<sp::Failure>(*result).error.retry_safety, sp::RetrySafety::NotSent);
    EXPECT_TRUE(store->receipt.empty());
    EXPECT_EQ(provider->state->calls, 0);
}

TEST(ControlledProvider, SettlementStorageFailureRetainsOwnedOutcomeAndPreventsReplayAfterRepair) {
    auto journal = std::make_shared<InMemoryProviderDispatchReceiptStore>();
    const auto storage_cause = std::make_exception_ptr(SettlementStorageUnavailable{});
    auto store = std::make_shared<SettlementFaultStore>(journal, storage_cause);
    auto effects = std::make_shared<std::atomic<unsigned>>(0);
    sp::Completion completion;
    auto message = test::message("delivered");
    message.id = "owned-response";
    message.parts.emplace_back(sp::Opaque{"provider-detail", test::document(R"({"nested":[null,{"id":9}]})")});
    message.parts.emplace_back(sp::InvalidToolCall{
        "partial-call", "read", sp::ToolCallKind::ClientExecuted,
        "{\"path\":", sp::InvalidReason::Truncated});
    completion.messages = {std::move(message), test::message("tail")};
    completion.stop = {sp::StopKind::MaxTokens, "length"};
    completion.usage = test::usage(0, std::nullopt, std::nullopt, sp::UsageStage::Final);
    completion.wire_envelope = test::document(R"({"id":"owned-response","output_metadata":{"complete":true}})");
    auto expected = std::make_shared<const sp::Outcome>(std::move(completion));
    const auto projection = provider_codec::observe_outcome(*expected);
    auto provider = std::make_shared<test::LocalProvider>(
        [effects, expected](ProviderRequest, const PreparedProviderRequest&,
                            const test::LocalProvider::EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            effects->fetch_add(1);
            co_return expected;
        }, "settlement-loss");
    sp::runtime::Result retained;
    {
        ControlledProvider controlled(provider, store, sha('f'));
        auto prepared = provider->prepare(test::request("model"));
        ASSERT_TRUE(prepared.valid());
        const auto assembled = assembly(prepared);
        try {
            (void)controlled.dispatch_prepared("tenant", "committed-effect", assembled, std::move(prepared));
            FAIL() << "post-effect settlement storage loss must remain observable";
        } catch (const ProviderDispatchOutcomePersistenceError& error) {
            retained = error.outcome();
            EXPECT_EQ(retained, expected);
            EXPECT_EQ(error.cause(), storage_cause);
            EXPECT_FALSE(error.delivery_error());
            EXPECT_THROW(std::rethrow_exception(error.cause()), SettlementStorageUnavailable);
        }
    }
    EXPECT_EQ(effects->load(), 1u);
    EXPECT_EQ(journal->state("tenant", "committed-effect"), ProviderDispatchState::AdmittedPending);
    EXPECT_FALSE(journal->outcome("tenant", "committed-effect"));
    ASSERT_TRUE(retained);
    EXPECT_EQ(provider_codec::observe_outcome(*retained), projection);
    const auto& actual = std::get<sp::Completion>(*retained);
    ASSERT_TRUE(actual.usage.input_total);
    EXPECT_EQ(actual.usage.input_total->value, 0u);
    EXPECT_FALSE(actual.usage.output_total);
    EXPECT_FALSE(actual.usage.total);
    ASSERT_EQ(actual.messages.size(), 2u);
    ASSERT_EQ(actual.messages.front().parts.size(), 3u);
    EXPECT_EQ(std::get<sp::InvalidToolCall>(actual.messages.front().parts[2]).raw_fragment,
              "{\"path\":");
    EXPECT_EQ(std::get<sp::Text>(actual.messages.back().parts.at(0)).value, "tail");
    EXPECT_EQ(actual.stop.kind, sp::StopKind::MaxTokens);

    // Reconnecting repaired storage does not grant a second provider effect:
    // the real admitted receipt still requires reconciliation, not redispatch.
    store.reset();
    {
        auto repaired = std::make_shared<SettlementFaultStore>(journal);
        ControlledProvider retry(provider, repaired, sha('f'));
        auto prepared = provider->prepare(test::request("model"));
        ASSERT_TRUE(prepared.valid());
        const auto assembled = assembly(prepared);
        EXPECT_THROW(retry.dispatch_prepared("tenant", "committed-effect", assembled, std::move(prepared)),
                     std::runtime_error);
        EXPECT_EQ(effects->load(), 1u);
        EXPECT_EQ(journal->state("tenant", "committed-effect"), ProviderDispatchState::AdmittedPending);
        EXPECT_FALSE(journal->outcome("tenant", "committed-effect"));
    }
    provider.reset();
    journal.reset();
    expected.reset();
    EXPECT_EQ(provider_codec::observe_outcome(*retained), projection);
}

TEST(ControlledProvider, InvalidPreparationReturnsTypedFailureBeforeAnyReceiptOrEvent) {
    auto provider = std::make_shared<RecordingProvider>();
    auto store = std::make_shared<RecordingStore>();
    ControlledProvider controlled(provider, store, sha('f'));
    const auto assembled = assembly(*provider, test::request("model"));
    std::vector<std::pair<ProviderRequest, sp::ErrorKind>> rejected;
    rejected.emplace_back(test::request(""), sp::ErrorKind::InvalidRequest);
    auto empty_messages = test::request("model");
    std::get<sp::chat::Request>(empty_messages.payload).canonical_messages.clear();
    rejected.emplace_back(std::move(empty_messages), sp::ErrorKind::InvalidRequest);
    auto unauthenticated_opaque = test::request("model");
    std::get<sp::chat::Request>(unauthenticated_opaque.payload).canonical_messages.front().parts.emplace_back(
        sp::Opaque{"future-provider-part", test::document(R"({"opaque":"preserve"})")});
    rejected.emplace_back(std::move(unauthenticated_opaque), sp::ErrorKind::ReplayIneligible);
    auto invalid_mode = test::request("model");
    invalid_mode.mode = static_cast<ProviderMode>(255);
    rejected.emplace_back(std::move(invalid_mode), sp::ErrorKind::InvalidRequest);
    auto expired = test::request("model");
    expired.options.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    rejected.emplace_back(std::move(expired), sp::ErrorKind::DeadlineExceeded);
    int events = 0;
    for (auto& [request, kind] : rejected) {
        request.on_event = [&](const sp::Event&) { ++events; };
        auto prepared = provider->prepare(request);
        ASSERT_FALSE(prepared.valid());
        ASSERT_NE(prepared.error(), nullptr);
        EXPECT_EQ(prepared.error()->kind, kind);
        const auto result = controlled.dispatch_prepared("invalid-prepare", assembled, std::move(prepared));
        ASSERT_TRUE(result);
        ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
        const auto& failed = std::get<sp::Failure>(*result);
        EXPECT_EQ(failed.error.kind, kind);
        EXPECT_EQ(failed.error.retry_safety, sp::RetrySafety::NotSent);
        EXPECT_TRUE(failed.partial.messages.empty());
        EXPECT_FALSE(failed.partial.usage.input_total);
        EXPECT_FALSE(failed.partial.usage.output_total);
        EXPECT_TRUE(store->receipt.empty());
        EXPECT_TRUE(store->terminal.empty());
        EXPECT_EQ(provider->state->calls, 0);
        EXPECT_EQ(events, 0);
    }
    auto invalid = test::request("");
    const auto result = controlled.dispatch("invalid-direct", assembled, std::move(invalid));
    ASSERT_TRUE(result);
    EXPECT_EQ(std::get<sp::Failure>(*result).error.kind, sp::ErrorKind::InvalidRequest);
    EXPECT_TRUE(store->receipt.empty());
    EXPECT_TRUE(store->terminal.empty());
    EXPECT_THROW(controlled.dispatch_prepared("empty-preparation", assembled, PreparedProviderRequest{}),
                 std::invalid_argument);
    EXPECT_TRUE(store->receipt.empty());
}

TEST(ControlledProvider, PreparedDispatchBindsOriginalBodyAndModeDespiteCallerMutation) {
    auto provider = std::make_shared<RecordingProvider>();
    auto store = std::make_shared<RecordingStore>();
    ControlledProvider controlled(provider, store, sha('f'));
    auto request = test::request("model", "original", ProviderMode::Stream);
    auto prepared = provider->prepare(request);
    ASSERT_TRUE(prepared.valid());
    const auto assembled = assembly(prepared);
    const auto digest = Provider::request_digest(prepared);
    std::get<sp::chat::Request>(request.payload).model = "tampered-model";
    std::get<sp::chat::Request>(request.payload).canonical_messages.front() =
        test::message("tampered", sp::Role::User);
    request.mode = ProviderMode::Collect;
    EXPECT_EQ(test::text(controlled.dispatch_prepared("immutable", assembled, std::move(prepared))), "ok");
    const auto receipt = ProviderDispatchReceipt::parse(store->receipt);
    EXPECT_EQ(receipt.normalized_request_digest(), digest);
    EXPECT_EQ(receipt.model(), "model");
    EXPECT_EQ(receipt.mode(), ProviderMode::Stream);
    EXPECT_EQ(provider->state->mode, ProviderMode::Stream);
    ASSERT_EQ(provider->state->messages.size(), 1u);
    EXPECT_EQ(std::get<sp::Text>(provider->state->messages.front().parts.at(0)).value, "original");
}

TEST(ControlledProvider, RejectsModeChangeAgainstPreparedAssemblyBeforePersistence) {
    auto provider = std::make_shared<RecordingProvider>();
    auto store = std::make_shared<RecordingStore>();
    ControlledProvider controlled(provider, store, sha('f'));
    auto request = test::request("model", "same");
    const auto assembled = assembly(*provider, request);
    request.mode = ProviderMode::Stream;
    EXPECT_THROW(controlled.dispatch("mode-change", assembled, request), std::invalid_argument);
    EXPECT_TRUE(store->receipt.empty());
    EXPECT_TRUE(store->terminal.empty());
    EXPECT_EQ(provider->state->calls, 0);
}

TEST(ControlledProvider, TypedFailureRetainsPartialUsageAndSettlesByAcceptanceEvidence) {
    for (const auto safety : {sp::RetrySafety::NotSent, sp::RetrySafety::RejectedBeforeOutput,
                              sp::RetrySafety::PossiblyAccepted, sp::RetrySafety::OutputObserved}) {
        sp::Failure failure;
        failure.error.kind = sp::ErrorKind::Transport;
        failure.error.retry_safety = safety;
        failure.error.safe_message = "fixture transport failure";
        failure.partial.messages = {test::message("partial")};
        failure.partial.messages.front().parts.emplace_back(sp::Refusal{"denied", "policy"});
        failure.partial.usage = test::usage(0, std::nullopt, std::nullopt, sp::UsageStage::Partial);
        const auto expected = std::make_shared<const sp::Outcome>(std::move(failure));
        auto provider = std::make_shared<test::LocalProvider>(
            [expected](ProviderRequest, const PreparedProviderRequest&,
                       const test::LocalProvider::EventCallback&) -> asio::awaitable<sp::runtime::Result> {
                co_return expected;
            });
        auto store = std::make_shared<InMemoryProviderDispatchReceiptStore>();
        ControlledProvider controlled(provider, store, sha('f'));
        auto request = test::request("model");
        const auto result = controlled.dispatch("failed", assembly(*provider, request), request);
        EXPECT_EQ(result, expected);
        const auto& retained = std::get<sp::Failure>(*result);
        EXPECT_EQ(std::get<sp::Text>(retained.partial.messages.front().parts.at(0)).value, "partial");
        EXPECT_EQ(std::get<sp::Refusal>(retained.partial.messages.front().parts.at(1)).raw_code, "policy");
        ASSERT_TRUE(retained.partial.usage.input_total);
        EXPECT_EQ(retained.partial.usage.input_total->value, 0u);
        EXPECT_FALSE(retained.partial.usage.output_total);
        EXPECT_FALSE(retained.partial.usage.total);
        // Actual partial output contradicts either no-send label; it must not
        // release effect custody merely because retry metadata says otherwise.
        const auto state = ProviderDispatchState::ReconciliationRequired;
        EXPECT_EQ(store->state("failed"), state);
        ASSERT_TRUE(store->outcome({}, "failed"));
        EXPECT_EQ(store->outcome({}, "failed")->state(), state);
    }
}

TEST(ControlledProvider, DeferredPreparedDispatchOwnsProviderAndRequestAfterCallerDestruction) {
    auto store = std::make_shared<RecordingStore>();
    auto provider = std::make_shared<RecordingProvider>();
    auto state = provider->state;
    auto operation = [&] {
        ControlledProvider controlled(provider, store, sha('f'));
        auto prepared = provider->prepare(test::request("model", "owned-history"));
        const auto assembled = assembly(prepared);
        return controlled.dispatch_prepared_async("deferred", assembled, std::move(prepared));
    }();
    provider.reset();
    const auto result = async::run_sync(std::move(operation));
    EXPECT_EQ(test::text(result), "ok");
    EXPECT_EQ(state->calls, 1);
    ASSERT_EQ(state->messages.size(), 1u);
    EXPECT_EQ(std::get<sp::Text>(state->messages.front().parts.at(0)).value, "owned-history");
    EXPECT_EQ(ProviderDispatchOutcomeReceipt::parse(store->terminal).state(),
              ProviderDispatchState::Succeeded);
}

TEST(RuntimeInterpositionController, BlocksBeforeRawProviderDispatchWithoutAnActiveEpoch) {
    auto provider = std::make_shared<RecordingProvider>();
    auto contexts = std::make_shared<InMemoryContextStore>();
    auto receipts = std::make_shared<InMemoryProviderDispatchReceiptStore>();
    RuntimeInterpositionController controller(provider, contexts, receipts, sha('f'), 1000);
    EXPECT_THROW(controller.invoke(test::request("model")), std::runtime_error);
    EXPECT_EQ(provider->state->calls, 0);
}

TEST(RuntimeInterpositionController, StrictEpochRejectsAnUndeclaredReceiptStoreBeforeDispatch) {
    auto provider = std::make_shared<RecordingProvider>();
    auto contexts = std::make_shared<InMemoryContextStore>();
    auto receipts = std::make_shared<InMemoryProviderDispatchReceiptStore>();
    RuntimeInterpositionController controller(provider, contexts, receipts, sha('f'), 1000);
    EXPECT_THROW(controller.activate("owner", admitted_epoch(*contexts, RuntimeGuaranteeProfile::Strict)),
                 std::invalid_argument);
    EXPECT_EQ(provider->state->calls, 0);
}

TEST(RuntimeInterpositionController, AssemblesAndDispatchesThroughControlledBoundaryWithStreaming) {
    auto provider = std::make_shared<RecordingProvider>();
    auto contexts = std::make_shared<InMemoryContextStore>();
    auto receipts = std::make_shared<RecordingStore>();
    provider->state->persisted_receipt = &receipts->receipt;
    RuntimeInterpositionController controller(provider, contexts, receipts, sha('f'), 1000);
    controller.activate("owner", admitted_epoch(*contexts, RuntimeGuaranteeProfile::Strict));
    auto request = test::request("model", "caller state is ignored", ProviderMode::Stream);
    request.on_event = [](const sp::Event&) {};
    EXPECT_EQ(test::text(controller.invoke(std::move(request))), "ok");
    EXPECT_EQ(provider->state->calls, 1);
    EXPECT_EQ(provider->state->mode, ProviderMode::Stream);
    EXPECT_TRUE(provider->state->receipt_precedes_call);
}

TEST(RuntimeInterpositionController, RetainsHostSlotsButReplacesCallerConversation) {
    auto provider = std::make_shared<RecordingProvider>();
    auto contexts = std::make_shared<InMemoryContextStore>();
    auto receipts = std::make_shared<RecordingStore>();
    RuntimeInterpositionController controller(provider, contexts, receipts, sha('f'), 1000);
    controller.activate("owner", admitted_epoch(*contexts, RuntimeGuaranteeProfile::Strict));
    auto request = test::request("model", "untrusted caller duplicate");
    EXPECT_EQ(test::text(async::run_sync(controller.invoke_async(
                  std::move(request), {test::message("host instruction", sp::Role::System)},
                  {test::message("host task", sp::Role::User)}))), "ok");
    ASSERT_EQ(provider->state->messages.size(), 3u);
    EXPECT_EQ(std::get<sp::Text>(provider->state->messages[0].parts.at(0)).value, "host instruction");
    EXPECT_EQ(std::get<sp::Text>(provider->state->messages[1].parts.at(0)).value, "hello");
    EXPECT_EQ(std::get<sp::Text>(provider->state->messages[2].parts.at(0)).value, "host task");
}

TEST(RuntimeInterpositionController, DoesNotDuplicateTrustedTaskThatMatchesAdmittedRawHistory) {
    auto provider = std::make_shared<RecordingProvider>();
    auto contexts = std::make_shared<InMemoryContextStore>();
    auto receipts = std::make_shared<RecordingStore>();
    RuntimeInterpositionController controller(provider, contexts, receipts, sha('f'), 1000);
    controller.activate("owner", admitted_epoch(*contexts, RuntimeGuaranteeProfile::Strict));
    auto request = test::request("model");
    EXPECT_EQ(test::text(async::run_sync(controller.invoke_async(
                  std::move(request), {test::message("host instruction", sp::Role::System)},
                  {test::message("hello", sp::Role::User)}))), "ok");
    ASSERT_EQ(provider->state->messages.size(), 2u);
    EXPECT_EQ(std::get<sp::Text>(provider->state->messages[0].parts.at(0)).value, "host instruction");
    EXPECT_EQ(std::get<sp::Text>(provider->state->messages[1].parts.at(0)).value, "hello");
}

TEST(RuntimeInterpositionController, ClearBlocksAnInvocationFromAnOlderGeneration) {
    auto provider = std::make_shared<RecordingProvider>();
    auto contexts = std::make_shared<BlockingContextStore>();
    auto receipts = std::make_shared<RecordingStore>();
    RuntimeInterpositionController controller(provider, contexts, receipts, sha('f'), 1000);
    controller.activate("owner", admitted_epoch(contexts->inner, RuntimeGuaranteeProfile::Strict));
    auto request = test::request("model");
    auto invoked = std::async(std::launch::async, [&] {
        try {
            (void)controller.invoke(request);
            return false;
        } catch (const std::runtime_error&) {
            return true;
        }
    });
    contexts->wait_until_hydrating();
    controller.clear();
    contexts->release();
    EXPECT_TRUE(invoked.get());
    EXPECT_EQ(provider->state->calls, 0);
}

TEST(StrictRuntimeProfile, RequiresDurableDependenciesAndStrictEpoch) {
    auto provider = std::make_shared<RecordingProvider>();
    auto contexts = std::make_shared<DurableMemoryContextStore>();
    auto receipts = std::make_shared<RecordingStore>();
    StrictRuntimeProfile profile({provider, contexts, receipts,
                                  empty_hook_runtime(), sha('f'), 1000, {}});
    ContextEpochData recorded_data;
    recorded_data.run_id = "recorded";
    recorded_data.sequence = 1;
    recorded_data.guarantee_profile = RuntimeGuaranteeProfile::Recorded;
    EXPECT_THROW(profile.activate(
                     "owner", ContextEpoch::create(std::move(recorded_data))),
                 std::invalid_argument);
    profile.activate("owner", admitted_epoch(
                                  contexts->inner,
                                  RuntimeGuaranteeProfile::Strict));
    EXPECT_TRUE(profile.active());
    EXPECT_EQ(test::text(profile.interposition()->invoke(test::request("model"))), "ok");
    EXPECT_FALSE(receipts->terminal.empty());
    profile.clear();
    EXPECT_FALSE(profile.active());

    EXPECT_THROW(StrictRuntimeProfile({provider, contexts, receipts,
                                       empty_hook_runtime(), sha('f'), 0, {}}),
                 std::invalid_argument);
}
