#include "json_bridge.h"

#include <neograph/context_store.h>
#include <neograph/context_transform.h>
#include <neograph/controlled_provider.h>
#include <neograph/graph/engine.h>
#include <neograph/runtime_interposition_controller.h>
#include <neograph/runtime_turn_assembler.h>
#include <neograph/strict_runtime.h>

#ifdef NEOGRAPH_PYBIND_HAS_SQLITE
#include <neograph/sqlite_runtime_stores.h>
#endif

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>

namespace py = pybind11;

namespace neograph::pybind {
namespace {

class PyDurableProviderDispatchReceiptStore final
    : public DurableProviderDispatchReceiptStore {
public:
    using DurableProviderDispatchReceiptStore::DurableProviderDispatchReceiptStore;

    ProviderDispatchReceiptPutResult persist(const ProviderDispatchReceipt& receipt) override {
        PYBIND11_OVERRIDE_PURE(ProviderDispatchReceiptPutResult,
                               DurableProviderDispatchReceiptStore, persist, receipt);
    }
    ProviderDispatchOutcomePutResult settle(
        std::string_view owner_scope,
        const ProviderDispatchOutcomeReceipt& outcome) override {
        PYBIND11_OVERRIDE_PURE(ProviderDispatchOutcomePutResult,
                               DurableProviderDispatchReceiptStore, settle,
                               owner_scope, outcome);
    }
    std::optional<ProviderDispatchOutcomeReceipt> outcome(
        std::string_view owner_scope,
        std::string_view dispatch_id) const override {
        PYBIND11_OVERRIDE_PURE(std::optional<ProviderDispatchOutcomeReceipt>,
                               DurableProviderDispatchReceiptStore, outcome,
                               owner_scope, dispatch_id);
    }
};

CompletionRequest request_from(CompletionParams params, bool stream, StreamCallback on_chunk) {
    return stream ? CompletionRequest::stream(std::move(params), std::move(on_chunk))
                  : CompletionRequest::collect(std::move(params));
}

template <typename T>
void bind_identity_methods(py::class_<T>& cls) {
    cls.def_property_readonly("id", &T::id)
       .def("serialize_canonical", &T::serialize_canonical);
}

}  // namespace

void init_runtime_context(py::module_& m) {
    py::enum_<RuntimeTrustClass>(m, "RuntimeTrustClass")
        .value("UntrustedInput", RuntimeTrustClass::UntrustedInput)
        .value("ModelOutput", RuntimeTrustClass::ModelOutput)
        .value("ToolOutput", RuntimeTrustClass::ToolOutput)
        .value("Developer", RuntimeTrustClass::Developer)
        .value("HostPolicy", RuntimeTrustClass::HostPolicy);
    py::enum_<ContextArtifactKind>(m, "ContextArtifactKind")
        .value("RawHistory", ContextArtifactKind::RawHistory)
        .value("DerivedContext", ContextArtifactKind::DerivedContext)
        .value("RequiredSkill", ContextArtifactKind::RequiredSkill)
        .value("HookOutput", ContextArtifactKind::HookOutput)
        .value("HardConstraint", ContextArtifactKind::HardConstraint);
    py::enum_<ContextPlacement>(m, "ContextPlacement")
        .value("BeforeLatestUser", ContextPlacement::BeforeLatestUser)
        .value("AfterLatestUser", ContextPlacement::AfterLatestUser);
    py::enum_<RuntimeGuaranteeProfile>(m, "RuntimeGuaranteeProfile")
        .value("Legacy", RuntimeGuaranteeProfile::Legacy)
        .value("Recorded", RuntimeGuaranteeProfile::Recorded)
        .value("Strict", RuntimeGuaranteeProfile::Strict);
    py::enum_<ContextStoreAppendResult>(m, "ContextStoreAppendResult")
        .value("Appended", ContextStoreAppendResult::Appended)
        .value("AlreadyPresent", ContextStoreAppendResult::AlreadyPresent)
        .value("Conflict", ContextStoreAppendResult::Conflict);
    py::enum_<ContextArtifactPutResult>(m, "ContextArtifactPutResult")
        .value("Stored", ContextArtifactPutResult::Stored)
        .value("AlreadyPresent", ContextArtifactPutResult::AlreadyPresent)
        .value("Conflict", ContextArtifactPutResult::Conflict);
    py::enum_<ProviderDispatchReceiptPutResult>(m, "ProviderDispatchReceiptPutResult")
        .value("Stored", ProviderDispatchReceiptPutResult::Stored)
        .value("AlreadyPresent", ProviderDispatchReceiptPutResult::AlreadyPresent)
        .value("Conflict", ProviderDispatchReceiptPutResult::Conflict);
    py::enum_<ProviderDispatchOutcomePutResult>(m, "ProviderDispatchOutcomePutResult")
        .value("Stored", ProviderDispatchOutcomePutResult::Stored)
        .value("AlreadyPresent", ProviderDispatchOutcomePutResult::AlreadyPresent)
        .value("Conflict", ProviderDispatchOutcomePutResult::Conflict)
        .value("MissingDispatch", ProviderDispatchOutcomePutResult::MissingDispatch);
    py::enum_<ProviderDispatchState>(m, "ProviderDispatchState")
        .value("Unavailable", ProviderDispatchState::Unavailable)
        .value("Missing", ProviderDispatchState::Missing)
        .value("AdmittedPending", ProviderDispatchState::AdmittedPending)
        .value("Succeeded", ProviderDispatchState::Succeeded)
        .value("Failed", ProviderDispatchState::Failed)
        .value("ReconciliationRequired", ProviderDispatchState::ReconciliationRequired);

    py::class_<RuntimeHistoryRecordData>(m, "RuntimeHistoryRecordData")
        .def(py::init<>()).def_readwrite("feed_id", &RuntimeHistoryRecordData::feed_id)
        .def_readwrite("sequence", &RuntimeHistoryRecordData::sequence)
        .def_readwrite("message_id", &RuntimeHistoryRecordData::message_id)
        .def_readwrite("trust", &RuntimeHistoryRecordData::trust)
        .def_readwrite("message", &RuntimeHistoryRecordData::message)
        .def_property("source_payload", [](const RuntimeHistoryRecordData& v) { return v.source_payload ? json_to_py(*v.source_payload) : py::none(); },
                      [](RuntimeHistoryRecordData& v, py::object x) { v.source_payload = x.is_none() ? std::nullopt : std::optional<json>(py_to_json(x)); })
        .def_readwrite("source_media_type", &RuntimeHistoryRecordData::source_media_type)
        .def_readwrite("predecessor_id", &RuntimeHistoryRecordData::predecessor_id);
    py::class_<RuntimeHistoryRecord> history(m, "RuntimeHistoryRecord");
    history.def_static("create", &RuntimeHistoryRecord::create).def_static("parse", &RuntimeHistoryRecord::parse)
        .def_property_readonly("feed_id", &RuntimeHistoryRecord::feed_id).def_property_readonly("sequence", &RuntimeHistoryRecord::sequence)
        .def_property_readonly("message_id", &RuntimeHistoryRecord::message_id).def_property_readonly("trust", &RuntimeHistoryRecord::trust)
        .def_property_readonly("message", &RuntimeHistoryRecord::message).def_property_readonly("predecessor_id", &RuntimeHistoryRecord::predecessor_id);
    bind_identity_methods(history);

    py::class_<ContextArtifactData>(m, "ContextArtifactData")
        .def(py::init<>()).def_readwrite("kind", &ContextArtifactData::kind).def_readwrite("producer_id", &ContextArtifactData::producer_id)
        .def_readwrite("source_digest", &ContextArtifactData::source_digest).def_readwrite("source_feed_id", &ContextArtifactData::source_feed_id)
        .def_readwrite("covers_from_sequence", &ContextArtifactData::covers_from_sequence).def_readwrite("covers_through_sequence", &ContextArtifactData::covers_through_sequence)
        .def_readwrite("media_type", &ContextArtifactData::media_type).def_readwrite("placement", &ContextArtifactData::placement)
        .def_readwrite("priority", &ContextArtifactData::priority).def_readwrite("required", &ContextArtifactData::required)
        .def_property("content", [](const ContextArtifactData& v) { return json_to_py(v.content); }, [](ContextArtifactData& v, py::object x) { v.content = py_to_json(x); });
    py::class_<ContextArtifact> artifact(m, "ContextArtifact");
    artifact.def_static("create", &ContextArtifact::create).def_static("parse", &ContextArtifact::parse)
        .def_property_readonly("kind", &ContextArtifact::kind).def_property_readonly("producer_id", &ContextArtifact::producer_id)
        .def_property_readonly("source_digest", &ContextArtifact::source_digest)
        .def_property_readonly("source_feed_id", &ContextArtifact::source_feed_id)
        .def_property_readonly("covers_from_sequence", &ContextArtifact::covers_from_sequence)
        .def_property_readonly("covers_through_sequence", &ContextArtifact::covers_through_sequence)
        .def_property_readonly("media_type", &ContextArtifact::media_type)
        .def_property_readonly("placement", &ContextArtifact::placement)
        .def_property_readonly("priority", &ContextArtifact::priority)
        .def_property_readonly("required", &ContextArtifact::required)
        .def_property_readonly("content_digest", &ContextArtifact::content_digest)
        .def_property_readonly("content", [](const ContextArtifact& v) { return json_to_py(v.content()); });
    bind_identity_methods(artifact);

    py::class_<ContextEpochData>(m, "ContextEpochData")
        .def(py::init<>()).def_readwrite("run_id", &ContextEpochData::run_id).def_readwrite("sequence", &ContextEpochData::sequence)
        .def_readwrite("predecessor_id", &ContextEpochData::predecessor_id).def_readwrite("feed_id", &ContextEpochData::feed_id)
        .def_readwrite("raw_from_sequence", &ContextEpochData::raw_from_sequence).def_readwrite("raw_through_sequence", &ContextEpochData::raw_through_sequence)
        .def_readwrite("raw_window_digest", &ContextEpochData::raw_window_digest).def_readwrite("artifact_ids", &ContextEpochData::artifact_ids)
        .def_readwrite("guarantee_profile", &ContextEpochData::guarantee_profile);
    py::class_<ContextEpoch> epoch(m, "ContextEpoch");
    epoch.def_static("create", &ContextEpoch::create).def_static("parse", &ContextEpoch::parse)
        .def_property_readonly("run_id", &ContextEpoch::run_id).def_property_readonly("sequence", &ContextEpoch::sequence)
        .def_property_readonly("predecessor_id", &ContextEpoch::predecessor_id)
        .def_property_readonly("feed_id", &ContextEpoch::feed_id)
        .def_property_readonly("raw_from_sequence", &ContextEpoch::raw_from_sequence)
        .def_property_readonly("raw_through_sequence", &ContextEpoch::raw_through_sequence)
        .def_property_readonly("raw_window_digest", &ContextEpoch::raw_window_digest)
        .def_property_readonly("artifact_ids", &ContextEpoch::artifact_ids)
        .def_property_readonly("guarantee_profile", &ContextEpoch::guarantee_profile);
    bind_identity_methods(epoch);

    py::class_<ContextAssemblyReceiptData>(m, "ContextAssemblyReceiptData")
        .def(py::init<>()).def_readwrite("context_epoch_id", &ContextAssemblyReceiptData::context_epoch_id)
        .def_readwrite("normalized_request_digest", &ContextAssemblyReceiptData::normalized_request_digest)
        .def_readwrite("message_window_digest", &ContextAssemblyReceiptData::message_window_digest)
        .def_readwrite("artifact_ids", &ContextAssemblyReceiptData::artifact_ids)
        .def_readwrite("required_skill_artifact_ids", &ContextAssemblyReceiptData::required_skill_artifact_ids)
        .def_readwrite("raw_from_sequence", &ContextAssemblyReceiptData::raw_from_sequence)
        .def_readwrite("raw_through_sequence", &ContextAssemblyReceiptData::raw_through_sequence)
        .def_readwrite("estimated_input_tokens", &ContextAssemblyReceiptData::estimated_input_tokens)
        .def_readwrite("mandatory_input_tokens", &ContextAssemblyReceiptData::mandatory_input_tokens);
    py::class_<ContextAssemblyReceipt> receipt(m, "ContextAssemblyReceipt");
    receipt.def_static("create", &ContextAssemblyReceipt::create).def_static("parse", &ContextAssemblyReceipt::parse)
        .def_property_readonly("context_epoch_id", &ContextAssemblyReceipt::context_epoch_id)
        .def_property_readonly("normalized_request_digest", &ContextAssemblyReceipt::normalized_request_digest)
        .def_property_readonly("message_window_digest", &ContextAssemblyReceipt::message_window_digest)
        .def_property_readonly("artifact_ids", &ContextAssemblyReceipt::artifact_ids)
        .def_property_readonly("required_skill_artifact_ids", &ContextAssemblyReceipt::required_skill_artifact_ids)
        .def_property_readonly("raw_from_sequence", &ContextAssemblyReceipt::raw_from_sequence)
        .def_property_readonly("raw_through_sequence", &ContextAssemblyReceipt::raw_through_sequence)
        .def_property_readonly("estimated_input_tokens", &ContextAssemblyReceipt::estimated_input_tokens)
        .def_property_readonly("mandatory_input_tokens", &ContextAssemblyReceipt::mandatory_input_tokens);
    bind_identity_methods(receipt);

    py::class_<ContextStoreFeed>(m, "ContextStoreFeed").def(py::init<>())
        .def(py::init<std::string, std::string>(), py::arg("owner_id"), py::arg("feed_id"))
        .def_readwrite("owner_id", &ContextStoreFeed::owner_id).def_readwrite("feed_id", &ContextStoreFeed::feed_id);
    py::class_<ContextStoreHead>(m, "ContextStoreHead").def(py::init<>())
        .def_readwrite("sequence", &ContextStoreHead::sequence).def_readwrite("record_id", &ContextStoreHead::record_id);
    py::class_<ContextHistoryRange>(m, "ContextHistoryRange").def(py::init<>())
        .def_readwrite("owner_id", &ContextHistoryRange::owner_id).def_readwrite("feed_id", &ContextHistoryRange::feed_id)
        .def_readwrite("from_sequence", &ContextHistoryRange::from_sequence).def_readwrite("through_sequence", &ContextHistoryRange::through_sequence)
        .def_readwrite("digest", &ContextHistoryRange::digest).def_readwrite("artifact_uri", &ContextHistoryRange::artifact_uri);
    py::class_<ContextStore, std::shared_ptr<ContextStore>>(m, "ContextStore")
        .def("append_history", &ContextStore::append_history)
        .def("history_head", &ContextStore::history_head)
        .def("snapshot_history", &ContextStore::snapshot_history)
        .def("hydrate_history", &ContextStore::hydrate_history)
        .def("put_artifact", &ContextStore::put_artifact)
        .def("get_artifact", &ContextStore::get_artifact);
    py::class_<DurableContextStore, ContextStore,
               std::shared_ptr<DurableContextStore>>(m, "DurableContextStore");
    py::class_<InMemoryContextStore, ContextStore, std::shared_ptr<InMemoryContextStore>>(m, "InMemoryContextStore")
        .def(py::init<>()).def("append_history", &InMemoryContextStore::append_history)
        .def("history_head", &InMemoryContextStore::history_head).def("snapshot_history", &InMemoryContextStore::snapshot_history)
        .def("hydrate_history", &InMemoryContextStore::hydrate_history).def("put_artifact", &InMemoryContextStore::put_artifact)
        .def("get_artifact", &InMemoryContextStore::get_artifact);

#ifdef NEOGRAPH_PYBIND_HAS_SQLITE
    py::class_<SQLiteContextStore, DurableContextStore,
               std::shared_ptr<SQLiteContextStore>>(m, "SQLiteContextStore")
        .def(py::init<std::string>(), py::arg("database_path"));
#endif

    py::class_<RuntimeContextRequirements>(m, "RuntimeContextRequirements")
        .def(py::init<>())
        .def_readwrite("required_artifact_ids",
                       &RuntimeContextRequirements::required_artifact_ids)
        .def_readwrite("required_skill_artifact_ids",
                       &RuntimeContextRequirements::required_skill_artifact_ids);

    py::class_<ContextTransformReceiptData>(m, "ContextTransformReceiptData")
        .def(py::init<>())
        .def_readwrite("source_context_epoch_id",
                       &ContextTransformReceiptData::source_context_epoch_id)
        .def_readwrite("transformer_identity",
                       &ContextTransformReceiptData::transformer_identity)
        .def_readwrite("input_artifact_ids",
                       &ContextTransformReceiptData::input_artifact_ids)
        .def_readwrite("output_artifact_ids",
                       &ContextTransformReceiptData::output_artifact_ids)
        .def_readwrite("preserved_required_artifact_ids",
                       &ContextTransformReceiptData::preserved_required_artifact_ids);
    py::class_<ContextTransformReceipt> transform(m, "ContextTransformReceipt");
    transform
        .def_static("create", &ContextTransformReceipt::create)
        .def_static("parse", &ContextTransformReceipt::parse)
        .def_property_readonly("source_context_epoch_id",
                               &ContextTransformReceipt::source_context_epoch_id)
        .def_property_readonly("transformer_identity",
                               &ContextTransformReceipt::transformer_identity)
        .def_property_readonly("input_artifact_ids",
                               &ContextTransformReceipt::input_artifact_ids)
        .def_property_readonly("output_artifact_ids",
                               &ContextTransformReceipt::output_artifact_ids)
        .def_property_readonly("preserved_required_artifact_ids",
                               &ContextTransformReceipt::preserved_required_artifact_ids);
    bind_identity_methods(transform);
    m.def("validate_context_transform_receipt",
          &validate_context_transform_receipt);

    py::register_exception<ContextBudgetBlocked>(m, "ContextBudgetBlocked");
    py::class_<RuntimeTurnAssembler>(m, "RuntimeTurnAssembler")
        .def(py::init([](std::shared_ptr<ContextStore> store, std::vector<std::string> skills, std::uint64_t budget) {
            return std::make_unique<RuntimeTurnAssembler>(*store, std::move(skills), budget);
        }), py::arg("store"), py::arg("static_required_skill_artifact_ids") = std::vector<std::string>{}, py::arg("max_input_tokens") = 0,
            py::keep_alive<1, 2>())
        .def(py::init([](std::shared_ptr<ContextStore> store,
                         std::uint64_t budget,
                         RuntimeContextRequirements requirements) {
            return std::make_unique<RuntimeTurnAssembler>(
                *store, budget, std::move(requirements));
        }), py::arg("store"), py::arg("max_input_tokens"), py::arg("requirements"),
            py::keep_alive<1, 2>())
        .def("assemble", [](const RuntimeTurnAssembler& self, std::string owner, const ContextEpoch& active_epoch,
                              CompletionParams params, bool stream) {
            auto turn = self.assemble(std::move(owner), active_epoch, request_from(std::move(params), stream, {}));
            return py::make_tuple(turn.request.params(), turn.assembly_receipt);
        }, py::arg("owner_id"), py::arg("epoch"), py::arg("params"), py::arg("stream") = false)
        .def_static("estimate_input_tokens", &RuntimeTurnAssembler::estimate_input_tokens);

    py::class_<ProviderDispatchReceiptData>(m, "ProviderDispatchReceiptData").def(py::init<>())
        .def_readwrite("dispatch_id", &ProviderDispatchReceiptData::dispatch_id).def_readwrite("provider_binding_identity", &ProviderDispatchReceiptData::provider_binding_identity)
        .def_readwrite("assembly_receipt_id", &ProviderDispatchReceiptData::assembly_receipt_id).def_readwrite("normalized_request_digest", &ProviderDispatchReceiptData::normalized_request_digest)
        .def_readwrite("model", &ProviderDispatchReceiptData::model);
    py::class_<ProviderDispatchReceipt> dispatch_receipt(m, "ProviderDispatchReceipt");
    dispatch_receipt.def_static("create", &ProviderDispatchReceipt::create).def_static("parse", &ProviderDispatchReceipt::parse)
        .def_property_readonly("dispatch_id", &ProviderDispatchReceipt::dispatch_id);
    bind_identity_methods(dispatch_receipt);
    py::class_<ProviderDispatchOutcomeReceiptData>(m, "ProviderDispatchOutcomeReceiptData")
        .def(py::init<>())
        .def_readwrite("dispatch_id", &ProviderDispatchOutcomeReceiptData::dispatch_id)
        .def_readwrite("dispatch_receipt_id", &ProviderDispatchOutcomeReceiptData::dispatch_receipt_id)
        .def_readwrite("state", &ProviderDispatchOutcomeReceiptData::state)
        .def_readwrite("response_digest", &ProviderDispatchOutcomeReceiptData::response_digest)
        .def_readwrite("error", &ProviderDispatchOutcomeReceiptData::error);
    py::class_<ProviderDispatchOutcomeReceipt> outcome_receipt(
        m, "ProviderDispatchOutcomeReceipt");
    outcome_receipt
        .def_static("create", &ProviderDispatchOutcomeReceipt::create)
        .def_static("parse", &ProviderDispatchOutcomeReceipt::parse)
        .def_property_readonly("dispatch_id", &ProviderDispatchOutcomeReceipt::dispatch_id)
        .def_property_readonly("dispatch_receipt_id", &ProviderDispatchOutcomeReceipt::dispatch_receipt_id)
        .def_property_readonly("state", &ProviderDispatchOutcomeReceipt::state)
        .def_property_readonly("response_digest", &ProviderDispatchOutcomeReceipt::response_digest)
        .def_property_readonly("error", &ProviderDispatchOutcomeReceipt::error);
    bind_identity_methods(outcome_receipt);
    py::class_<ProviderDispatchReceiptStore, std::shared_ptr<ProviderDispatchReceiptStore>>(m, "ProviderDispatchReceiptStore")
        .def("persist", py::overload_cast<const ProviderDispatchReceipt&>(&ProviderDispatchReceiptStore::persist))
        .def("state", py::overload_cast<std::string_view>(&ProviderDispatchReceiptStore::state, py::const_));
    py::class_<DurableProviderDispatchReceiptStore, ProviderDispatchReceiptStore, PyDurableProviderDispatchReceiptStore,
               std::shared_ptr<DurableProviderDispatchReceiptStore>>(m, "DurableProviderDispatchReceiptStore").def(py::init<>());
    py::class_<InMemoryProviderDispatchReceiptStore, ProviderDispatchReceiptStore,
               std::shared_ptr<InMemoryProviderDispatchReceiptStore>>(m, "InMemoryProviderDispatchReceiptStore")
        .def(py::init<>())
        .def("settle", &InMemoryProviderDispatchReceiptStore::settle)
        .def("outcome", &InMemoryProviderDispatchReceiptStore::outcome);
#ifdef NEOGRAPH_PYBIND_HAS_SQLITE
    py::class_<SQLiteProviderDispatchReceiptStore,
               DurableProviderDispatchReceiptStore,
               std::shared_ptr<SQLiteProviderDispatchReceiptStore>>(
        m, "SQLiteProviderDispatchReceiptStore")
        .def(py::init<std::string>(), py::arg("database_path"))
        .def("settle", &SQLiteProviderDispatchReceiptStore::settle)
        .def("outcome", &SQLiteProviderDispatchReceiptStore::outcome);
#endif

    py::class_<ControlledProvider>(m, "ControlledProvider")
        .def(py::init<std::shared_ptr<Provider>, std::shared_ptr<ProviderDispatchReceiptStore>, std::string>(),
             py::arg("provider"), py::arg("receipts"), py::arg("provider_binding_identity"), py::keep_alive<1, 2>(), py::keep_alive<1, 3>())
        .def("dispatch", [](ControlledProvider& self, std::string dispatch_id, const ContextAssemblyReceipt& assembly,
                             CompletionParams params, bool stream, StreamCallback on_chunk) {
            return self.dispatch(std::move(dispatch_id), assembly, request_from(std::move(params), stream, std::move(on_chunk)));
        }, py::arg("dispatch_id"), py::arg("assembly"), py::arg("params"), py::arg("stream") = false, py::arg("on_chunk") = StreamCallback{});
    py::class_<RuntimeInterpositionController>(m, "RuntimeInterpositionController")
        .def(py::init<std::shared_ptr<Provider>, std::shared_ptr<ContextStore>, std::shared_ptr<ProviderDispatchReceiptStore>, std::string, std::uint64_t, std::vector<std::string>>(),
              py::arg("provider"), py::arg("context_store"), py::arg("dispatch_store"), py::arg("provider_binding_identity"), py::arg("max_input_tokens") = 0, py::arg("static_required_skill_artifact_ids") = std::vector<std::string>{},
             py::keep_alive<1, 2>(), py::keep_alive<1, 3>(), py::keep_alive<1, 4>())
        .def("activate", &RuntimeInterpositionController::activate).def("clear", &RuntimeInterpositionController::clear)
        .def_property_readonly("active", &RuntimeInterpositionController::active)
        .def("set_hook_runtime", &RuntimeInterpositionController::set_hook_runtime,
             py::arg("runtime"), py::keep_alive<1, 2>())
        .def("invoke", [](RuntimeInterpositionController& self,
                           CompletionParams params,
                           py::object on_chunk) {
            StreamCallback callback;
            std::shared_ptr<py::function> held;
            if (!on_chunk.is_none()) {
                held = std::shared_ptr<py::function>(
                    new py::function(on_chunk.cast<py::function>()),
                    [](py::function* value) {
                        py::gil_scoped_acquire acquire;
                        delete value;
                    });
                callback = [held](const std::string& chunk) {
                    py::gil_scoped_acquire acquire;
                    (*held)(chunk);
                };
            }
            py::gil_scoped_release release;
            return self.invoke(std::move(params), std::move(callback));
        }, py::arg("params"), py::arg("on_chunk") = py::none());

    py::class_<StrictRuntimeProfile, std::shared_ptr<StrictRuntimeProfile>>(
        m, "StrictRuntimeProfile")
        .def(py::init([](std::shared_ptr<Provider> provider,
                         std::shared_ptr<DurableContextStore> context_store,
                         std::shared_ptr<DurableProviderDispatchReceiptStore> dispatch_store,
                         std::shared_ptr<HookRuntime> hook_runtime,
                         std::string provider_binding_identity,
                         std::uint64_t max_input_tokens,
                         std::vector<std::string> required_context_artifact_ids,
                         std::vector<std::string> required_skill_artifact_ids) {
            return std::make_shared<StrictRuntimeProfile>(StrictRuntimeProfileConfig{
                std::move(provider), std::move(context_store), std::move(dispatch_store),
                std::move(hook_runtime), std::move(provider_binding_identity),
                max_input_tokens, std::move(required_context_artifact_ids),
                std::move(required_skill_artifact_ids)});
        }),
        py::arg("provider"), py::arg("context_store"), py::arg("dispatch_store"),
        py::arg("hook_runtime"), py::arg("provider_binding_identity"),
        py::arg("max_input_tokens"),
        py::arg("required_context_artifact_ids") = std::vector<std::string>{},
        py::arg("required_skill_artifact_ids") = std::vector<std::string>{},
        py::keep_alive<1, 2>(), py::keep_alive<1, 3>(),
        py::keep_alive<1, 4>(), py::keep_alive<1, 5>())
        .def("activate", &StrictRuntimeProfile::activate)
        .def("clear", &StrictRuntimeProfile::clear)
        .def_property_readonly("active", &StrictRuntimeProfile::active)
        .def("attach", &StrictRuntimeProfile::attach, py::arg("engine"),
             py::keep_alive<2, 1>())
        .def("invoke", [](const StrictRuntimeProfile& self,
                           CompletionParams params) {
            auto controller = self.interposition();
            py::gil_scoped_release release;
            return controller->invoke(std::move(params));
        }, py::arg("params"),
             "Invoke through this profile's strict context, Hook, and receipt boundary.")
        .def_property_readonly("interposition", &StrictRuntimeProfile::interposition)
        .def_property_readonly("hooks", &StrictRuntimeProfile::hooks)
        .def_property_readonly("context_store", &StrictRuntimeProfile::context_store)
        .def_property_readonly("dispatch_store", &StrictRuntimeProfile::dispatch_store);
}

}  // namespace neograph::pybind
