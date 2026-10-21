#include "json_bridge.h"
#include "provider_bridge.h"

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


template <typename T>
void bind_identity_methods(py::class_<T>& cls) {
    cls.def_property_readonly("id", &T::id)
       .def("serialize_canonical", [](const T& value) {
           return value.serialize_canonical();
       });
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
        .value("HardConstraint", ContextArtifactKind::HardConstraint)
        .value("UntrustedSupplemental", ContextArtifactKind::UntrustedSupplemental);
    py::enum_<ContextPlacement>(m, "ContextPlacement")
        .value("BeforeLatestUser", ContextPlacement::BeforeLatestUser)
        .value("AfterLatestUser", ContextPlacement::AfterLatestUser)
        .value("AfterHistory", ContextPlacement::AfterHistory)
        .value("BeforeHistory", ContextPlacement::BeforeHistory);
    py::enum_<RuntimeGuaranteeProfile>(m, "RuntimeGuaranteeProfile")
        .value("Legacy", RuntimeGuaranteeProfile::Legacy)
        .value("Recorded", RuntimeGuaranteeProfile::Recorded)
        .value("Strict", RuntimeGuaranteeProfile::Strict);
    py::enum_<StabilityClass>(m, "StabilityClass")
        .value("Stable", StabilityClass::Stable)
        .value("Slow", StabilityClass::Slow)
        .value("Volatile", StabilityClass::Volatile);
    py::enum_<AnchorMode>(m, "AnchorMode")
        .value("Replace", AnchorMode::Replace)
        .value("KeepInPlace", AnchorMode::KeepInPlace);
    py::enum_<ContextBreakReason>(m, "ContextBreakReason")
        .value("Compaction", ContextBreakReason::Compaction)
        .value("StableChange", ContextBreakReason::StableChange)
        .value("LayoutChange", ContextBreakReason::LayoutChange)
        .value("AnchorDrop", ContextBreakReason::AnchorDrop)
        .value("OrderChange", ContextBreakReason::OrderChange)
        .value("Explicit", ContextBreakReason::Explicit);
    py::enum_<ContextPrefixClaim>(m, "ContextPrefixClaim")
        .value("Extends", ContextPrefixClaim::Extends)
        .value("Breaks", ContextPrefixClaim::Breaks);
    py::enum_<AssemblyLayout>(m, "AssemblyLayout")
        .value("Legacy", AssemblyLayout::Legacy)
        .value("StableFirst", AssemblyLayout::StableFirst);
    py::enum_<ClassSource>(m, "ClassSource")
        .value("Declared", ClassSource::Declared)
        .value("Derived", ClassSource::Derived);
    m.attr("STABILITY_DERIVATION_RULE_VERSION") = STABILITY_DERIVATION_RULE_VERSION;
    py::enum_<ContextStabilityRefusal>(m, "ContextStabilityRefusal")
        .value("VolatileSystemArtifact", ContextStabilityRefusal::VolatileSystemArtifact)
        .value("PredecessorUnavailable", ContextStabilityRefusal::PredecessorUnavailable)
        .value("PredecessorOrderUnknown", ContextStabilityRefusal::PredecessorOrderUnknown)
        .value("ArtifactsMismatch", ContextStabilityRefusal::ArtifactsMismatch)
        .value("RawWindowMoved", ContextStabilityRefusal::RawWindowMoved)
        .value("RawWindowNotGrown", ContextStabilityRefusal::RawWindowNotGrown)
        .value("StablePrefixChanged", ContextStabilityRefusal::StablePrefixChanged)
        .value("AnchorDropped", ContextStabilityRefusal::AnchorDropped)
        .value("AnchorOutOfRange", ContextStabilityRefusal::AnchorOutOfRange)
        .value("LayoutChangeUndeclared", ContextStabilityRefusal::LayoutChangeUndeclared)
        .value("StableOutsideHistoryHead", ContextStabilityRefusal::StableOutsideHistoryHead)
        .value("VolatileOutsideTail", ContextStabilityRefusal::VolatileOutsideTail)
        .value("TierOrderViolated", ContextStabilityRefusal::TierOrderViolated)
        .value("VolatileSystemText", ContextStabilityRefusal::VolatileSystemText)
        .value("PrefixBoundaryUnavailable", ContextStabilityRefusal::PrefixBoundaryUnavailable)
        .value("AmbiguousOrder", ContextStabilityRefusal::AmbiguousOrder);
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
    history.def_static("create", &RuntimeHistoryRecord::create)
        .def_static("parse", &RuntimeHistoryRecord::parse,
                    py::arg("stored_bytes"), py::arg("archive") = nullptr,
                    py::arg("owner_id") = "",
                    py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("feed_id", &RuntimeHistoryRecord::feed_id).def_property_readonly("sequence", &RuntimeHistoryRecord::sequence)
        .def_property_readonly("message_id", &RuntimeHistoryRecord::message_id).def_property_readonly("trust", &RuntimeHistoryRecord::trust)
        .def_property_readonly("message", [](const RuntimeHistoryRecord& value) { return value.message(); })
        .def_property_readonly("predecessor_id", &RuntimeHistoryRecord::predecessor_id);
    bind_identity_methods(history);
    history.def("serialize_canonical",
                [](const RuntimeHistoryRecord& value,
                   const std::shared_ptr<sp::NativeArchive>& archive,
                   std::string_view owner_id) {
                    return value.serialize_canonical(archive, owner_id);
                },
                py::arg("archive"), py::arg("owner_id"),
                py::call_guard<py::gil_scoped_release>());

    py::class_<ContextArtifactData>(m, "ContextArtifactData")
        .def(py::init<>()).def_readwrite("kind", &ContextArtifactData::kind).def_readwrite("producer_id", &ContextArtifactData::producer_id)
        .def_readwrite("source_digest", &ContextArtifactData::source_digest).def_readwrite("source_feed_id", &ContextArtifactData::source_feed_id)
        .def_readwrite("covers_from_sequence", &ContextArtifactData::covers_from_sequence).def_readwrite("covers_through_sequence", &ContextArtifactData::covers_through_sequence)
        .def_readwrite("media_type", &ContextArtifactData::media_type).def_readwrite("placement", &ContextArtifactData::placement)
        .def_readwrite("priority", &ContextArtifactData::priority).def_readwrite("required", &ContextArtifactData::required)
        .def_readwrite("stability_class", &ContextArtifactData::stability_class)
        .def_readwrite("anchor_mode", &ContextArtifactData::anchor_mode)
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
        .def_property_readonly("stability_class", &ContextArtifact::stability_class)
        .def_property_readonly("declared_stability_class", &ContextArtifact::declared_stability_class)
        .def_property_readonly("anchor_mode", &ContextArtifact::anchor_mode)
        .def_property_readonly("declared_anchor_mode", &ContextArtifact::declared_anchor_mode)
        .def_property_readonly("content", [](const ContextArtifact& v) { return json_to_py(v.content()); });
    bind_identity_methods(artifact);

    py::class_<ContextEpochData>(m, "ContextEpochData")
        .def(py::init<>()).def_readwrite("run_id", &ContextEpochData::run_id).def_readwrite("sequence", &ContextEpochData::sequence)
        .def_readwrite("predecessor_id", &ContextEpochData::predecessor_id).def_readwrite("feed_id", &ContextEpochData::feed_id)
        .def_readwrite("raw_from_sequence", &ContextEpochData::raw_from_sequence).def_readwrite("raw_through_sequence", &ContextEpochData::raw_through_sequence)
        .def_readwrite("raw_window_digest", &ContextEpochData::raw_window_digest).def_readwrite("artifact_ids", &ContextEpochData::artifact_ids)
        .def_readwrite("guarantee_profile", &ContextEpochData::guarantee_profile)
        .def_readwrite("ordered_artifact_ids", &ContextEpochData::ordered_artifact_ids)
        .def_readwrite("prefix_claim", &ContextEpochData::prefix_claim)
        .def_readwrite("break_reason", &ContextEpochData::break_reason)
        .def_readwrite("assembly_layout", &ContextEpochData::assembly_layout);
    py::class_<ContextEpoch> epoch(m, "ContextEpoch");
    epoch.def_static("create", &ContextEpoch::create).def_static("parse", &ContextEpoch::parse)
        .def_property_readonly("run_id", &ContextEpoch::run_id).def_property_readonly("sequence", &ContextEpoch::sequence)
        .def_property_readonly("predecessor_id", &ContextEpoch::predecessor_id)
        .def_property_readonly("feed_id", &ContextEpoch::feed_id)
        .def_property_readonly("raw_from_sequence", &ContextEpoch::raw_from_sequence)
        .def_property_readonly("raw_through_sequence", &ContextEpoch::raw_through_sequence)
        .def_property_readonly("raw_window_digest", &ContextEpoch::raw_window_digest)
        .def_property_readonly("artifact_ids", &ContextEpoch::artifact_ids)
        .def_property_readonly("guarantee_profile", &ContextEpoch::guarantee_profile)
        .def_property_readonly("ordered_artifact_ids", &ContextEpoch::ordered_artifact_ids)
        .def_property_readonly("prefix_claim", &ContextEpoch::prefix_claim)
        .def_property_readonly("break_reason", &ContextEpoch::break_reason)
        .def_property_readonly("assembly_layout", &ContextEpoch::assembly_layout);
    bind_identity_methods(epoch);

    py::class_<ContextPrefixBoundary>(m, "ContextPrefixBoundary")
        .def(py::init<>())
        .def(py::init([](std::uint64_t message_index, std::uint64_t part_index) {
            return ContextPrefixBoundary{message_index, part_index};
        }), py::arg("message_index"), py::arg("part_index"))
        .def_readwrite("message_index", &ContextPrefixBoundary::message_index)
        .def_readwrite("part_index", &ContextPrefixBoundary::part_index)
        .def("__eq__", [](const ContextPrefixBoundary& a, const ContextPrefixBoundary& b) { return a == b; });

    py::class_<ContextAssemblyReceiptData>(m, "ContextAssemblyReceiptData")
        .def(py::init<>()).def_readwrite("context_epoch_id", &ContextAssemblyReceiptData::context_epoch_id)
        .def_readwrite("normalized_request_digest", &ContextAssemblyReceiptData::normalized_request_digest)
        .def_readwrite("message_window_digest", &ContextAssemblyReceiptData::message_window_digest)
        .def_readwrite("artifact_ids", &ContextAssemblyReceiptData::artifact_ids)
        .def_readwrite("required_skill_artifact_ids", &ContextAssemblyReceiptData::required_skill_artifact_ids)
        .def_readwrite("raw_from_sequence", &ContextAssemblyReceiptData::raw_from_sequence)
        .def_readwrite("raw_through_sequence", &ContextAssemblyReceiptData::raw_through_sequence)
        .def_readwrite("estimated_input_tokens", &ContextAssemblyReceiptData::estimated_input_tokens)
        .def_readwrite("mandatory_input_tokens", &ContextAssemblyReceiptData::mandatory_input_tokens)
        .def_readwrite("assembled_artifact_ids", &ContextAssemblyReceiptData::assembled_artifact_ids)
        .def_readwrite("artifact_classes", &ContextAssemblyReceiptData::artifact_classes)
        .def_readwrite("class_sources", &ContextAssemblyReceiptData::class_sources)
        .def_readwrite("class_rule_version", &ContextAssemblyReceiptData::class_rule_version)
        .def_readwrite("encoder_family", &ContextAssemblyReceiptData::encoder_family)
        .def_readwrite("stable_boundary", &ContextAssemblyReceiptData::stable_boundary)
        .def_readwrite("prefix_boundary", &ContextAssemblyReceiptData::prefix_boundary)
        .def_readwrite("prefix_digest", &ContextAssemblyReceiptData::prefix_digest)
        .def_readwrite("tools_digest", &ContextAssemblyReceiptData::tools_digest)
        .def_readwrite("break_reason", &ContextAssemblyReceiptData::break_reason);
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
        .def_property_readonly("mandatory_input_tokens", &ContextAssemblyReceipt::mandatory_input_tokens)
        .def_property_readonly("assembled_artifact_ids", &ContextAssemblyReceipt::assembled_artifact_ids)
        .def_property_readonly("artifact_classes", &ContextAssemblyReceipt::artifact_classes)
        .def_property_readonly("class_sources", &ContextAssemblyReceipt::class_sources)
        .def_property_readonly("class_rule_version", &ContextAssemblyReceipt::class_rule_version)
        .def_property_readonly("encoder_family", &ContextAssemblyReceipt::encoder_family)
        .def_property_readonly("stable_boundary", &ContextAssemblyReceipt::stable_boundary)
        .def_property_readonly("prefix_boundary", &ContextAssemblyReceipt::prefix_boundary)
        .def_property_readonly("prefix_digest", &ContextAssemblyReceipt::prefix_digest)
        .def_property_readonly("tools_digest", &ContextAssemblyReceipt::tools_digest)
        .def_property_readonly("break_reason", &ContextAssemblyReceipt::break_reason);
    bind_identity_methods(receipt);

    m.attr("COMPACTION_PRODUCER_ID") = std::string(COMPACTION_PRODUCER_ID);
    m.def("derive_stability_class", &derive_stability_class,
          py::arg("kind"), py::arg("producer_id"),
          "The class of an artifact that declares none: Stable for a required skill or hard "
          "constraint, Slow for a compaction summary, Volatile for everything else.");
    // Registered after pybind11's plain translators, so it is tried first and the Python
    // exception keeps the typed refusal as `code`.
    py::register_exception<ContextStabilityError>(m, "ContextStabilityError", PyExc_ValueError);
    py::register_exception_translator([](std::exception_ptr pointer) {
        if (!pointer) return;
        try {
            std::rethrow_exception(pointer);
        } catch (const ContextStabilityError& error) {
            try {
                py::object type = py::module_::import("neograph_engine._neograph")
                                      .attr("ContextStabilityError");
                py::object instance = type(py::str(error.what()));
                instance.attr("code") = py::cast(error.code());
                PyErr_SetObject(type.ptr(), instance.ptr());
            } catch (py::error_already_set& failure) {
                failure.restore();
            }
        }
    });
    m.def("order_context_artifacts",
          [](const std::vector<std::pair<ContextArtifact, std::string>>& entries) {
              std::vector<ContextArtifactOrderEntry> converted;
              converted.reserve(entries.size());
              for (const auto& [artifact, key] : entries) converted.push_back({artifact, key});
              return order_context_artifacts(converted);
          },
          py::arg("entries"),
          "The identities of (artifact, order_key) pairs in the total order of the stable-first layout: "
          "stability class, priority (higher first), producer, order_key. A full tie raises "
          "ContextStabilityError.");
    m.def("validate_context_layout", &validate_context_layout, py::arg("epoch"), py::arg("artifacts"),
          "Check that the artifacts of a StableFirst epoch are placed and ordered for that layout.");
    m.def("verify_context_epoch_claim",
          [](const ContextEpoch& epoch, const std::vector<ContextArtifact>& artifacts,
             const std::optional<ContextEpoch>& predecessor,
             const std::vector<ContextArtifact>& predecessor_artifacts) {
              verify_context_epoch_claim(epoch, artifacts, predecessor ? &*predecessor : nullptr,
                                         predecessor_artifacts);
          },
          py::arg("epoch"), py::arg("artifacts"), py::arg("predecessor") = py::none(),
          py::arg("predecessor_artifacts") = std::vector<ContextArtifact>{},
          "Check an epoch's claim to extend its predecessor's request prefix; raises "
          "ContextStabilityError when it does not hold.");

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
        .def("hydrate_records", &ContextStore::hydrate_records,
             py::arg("range"), py::call_guard<py::gil_scoped_release>())
        .def("history_record_by_message_id", &ContextStore::history_record_by_message_id,
             py::arg("feed"), py::arg("message_id"), py::call_guard<py::gil_scoped_release>())
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
        .def(py::init<std::string, std::shared_ptr<sp::NativeArchive>>(),
             py::arg("database_path"), py::arg("archive") = nullptr,
             py::call_guard<py::gil_scoped_release>());
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
        .def("assemble", [](const RuntimeTurnAssembler& self, Provider& provider, std::string owner,
                             const ContextEpoch& epoch, ProviderRequest request) {
            auto turn = self.assemble(provider, std::move(owner), epoch, std::move(request));
            return py::make_tuple(std::make_shared<PreparedHandle>(std::move(turn.request)), turn.assembly_receipt);
        }, py::arg("provider"), py::arg("owner_id"), py::arg("epoch"), py::arg("request"))
        .def_static("estimate_input_tokens", [](const PreparedHandle& request) {
            return RuntimeTurnAssembler::estimate_input_tokens(request.get());
        })
        .def_static("normalized_request_digest", [](const PreparedHandle& request) {
            return RuntimeTurnAssembler::normalized_request_digest(request.get());
        });

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
        .def("dispatch", [](ControlledProvider& self, std::string dispatch_id,
                            const ContextAssemblyReceipt& assembly, ProviderRequest request) {
            py::gil_scoped_release release;
            return self.dispatch(std::move(dispatch_id), assembly, std::move(request));
        }, py::arg("dispatch_id"), py::arg("assembly"), py::arg("request"))
        .def("dispatch_prepared", [](ControlledProvider& self, std::string dispatch_id,
                                     const ContextAssemblyReceipt& assembly, PreparedHandle& prepared) {
            auto request = prepared.take();
            py::gil_scoped_release release;
            return self.dispatch_prepared(std::move(dispatch_id), assembly, std::move(request));
        }, py::arg("dispatch_id"), py::arg("assembly"), py::arg("prepared"));
    py::class_<RuntimeInterpositionController>(m, "RuntimeInterpositionController")
        .def(py::init<std::shared_ptr<Provider>, std::shared_ptr<ContextStore>, std::shared_ptr<ProviderDispatchReceiptStore>, std::string, std::uint64_t, std::vector<std::string>>(),
              py::arg("provider"), py::arg("context_store"), py::arg("dispatch_store"), py::arg("provider_binding_identity"), py::arg("max_input_tokens") = 0, py::arg("static_required_skill_artifact_ids") = std::vector<std::string>{},
             py::keep_alive<1, 2>(), py::keep_alive<1, 3>(), py::keep_alive<1, 4>())
        .def("activate", [](RuntimeInterpositionController& self, std::string owner_id, ContextEpoch epoch,
                            std::optional<ContextEpoch> predecessor) {
            self.activate(std::move(owner_id), std::move(epoch), std::move(predecessor));
        }, py::arg("owner_id"), py::arg("epoch"), py::arg("predecessor") = py::none())
        .def("clear", &RuntimeInterpositionController::clear)
        .def_property_readonly("active", &RuntimeInterpositionController::active)
        .def("set_hook_runtime", &RuntimeInterpositionController::set_hook_runtime,
             py::arg("runtime"), py::keep_alive<1, 2>())
        .def("invoke", [](RuntimeInterpositionController& self, ProviderRequest request) {
            py::gil_scoped_release release;
            return self.invoke(std::move(request));
        }, py::arg("request"));

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
        .def("activate", [](StrictRuntimeProfile& self, std::string owner_id, ContextEpoch epoch,
                            std::optional<ContextEpoch> predecessor) {
            self.activate(std::move(owner_id), std::move(epoch), std::move(predecessor));
        }, py::arg("owner_id"), py::arg("epoch"), py::arg("predecessor") = py::none())
        .def("clear", &StrictRuntimeProfile::clear)
        .def_property_readonly("active", &StrictRuntimeProfile::active)
        .def("attach", &StrictRuntimeProfile::attach, py::arg("engine"),
             py::keep_alive<2, 1>())
        .def("invoke", [](const StrictRuntimeProfile& self, ProviderRequest request) {
            auto controller = self.interposition();
            py::gil_scoped_release release;
            return controller->invoke(std::move(request));
        }, py::arg("request"),
             "Invoke through this profile's strict context, Hook, and receipt boundary.")
        .def_property_readonly("interposition", &StrictRuntimeProfile::interposition)
        .def_property_readonly("hooks", &StrictRuntimeProfile::hooks)
        .def_property_readonly("context_store", &StrictRuntimeProfile::context_store)
        .def_property_readonly("dispatch_store", &StrictRuntimeProfile::dispatch_store);
}

}  // namespace neograph::pybind
