#include "json_bridge.h"

#include <neograph/context_store.h>
#include <neograph/controlled_provider.h>
#include <neograph/runtime_interposition_controller.h>
#include <neograph/runtime_turn_assembler.h>

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
        .value("HookOutput", ContextArtifactKind::HookOutput);
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
        .def_property_readonly("feed_id", &ContextEpoch::feed_id).def_property_readonly("raw_window_digest", &ContextEpoch::raw_window_digest)
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
        .def_property_readonly("normalized_request_digest", &ContextAssemblyReceipt::normalized_request_digest);
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
    py::class_<InMemoryContextStore, ContextStore, std::shared_ptr<InMemoryContextStore>>(m, "InMemoryContextStore")
        .def(py::init<>()).def("append_history", &InMemoryContextStore::append_history)
        .def("history_head", &InMemoryContextStore::history_head).def("snapshot_history", &InMemoryContextStore::snapshot_history)
        .def("hydrate_history", &InMemoryContextStore::hydrate_history).def("put_artifact", &InMemoryContextStore::put_artifact)
        .def("get_artifact", &InMemoryContextStore::get_artifact);

    py::register_exception<ContextBudgetBlocked>(m, "ContextBudgetBlocked");
    py::class_<RuntimeTurnAssembler>(m, "RuntimeTurnAssembler")
        .def(py::init([](std::shared_ptr<ContextStore> store, std::vector<std::string> skills, std::uint64_t budget) {
            return std::make_unique<RuntimeTurnAssembler>(*store, std::move(skills), budget);
        }), py::arg("store"), py::arg("static_required_skill_artifact_ids") = std::vector<std::string>{}, py::arg("max_input_tokens") = 0,
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
    py::class_<ProviderDispatchReceiptStore, std::shared_ptr<ProviderDispatchReceiptStore>>(m, "ProviderDispatchReceiptStore")
        .def("persist", &ProviderDispatchReceiptStore::persist)
        .def("state", &ProviderDispatchReceiptStore::state);
    py::class_<DurableProviderDispatchReceiptStore, ProviderDispatchReceiptStore, PyDurableProviderDispatchReceiptStore,
               std::shared_ptr<DurableProviderDispatchReceiptStore>>(m, "DurableProviderDispatchReceiptStore").def(py::init<>());
    py::class_<InMemoryProviderDispatchReceiptStore, ProviderDispatchReceiptStore,
               std::shared_ptr<InMemoryProviderDispatchReceiptStore>>(m, "InMemoryProviderDispatchReceiptStore").def(py::init<>());

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
        .def("invoke", &RuntimeInterpositionController::invoke, py::arg("params"), py::arg("on_chunk") = StreamCallback{});
}

}  // namespace neograph::pybind
