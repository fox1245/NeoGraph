#include <gtest/gtest.h>

#include <neograph/controlled_provider.h>
#include <neograph/runtime_interposition_controller.h>
#include <neograph/runtime_turn_assembler.h>
#include <neograph/strict_runtime.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>

#include <condition_variable>
#include <future>
#include <mutex>

using namespace neograph;

namespace {

std::string sha(char value) { return "sha256:" + std::string(64, value); }

ContextAssemblyReceipt assembly(CompletionRequest request) {
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

RuntimeHistoryRecord history() {
    RuntimeHistoryRecordData data;
    data.feed_id = "feed";
    data.sequence = 1;
    data.message_id = "message";
    data.trust = RuntimeTrustClass::UntrustedInput;
    data.message = {"user", "hello"};
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

class RecordingProvider final : public CompletionProvider {
public:
    std::string get_name() const override { return "test-provider"; }
    int calls = 0;
    CompletionMode mode = CompletionMode::COLLECT;
    bool callback = true;
    std::vector<ChatMessage> messages;
    std::shared_ptr<graph::CancelToken> cancellation;
    const std::string* persisted_receipt = nullptr;
    bool receipt_precedes_call = false;
    bool fail = false;
protected:
    asio::awaitable<ChatCompletion> do_invoke(CompletionRequest request) override {
        ++calls;
        mode = request.mode();
        callback = static_cast<bool>(request.on_chunk());
        cancellation = request.params().cancel_token;
        messages = request.params().messages;
        receipt_precedes_call = persisted_receipt && !persisted_receipt->empty();
        if (fail) throw std::runtime_error("provider outcome is unknown");
        ChatCompletion result;
        result.message = {"assistant", "ok"};
        co_return result;
    }
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
    provider->persisted_receipt = &store->receipt;
    CompletionParams params;
    params.model = "model";
    auto result = controlled.dispatch("dispatch-1", assembly(CompletionRequest::stream(params)),
                                      CompletionRequest::stream(params));
    EXPECT_EQ(result.message.content, "ok");
    EXPECT_EQ(provider->calls, 1);
    EXPECT_EQ(provider->mode, CompletionMode::STREAM);
    EXPECT_FALSE(provider->callback);
    EXPECT_TRUE(provider->receipt_precedes_call);
    ASSERT_FALSE(store->receipt.empty());
    const auto receipt = ProviderDispatchReceipt::parse(store->receipt);
    const auto assembled = assembly(CompletionRequest::stream(params));
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
    CompletionParams params;
    params.model = "model";
    EXPECT_THROW(controlled.dispatch("dispatch-2", assembly(CompletionRequest::collect(params)),
                                     CompletionRequest::collect(params)), std::runtime_error);
    EXPECT_EQ(provider->calls, 0);
}

TEST(ControlledProvider, RejectsRequestThatDoesNotMatchAssemblyReceipt) {
    auto provider = std::make_shared<RecordingProvider>();
    auto store = std::make_shared<RecordingStore>();
    ControlledProvider controlled(provider, store, sha('f'));
    CompletionParams assembled_params;
    assembled_params.model = "model";
    CompletionParams altered_params;
    altered_params.model = "other-model";
    EXPECT_THROW(controlled.dispatch("dispatch-3", assembly(CompletionRequest::collect(assembled_params)),
                                      CompletionRequest::collect(altered_params)),
                 std::invalid_argument);
    EXPECT_TRUE(store->receipt.empty());
    EXPECT_EQ(provider->calls, 0);
}

TEST(ControlledProvider, RejectsDuplicateDispatchAndPreservesCollectCancellation) {
    auto provider = std::make_shared<RecordingProvider>();
    auto store = std::make_shared<InMemoryProviderDispatchReceiptStore>();
    ControlledProvider controlled(provider, store, sha('f'));
    CompletionParams params;
    params.model = "model";
    params.cancel_token = std::make_shared<graph::CancelToken>();
    const auto assembled = assembly(CompletionRequest::collect(params));
    EXPECT_EQ(controlled.dispatch("dispatch-once", assembled,
                                  CompletionRequest::collect(params)).message.content, "ok");
    try {
        (void)controlled.dispatch("dispatch-once", assembled, CompletionRequest::collect(params));
        FAIL() << "duplicate dispatch must require reconciliation";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("reconciliation_required"), std::string::npos);
    }
    EXPECT_EQ(store->state("dispatch-once"), ProviderDispatchState::Succeeded);
    EXPECT_EQ(provider->calls, 1);
    EXPECT_EQ(provider->mode, CompletionMode::COLLECT);
    EXPECT_FALSE(provider->callback);
    EXPECT_EQ(provider->cancellation, params.cancel_token);

    CompletionParams changed_params;
    changed_params.model = "other-model";
    const auto changed_assembly = assembly(CompletionRequest::collect(changed_params));
    EXPECT_THROW(controlled.dispatch("dispatch-once", changed_assembly,
                                       CompletionRequest::collect(changed_params)),
                  std::runtime_error);
    EXPECT_EQ(provider->calls, 1);
}

TEST(ControlledProvider, PersistsReconciliationWhenProviderOutcomeIsUnknown) {
    auto provider = std::make_shared<RecordingProvider>();
    provider->fail = true;
    auto store = std::make_shared<InMemoryProviderDispatchReceiptStore>();
    ControlledProvider controlled(provider, store, sha('f'));
    CompletionParams params;
    params.model = "model";
    EXPECT_THROW(controlled.dispatch(
                     "dispatch-unknown", assembly(CompletionRequest::collect(params)),
                     CompletionRequest::collect(params)),
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
    CompletionParams params;
    params.model = "model";
    EXPECT_THROW(controlled.dispatch("", assembly(CompletionRequest::collect(params)),
                                      CompletionRequest::collect(params)), std::invalid_argument);
}

TEST(ControlledProvider, PreCancelledDispatchWritesNoReceiptAndCallsNoProvider) {
    auto provider = std::make_shared<RecordingProvider>();
    auto store = std::make_shared<RecordingStore>();
    ControlledProvider controlled(provider, store, sha('f'));
    CompletionParams params;
    params.model = "model";
    params.cancel_token = std::make_shared<graph::CancelToken>();
    params.cancel_token->cancel();
    const auto assembled = assembly(CompletionRequest::collect(params));
    EXPECT_THROW(controlled.dispatch("cancelled", assembled,
                                     CompletionRequest::collect(params)),
                 graph::CancelledException);
    EXPECT_TRUE(store->receipt.empty());
    EXPECT_EQ(provider->calls, 0);
}

TEST(RuntimeInterpositionController, BlocksBeforeRawProviderDispatchWithoutAnActiveEpoch) {
    auto provider = std::make_shared<RecordingProvider>();
    auto contexts = std::make_shared<InMemoryContextStore>();
    auto receipts = std::make_shared<InMemoryProviderDispatchReceiptStore>();
    RuntimeInterpositionController controller(provider, contexts, receipts, sha('f'), 1000);
    CompletionParams params;
    params.model = "model";
    EXPECT_THROW(controller.invoke(params), std::runtime_error);
    EXPECT_EQ(provider->calls, 0);
}

TEST(RuntimeInterpositionController, StrictEpochRejectsAnUndeclaredReceiptStoreBeforeDispatch) {
    auto provider = std::make_shared<RecordingProvider>();
    auto contexts = std::make_shared<InMemoryContextStore>();
    auto receipts = std::make_shared<InMemoryProviderDispatchReceiptStore>();
    RuntimeInterpositionController controller(provider, contexts, receipts, sha('f'), 1000);
    EXPECT_THROW(controller.activate("owner", admitted_epoch(*contexts, RuntimeGuaranteeProfile::Strict)),
                 std::invalid_argument);
    EXPECT_EQ(provider->calls, 0);
}

TEST(RuntimeInterpositionController, AssemblesAndDispatchesThroughControlledBoundaryWithStreaming) {
    auto provider = std::make_shared<RecordingProvider>();
    auto contexts = std::make_shared<InMemoryContextStore>();
    auto receipts = std::make_shared<RecordingStore>();
    provider->persisted_receipt = &receipts->receipt;
    RuntimeInterpositionController controller(provider, contexts, receipts, sha('f'), 1000);
    controller.activate("owner", admitted_epoch(*contexts, RuntimeGuaranteeProfile::Strict));
    CompletionParams params;
    params.model = "model";
    params.messages = {{"user", "legacy state is ignored"}};
    EXPECT_EQ(controller.invoke(params, [](const std::string&) {}).message.content, "ok");
    EXPECT_EQ(provider->calls, 1);
    EXPECT_EQ(provider->mode, CompletionMode::STREAM);
    EXPECT_TRUE(provider->callback);
    EXPECT_TRUE(provider->receipt_precedes_call);
}

TEST(RuntimeInterpositionController, RetainsHostSlotsButReplacesCallerConversation) {
    auto provider = std::make_shared<RecordingProvider>();
    auto contexts = std::make_shared<InMemoryContextStore>();
    auto receipts = std::make_shared<RecordingStore>();
    RuntimeInterpositionController controller(provider, contexts, receipts, sha('f'), 1000);
    controller.activate("owner", admitted_epoch(*contexts, RuntimeGuaranteeProfile::Strict));
    CompletionParams params;
    params.model = "model";
    params.messages = {{"user", "untrusted caller duplicate"}};
    EXPECT_EQ(async::run_sync(controller.invoke_async(
                  std::move(params), {}, {{"system", "host instruction"}},
                  {{"user", "host task"}})).message.content, "ok");
    ASSERT_EQ(provider->messages.size(), 3u);
    EXPECT_EQ(provider->messages[0].content, "host instruction");
    EXPECT_EQ(provider->messages[1].content, "hello");
    EXPECT_EQ(provider->messages[2].content, "host task");
}

TEST(RuntimeInterpositionController, DoesNotDuplicateTrustedTaskThatMatchesAdmittedRawHistory) {
    auto provider = std::make_shared<RecordingProvider>();
    auto contexts = std::make_shared<InMemoryContextStore>();
    auto receipts = std::make_shared<RecordingStore>();
    RuntimeInterpositionController controller(provider, contexts, receipts, sha('f'), 1000);
    controller.activate("owner", admitted_epoch(*contexts, RuntimeGuaranteeProfile::Strict));
    CompletionParams params;
    params.model = "model";
    EXPECT_EQ(async::run_sync(controller.invoke_async(
                  std::move(params), {}, {{"system", "host instruction"}},
                  {{"user", "hello"}})).message.content, "ok");
    ASSERT_EQ(provider->messages.size(), 2u);
    EXPECT_EQ(provider->messages[0].content, "host instruction");
    EXPECT_EQ(provider->messages[1].content, "hello");
}

TEST(RuntimeInterpositionController, ClearBlocksAnInvocationFromAnOlderGeneration) {
    auto provider = std::make_shared<RecordingProvider>();
    auto contexts = std::make_shared<BlockingContextStore>();
    auto receipts = std::make_shared<RecordingStore>();
    RuntimeInterpositionController controller(provider, contexts, receipts, sha('f'), 1000);
    controller.activate("owner", admitted_epoch(contexts->inner, RuntimeGuaranteeProfile::Strict));
    CompletionParams params;
    params.model = "model";
    auto invoked = std::async(std::launch::async, [&] {
        try {
            (void)controller.invoke(params);
            return false;
        } catch (const std::runtime_error& error) {
            return std::string(error.what()).find("Context epoch changed") != std::string::npos;
        }
    });
    contexts->wait_until_hydrating();
    controller.clear();
    contexts->release();
    EXPECT_TRUE(invoked.get());
    EXPECT_EQ(provider->calls, 0);
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
    CompletionParams params;
    params.model = "model";
    EXPECT_EQ(profile.interposition()->invoke(params).message.content, "ok");
    EXPECT_FALSE(receipts->terminal.empty());
    profile.clear();
    EXPECT_FALSE(profile.active());

    EXPECT_THROW(StrictRuntimeProfile({provider, contexts, receipts,
                                       empty_hook_runtime(), sha('f'), 0, {}}),
                 std::invalid_argument);
}
