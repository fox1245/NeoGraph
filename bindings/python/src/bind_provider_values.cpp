// Typed SDK values and owned provider observations. JSON fields below are
// data-only copies; native replay remains in the original C++ shared owners.
#include "json_bridge.h"
#include "provider_bridge.h"
#include <core/native.h>
#include <core/native_archive.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <type_traits>
namespace py = pybind11;
namespace neograph::pybind {
namespace {
py::object document_view(const std::shared_ptr<const sp::json::Document>& value) {
    if (!value) return py::none();
    return py::module_::import("json").attr("loads")(value->root().dump());
}
std::shared_ptr<const sp::json::Document> document_from(py::object value) {
    if (value.is_none()) return {};
    auto parsed = sp::json::parse(py_to_json(value).dump());
    auto* document = std::get_if<sp::json::Document>(&parsed);
    if (!document) throw py::value_error("Invalid or oversized SDK JSON data");
    return std::make_shared<const sp::json::Document>(std::move(*document));
}
struct NativeReplayView { std::shared_ptr<const sp::NativeReplay> value; };
struct NativeContextView { std::shared_ptr<const sp::NativeContext> value; };
struct OwnedDelta { sp::LocalId part; sp::PartKind kind; std::string bytes; sp::DeltaChannel channel; };
struct OwnedSeal { sp::LocalId part; std::optional<std::string> snapshot; std::shared_ptr<const sp::json::Document> wire_metadata; };
using OwnedEventValue = std::variant<sp::Begin, sp::MessageBegin, sp::PartBegin,
    OwnedDelta, OwnedSeal, sp::MessageSeal, sp::UsageUpdate, sp::Stop, sp::Commit,
    sp::Fail, sp::RawWire, sp::ResponseEnvelope>;
struct OwnedEvent {
    OwnedEventValue value;
    explicit OwnedEvent(const sp::Event& event) : value(std::visit([](const auto& item) -> OwnedEventValue {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, sp::PartDelta>)
            return OwnedDelta{item.part, item.payload.kind, std::string(item.payload.bytes), item.payload.channel};
        else if constexpr (std::is_same_v<T, sp::PartSeal>)
            return OwnedSeal{item.part, item.snapshot ? std::optional<std::string>{std::string(*item.snapshot)} : std::nullopt, item.wire_metadata};
        else return item;
    }, event)) {}
};
struct PythonObserver {
    std::shared_ptr<py::function> callback;
    void operator()(const sp::Event& event) const {
        py::gil_scoped_acquire gil;
        (*callback)(OwnedEvent(event));
    }
};
}
std::function<void(const sp::Event&)> provider_observer(py::object callback) {
    if (callback.is_none()) return {};
    auto owned = std::shared_ptr<py::function>(new py::function(callback.cast<py::function>()),
        [](py::function* function) { py::gil_scoped_acquire gil; delete function; });
    return PythonObserver{std::move(owned)};
}
py::object provider_observer_function(const std::function<void(const sp::Event&)>& observer) {
    if (!observer) return py::none();
    if (const auto* python = observer.target<PythonObserver>()) return *python->callback;
    return py::cpp_function([observer](const OwnedEvent& owned) {
        auto event = std::visit([](const auto& item) -> sp::Event {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, OwnedDelta>)
                return sp::PartDelta{item.part, {item.kind, item.bytes, item.channel}};
            else if constexpr (std::is_same_v<T, OwnedSeal>)
                return sp::PartSeal{item.part,
                    item.snapshot ? std::optional<std::string_view>{*item.snapshot} : std::nullopt,
                    item.wire_metadata};
            else return item;
        }, owned.value);
        py::gil_scoped_release release;
        observer(event);
    });
}
py::object provider_cause(const std::exception_ptr& cause) {
    if (!cause) return py::none();
    auto retain_python_error = [](py::error_already_set& error) {
        auto value = error.value();
        if (error.trace() && PyException_SetTraceback(value.ptr(), error.trace().ptr()) != 0)
            throw py::error_already_set();
        return value;
    };
    // A stored Python exception may be inspected repeatedly. Do not send it
    // through the dispatcher, which consumes error_already_set's restore state.
    try { std::rethrow_exception(cause); }
    catch (py::error_already_set& error) { return retain_python_error(error); }
    catch (...) {}
    try { py::cpp_function([cause] { std::rethrow_exception(cause); })(); }
    catch (py::error_already_set& error) { return retain_python_error(error); }
    throw std::logic_error("Provider cause did not throw");
}
void init_provider_values(py::module_& m) {
    py::enum_<sp::Role>(m, "ProviderRole")
        .value("System", sp::Role::System)
        .value("Developer", sp::Role::Developer)
        .value("User", sp::Role::User)
        .value("Assistant", sp::Role::Assistant)
        .value("Tool", sp::Role::Tool)
        ;
    py::enum_<sp::ToolCallKind>(m, "ProviderToolCallKind")
        .value("ClientExecuted", sp::ToolCallKind::ClientExecuted)
        .value("ServerExecuted", sp::ToolCallKind::ServerExecuted)
        .value("ApprovalRequest", sp::ToolCallKind::ApprovalRequest)
        ;
    py::enum_<sp::InvalidReason>(m, "InvalidToolCallReason")
        .value("Truncated", sp::InvalidReason::Truncated)
        .value("NotJson", sp::InvalidReason::NotJson)
        .value("DuplicateKey", sp::InvalidReason::DuplicateKey)
        .value("DepthExceeded", sp::InvalidReason::DepthExceeded)
        .value("Empty", sp::InvalidReason::Empty)
        .value("Other", sp::InvalidReason::Other)
        ;
    py::enum_<sp::Evidence>(m, "UsageEvidence")
        .value("Reported", sp::Evidence::Reported)
        .value("Derived", sp::Evidence::Derived)
        ;
    py::enum_<sp::UsageStage>(m, "UsageStage")
        .value("Missing", sp::UsageStage::Missing)
        .value("Partial", sp::UsageStage::Partial)
        .value("Final", sp::UsageStage::Final)
        ;
    py::enum_<sp::UsageQuality>(m, "UsageQuality")
        .value("Consistent", sp::UsageQuality::Consistent)
        .value("Inconsistent", sp::UsageQuality::Inconsistent)
        ;
    py::enum_<sp::StopKind>(m, "ProviderStopKind")
        .value("EndTurn", sp::StopKind::EndTurn)
        .value("ToolUse", sp::StopKind::ToolUse)
        .value("MaxTokens", sp::StopKind::MaxTokens)
        .value("StopSequence", sp::StopKind::StopSequence)
        .value("ContentFilter", sp::StopKind::ContentFilter)
        .value("Refusal", sp::StopKind::Refusal)
        .value("PauseTurn", sp::StopKind::PauseTurn)
        .value("ContextLimit", sp::StopKind::ContextLimit)
        .value("MalformedCall", sp::StopKind::MalformedCall)
        .value("Unknown", sp::StopKind::Unknown)
        ;
    py::enum_<sp::ErrorKind>(m, "ProviderErrorKind")
        .value("InvalidConfig", sp::ErrorKind::InvalidConfig)
        .value("InvalidRequest", sp::ErrorKind::InvalidRequest)
        .value("Unsupported", sp::ErrorKind::Unsupported)
        .value("Transport", sp::ErrorKind::Transport)
        .value("ProtocolCorrupt", sp::ErrorKind::ProtocolCorrupt)
        .value("Truncated", sp::ErrorKind::Truncated)
        .value("RemoteFailure", sp::ErrorKind::RemoteFailure)
        .value("Cancelled", sp::ErrorKind::Cancelled)
        .value("DeadlineExceeded", sp::ErrorKind::DeadlineExceeded)
        .value("ResourceLimit", sp::ErrorKind::ResourceLimit)
        .value("Misuse", sp::ErrorKind::Misuse)
        .value("ReplayIneligible", sp::ErrorKind::ReplayIneligible)
        .value("Authentication", sp::ErrorKind::Authentication)
        .value("Permission", sp::ErrorKind::Permission)
        .value("NotFound", sp::ErrorKind::NotFound)
        .value("RateLimited", sp::ErrorKind::RateLimited)
        .value("QuotaExhausted", sp::ErrorKind::QuotaExhausted)
        .value("LimitUnknown", sp::ErrorKind::LimitUnknown)
        .value("Overloaded", sp::ErrorKind::Overloaded)
        ;
    py::enum_<sp::RetryClass>(m, "ProviderRetryClass")
        .value("Never", sp::RetryClass::Never)
        .value("Transient", sp::RetryClass::Transient)
        .value("AfterReset", sp::RetryClass::AfterReset)
        .value("Unknown", sp::RetryClass::Unknown)
        ;
    py::enum_<sp::RetrySafety>(m, "ProviderRetrySafety")
        .value("NotSent", sp::RetrySafety::NotSent)
        .value("PossiblyAccepted", sp::RetrySafety::PossiblyAccepted)
        .value("RejectedBeforeOutput", sp::RetrySafety::RejectedBeforeOutput)
        .value("OutputObserved", sp::RetrySafety::OutputObserved)
        ;
    py::enum_<sp::ImageDetail>(m, "ProviderImageDetail")
        .value("Auto", sp::ImageDetail::Auto)
        .value("Low", sp::ImageDetail::Low)
        .value("High", sp::ImageDetail::High)
        .value("Original", sp::ImageDetail::Original)
        ;
    py::enum_<sp::PartKind>(m, "ProviderPartKind")
        .value("Text", sp::PartKind::Text)
        .value("Refusal", sp::PartKind::Refusal)
        .value("ToolCall", sp::PartKind::ToolCall)
        .value("Thinking", sp::PartKind::Thinking)
        .value("RedactedThinking", sp::PartKind::RedactedThinking)
        .value("ServerToolResult", sp::PartKind::ServerToolResult)
        .value("Reasoning", sp::PartKind::Reasoning)
        .value("Opaque", sp::PartKind::Opaque)
        .value("Thought", sp::PartKind::Thought)
        ;
    py::enum_<sp::DeltaChannel>(m, "ProviderDeltaChannel")
        .value("Content", sp::DeltaChannel::Content)
        .value("Signature", sp::DeltaChannel::Signature)
        ;
    auto cText = py::class_<sp::Text>(m, "Text").def(py::init<>());
    cText.def_readwrite("value", &sp::Text::value);
    auto cRefusal = py::class_<sp::Refusal>(m, "Refusal").def(py::init<>());
    cRefusal.def_readwrite("text", &sp::Refusal::text);
    cRefusal.def_readwrite("raw_code", &sp::Refusal::raw_code);
    auto cToolCall = py::class_<sp::ToolCall>(m, "ProviderToolCall").def(py::init<>());
    cToolCall.def_readwrite("id", &sp::ToolCall::id);
    cToolCall.def_readwrite("name", &sp::ToolCall::name);
    cToolCall.def_readwrite("kind", &sp::ToolCall::kind);
    cToolCall.def_readwrite("wire_type", &sp::ToolCall::wire_type);
    auto cInvalidToolCall = py::class_<sp::InvalidToolCall>(m, "InvalidToolCall").def(py::init<>());
    cInvalidToolCall.def_readwrite("id", &sp::InvalidToolCall::id);
    cInvalidToolCall.def_readwrite("name", &sp::InvalidToolCall::name);
    cInvalidToolCall.def_readwrite("kind", &sp::InvalidToolCall::kind);
    cInvalidToolCall.def_readwrite("raw_fragment", &sp::InvalidToolCall::raw_fragment);
    cInvalidToolCall.def_readwrite("reason", &sp::InvalidToolCall::reason);
    cInvalidToolCall.def_readwrite("wire_type", &sp::InvalidToolCall::wire_type);
    auto cThinking = py::class_<sp::Thinking>(m, "Thinking").def(py::init<>());
    cThinking.def_readwrite("text", &sp::Thinking::text);
    cThinking.def_readwrite("signature", &sp::Thinking::signature);
    auto cRedactedThinking = py::class_<sp::RedactedThinking>(m, "RedactedThinking").def(py::init<>());
    cRedactedThinking.def_readwrite("data", &sp::RedactedThinking::data);
    auto cServerToolResult = py::class_<sp::ServerToolResult>(m, "ServerToolResult").def(py::init<>());
    cServerToolResult.def_readwrite("tool_use_id", &sp::ServerToolResult::tool_use_id);
    cServerToolResult.def_readwrite("wire_type", &sp::ServerToolResult::wire_type);
    auto cToolResultHostMetadata = py::class_<sp::ToolResultHostMetadata>(m, "ToolResultHostMetadata").def(py::init<>());
    cToolResultHostMetadata.def_readwrite("name", &sp::ToolResultHostMetadata::name);
    cToolResultHostMetadata.def_readwrite("status", &sp::ToolResultHostMetadata::status);
    cToolResultHostMetadata.def_readwrite("retryable", &sp::ToolResultHostMetadata::retryable);
    cToolResultHostMetadata.def_readwrite("effect_uncertain", &sp::ToolResultHostMetadata::effect_uncertain);
    auto cToolResult = py::class_<sp::ToolResult>(m, "ProviderToolResult").def(py::init<>());
    cToolResult.def_readwrite("tool_use_id", &sp::ToolResult::tool_use_id);
    cToolResult.def_readwrite("content", &sp::ToolResult::content);
    cToolResult.def_readwrite("is_error", &sp::ToolResult::is_error);
    cToolResult.def_property("host", [](const sp::ToolResult& v) { return v.host; },
        [](sp::ToolResult& v, std::optional<sp::ToolResultHostMetadata> host) { v.host = std::move(host); });
    auto cReasoning = py::class_<sp::Reasoning>(m, "Reasoning").def(py::init<>());
    cReasoning.def_readwrite("id", &sp::Reasoning::id);
    cReasoning.def_readwrite("summary", &sp::Reasoning::summary);
    cReasoning.def_readwrite("encrypted_content", &sp::Reasoning::encrypted_content);
    cReasoning.def_readwrite("status", &sp::Reasoning::status);
    cReasoning.def_readwrite("content", &sp::Reasoning::content);
    auto cOpaque = py::class_<sp::Opaque>(m, "Opaque").def(py::init<>());
    cOpaque.def_readwrite("wire_type", &sp::Opaque::wire_type);
    auto cThought = py::class_<sp::Thought>(m, "Thought").def(py::init<>());
    cThought.def_readwrite("summary", &sp::Thought::summary);
    cThought.def_readwrite("signature", &sp::Thought::signature);
    cText.def(py::init([](std::string value) { return sp::Text{std::move(value)}; }), py::arg("value"));
    cThinking.def(py::init([](std::string text, std::optional<std::string> signature) { return sp::Thinking{std::move(text), std::move(signature)}; }), py::arg("text"), py::arg("signature") = py::none());
    cToolResult.def(py::init([](std::string id, std::string content, bool error) { return sp::ToolResult{std::move(id), std::move(content), error, {}}; }), py::arg("tool_use_id"), py::arg("content"), py::arg("is_error") = false);
    cToolCall.def_property("input", [](const sp::ToolCall& v) { return document_view(v.input); }, [](sp::ToolCall& v, py::object x) { v.input = document_from(x); });
    cToolCall.def_property("wire_metadata", [](const sp::ToolCall& v) { return document_view(v.wire_metadata); }, [](sp::ToolCall& v, py::object x) { v.wire_metadata = document_from(x); });
    cInvalidToolCall.def_property("wire_metadata", [](const sp::InvalidToolCall& v) { return document_view(v.wire_metadata); }, [](sp::InvalidToolCall& v, py::object x) { v.wire_metadata = document_from(x); });
    cServerToolResult.def_property("content", [](const sp::ServerToolResult& v) { return document_view(v.content); }, [](sp::ServerToolResult& v, py::object x) { v.content = document_from(x); });
    cOpaque.def_property("wire_metadata", [](const sp::Opaque& v) { return document_view(v.wire_metadata); }, [](sp::Opaque& v, py::object x) { v.wire_metadata = document_from(x); });
    py::class_<sp::Image>(m, "ProviderImage")
        .def(py::init([](std::string mime, std::string data, sp::ImageDetail detail) {
            sp::Image image{std::move(mime), std::make_shared<const std::string>(std::move(data)), detail};
            if (!sp::valid_image(image)) throw py::value_error("Invalid inline provider image");
            return image;
        }), py::arg("mime"), py::arg("data"), py::arg("detail") = sp::ImageDetail::Auto)
        .def_readonly("mime", &sp::Image::mime)
        .def_readonly("detail", &sp::Image::detail)
        .def_property_readonly("data", [](const sp::Image& v) -> py::object { return v.data ? py::cast(*v.data) : py::none(); });
    py::class_<NativeReplayView>(m, "NativeReplay")
        .def_property_readonly("complete", [](const NativeReplayView& v) { return v.value->complete(); });
    py::class_<NativeContextView>(m, "NativeContext")
        .def_property_readonly("model", [](const NativeContextView& v) { return std::string(v.value->model()); })
        .def_property_readonly("family", [](const NativeContextView& v) { return std::string(v.value->family()); })
        .def_property_readonly("replay_eligible", [](const NativeContextView& v) { return v.value->replay_eligible(); });
    py::class_<sp::Message>(m, "ProviderMessage")
        .def(py::init([](sp::Role role, std::vector<sp::Part> parts, std::string id) {
            sp::Message message; message.role = role; message.parts = std::move(parts); message.id = std::move(id); return message;
        }), py::arg("role") = sp::Role::Assistant, py::arg("parts") = std::vector<sp::Part>{}, py::arg("id") = "")
        .def_readwrite("id", &sp::Message::id)
        .def_readwrite("role", &sp::Message::role)
        .def_property("parts", [](const sp::Message& v) { return v.parts; },
            [](sp::Message& v, std::vector<sp::Part> parts) { v.parts = std::move(parts); })
        .def_property_readonly("native", [](const sp::Message& v) -> py::object { return v.native ? py::cast(NativeReplayView{v.native}) : py::none(); })
        .def_property_readonly("wire_output", [](const sp::Message& v) { return document_view(v.wire_output); });
    auto cCount = py::class_<sp::Count>(m, "UsageCount");
    cCount.def_readonly("value", &sp::Count::value);
    cCount.def_readonly("evidence", &sp::Count::evidence);
    auto cUsageConflict = py::class_<sp::UsageConflict>(m, "UsageConflict");
    cUsageConflict.def_readonly("counter", &sp::UsageConflict::counter);
    cUsageConflict.def_readonly("detail", &sp::UsageConflict::detail);
    py::enum_<sp::CostSource>(m, "ProviderCostSource")
        .value("None", sp::CostSource::None)
        .value("OpenRouterUsd", sp::CostSource::OpenRouterUsd)
        .value("UnknownCurrency", sp::CostSource::UnknownCurrency);
    py::enum_<sp::CostStatus>(m, "ProviderCostStatus")
        .value("Missing", sp::CostStatus::Missing)
        .value("Available", sp::CostStatus::Available)
        .value("Malformed", sp::CostStatus::Malformed)
        .value("PrecisionExceeded", sp::CostStatus::PrecisionExceeded)
        .value("Overflow", sp::CostStatus::Overflow)
        .value("UnknownCurrency", sp::CostStatus::UnknownCurrency)
        .value("Conflict", sp::CostStatus::Conflict);
    py::enum_<sp::CostRounding>(m, "ProviderCostRounding")
        .value("CeilingParsedBinary64", sp::CostRounding::CeilingParsedBinary64);
    py::class_<sp::UsdAmount>(m, "ProviderUsdAmount")
        .def_readonly("nano_usd", &sp::UsdAmount::nano_usd)
        .def_readonly("evidence", &sp::UsdAmount::evidence)
        .def_readonly("rounding", &sp::UsdAmount::rounding);
    py::class_<sp::ProviderReportedCost>(m, "ProviderReportedCost")
        .def_readonly("total", &sp::ProviderReportedCost::total)
        .def_readonly("upstream_total", &sp::ProviderReportedCost::upstream_total)
        .def_readonly("upstream_input", &sp::ProviderReportedCost::upstream_input)
        .def_readonly("upstream_output", &sp::ProviderReportedCost::upstream_output)
        .def_readonly("status", &sp::ProviderReportedCost::status)
        .def_readonly("is_byok", &sp::ProviderReportedCost::is_byok)
        .def_readonly("byok_status", &sp::ProviderReportedCost::byok_status)
        .def_readonly("source", &sp::ProviderReportedCost::source)
        .def_readonly("quality", &sp::ProviderReportedCost::quality);
    auto cUsage = py::class_<sp::Usage>(m, "ProviderUsage");
    cUsage.def_readonly("input_total", &sp::Usage::input_total);
    cUsage.def_readonly("output_total", &sp::Usage::output_total);
    cUsage.def_readonly("total", &sp::Usage::total);
    cUsage.def_readonly("provider_reported_total", &sp::Usage::provider_reported_total);
    cUsage.def_readonly("input_uncached", &sp::Usage::input_uncached);
    cUsage.def_readonly("cache_read", &sp::Usage::cache_read);
    cUsage.def_readonly("cache_write", &sp::Usage::cache_write);
    cUsage.def_readonly("reasoning", &sp::Usage::reasoning);
    cUsage.def_property_readonly("extra", [](const sp::Usage& v) { return v.extra; });
    cUsage.def_readonly("stage", &sp::Usage::stage);
    cUsage.def_readonly("quality", &sp::Usage::quality);
    cUsage.def_property_readonly("conflicts", [](const sp::Usage& v) { return v.conflicts; });
    cUsage.def_readonly("provider_cost", &sp::Usage::provider_cost);
    auto cAttemptEvidence = py::class_<sp::AttemptEvidence>(m, "ProviderAttemptEvidence");
    cAttemptEvidence.def_readonly("request_may_have_left", &sp::AttemptEvidence::request_may_have_left);
    cAttemptEvidence.def_readonly("request_body_bytes", &sp::AttemptEvidence::request_body_bytes);
    cAttemptEvidence.def_readonly("response_head_seen", &sp::AttemptEvidence::response_head_seen);
    cAttemptEvidence.def_readonly("transport_internal_resends", &sp::AttemptEvidence::transport_internal_resends);
    cAttemptEvidence.def_readonly("attempts", &sp::AttemptEvidence::attempts);
    cAttemptEvidence.def_readonly("prior_usage_unknown", &sp::AttemptEvidence::prior_usage_unknown);
    auto cError = py::class_<sp::Error>(m, "ProviderError");
    cError.def_readonly("kind", &sp::Error::kind);
    cError.def_readonly("safe_message", &sp::Error::safe_message);
    cError.def_readonly("retry_class", &sp::Error::retry_class);
    cError.def_readonly("retry_safety", &sp::Error::retry_safety);
    cError.def_readonly("http_status", &sp::Error::http_status);
    cError.def_readonly("vendor_code", &sp::Error::vendor_code);
    cError.def_readonly("attempt", &sp::Error::attempt);
    auto cStopReason = py::class_<sp::StopReason>(m, "ProviderStopReason");
    cStopReason.def_readonly("kind", &sp::StopReason::kind);
    cStopReason.def_readonly("raw", &sp::StopReason::raw);
    cStopReason.def_readonly("sequence", &sp::StopReason::sequence);
    auto cRawWire = py::class_<sp::RawWire>(m, "ProviderRawWire");
    cRawWire.def_readonly("type", &sp::RawWire::type);
    auto cCompletion = py::class_<sp::Completion>(m, "ProviderCompletion");
    cCompletion.def_property_readonly("messages", [](const sp::Completion& v) { return v.messages; });
    cCompletion.def_readonly("stop", &sp::Completion::stop);
    cCompletion.def_readonly("usage", &sp::Completion::usage);
    cCompletion.def_readonly("attempt", &sp::Completion::attempt);
    cCompletion.def_property_readonly("raw_events", [](const sp::Completion& v) { return v.raw_events; });
    auto cPartialCompletion = py::class_<sp::PartialCompletion>(m, "ProviderPartialCompletion");
    cPartialCompletion.def_property_readonly("messages", [](const sp::PartialCompletion& v) { return v.messages; });
    cPartialCompletion.def_readonly("usage", &sp::PartialCompletion::usage);
    cPartialCompletion.def_readonly("stop", &sp::PartialCompletion::stop);
    cPartialCompletion.def_property_readonly("raw_events", [](const sp::PartialCompletion& v) { return v.raw_events; });
    auto cFailure = py::class_<sp::Failure>(m, "ProviderFailure");
    cFailure.def_readonly("error", &sp::Failure::error);
    cFailure.def_readonly("partial", &sp::Failure::partial);
    cStopReason.def_property_readonly("details", [](const sp::StopReason& v) { return document_view(v.details); });
    cRawWire.def_property_readonly("payload", [](const sp::RawWire& v) { return document_view(v.payload); });
    cCompletion.def_property_readonly("wire_envelope", [](const sp::Completion& v) { return document_view(v.wire_envelope); });
    cPartialCompletion.def_property_readonly("wire_envelope", [](const sp::PartialCompletion& v) { return document_view(v.wire_envelope); });
    cError.def_property_readonly("retry_after_ms", [](const sp::Error& v) -> py::object { return v.retry_after ? py::cast(v.retry_after->count()) : py::none(); });
    py::class_<ProviderOutcome>(m, "ProviderOutcome")
        .def_property_readonly("completion", [](const ProviderOutcome& v) -> py::object { const auto* c = std::get_if<sp::Completion>(v.value.get()); return c ? py::cast(*c, py::return_value_policy::copy) : py::none(); })
        .def_property_readonly("failure", [](const ProviderOutcome& v) -> py::object { const auto* f = std::get_if<sp::Failure>(v.value.get()); return f ? py::cast(*f, py::return_value_policy::copy) : py::none(); })
        .def_property_readonly("messages", [](const ProviderOutcome& v) { return outcome_messages(*v.value); })
        .def_property_readonly("usage", [](const ProviderOutcome& v) { return outcome_usage(*v.value); })
        .def_property_readonly("text", [](const ProviderOutcome& v) { return outcome_text(*v.value); });
    auto eLocalId = py::class_<sp::LocalId>(m, "ProviderLocalId");
    eLocalId.def_readonly("value", &sp::LocalId::value);
    auto ePartHeader = py::class_<sp::PartHeader>(m, "ProviderPartHeader");
    ePartHeader.def_readonly("wire_id", &sp::PartHeader::wire_id);
    ePartHeader.def_readonly("name", &sp::PartHeader::name);
    ePartHeader.def_readonly("tool_kind", &sp::PartHeader::tool_kind);
    ePartHeader.def_readonly("wire_type", &sp::PartHeader::wire_type);
    auto eBegin = py::class_<sp::Begin>(m, "ProviderBegin");
    eBegin.def_readonly("generation", &sp::Begin::generation);
    auto eMessageBegin = py::class_<sp::MessageBegin>(m, "ProviderMessageBegin");
    eMessageBegin.def_readonly("message", &sp::MessageBegin::message);
    eMessageBegin.def_readonly("vendor_id", &sp::MessageBegin::vendor_id);
    eMessageBegin.def_readonly("role", &sp::MessageBegin::role);
    auto ePartBegin = py::class_<sp::PartBegin>(m, "ProviderPartBegin");
    ePartBegin.def_readonly("message", &sp::PartBegin::message);
    ePartBegin.def_readonly("part", &sp::PartBegin::part);
    ePartBegin.def_readonly("kind", &sp::PartBegin::kind);
    ePartBegin.def_readonly("header", &sp::PartBegin::header);
    ePartBegin.def_readonly("order", &sp::PartBegin::order);
    auto eMessageSeal = py::class_<sp::MessageSeal>(m, "ProviderMessageSeal");
    eMessageSeal.def_readonly("message", &sp::MessageSeal::message);
    auto eUsageUpdate = py::class_<sp::UsageUpdate>(m, "ProviderUsageUpdate");
    eUsageUpdate.def_readonly("snapshot", &sp::UsageUpdate::snapshot);
    auto eStop = py::class_<sp::Stop>(m, "ProviderStop");
    eStop.def_readonly("reason", &sp::Stop::reason);
    auto eCommit = py::class_<sp::Commit>(m, "ProviderCommit");
    eCommit.def_readonly("evidence", &sp::Commit::evidence);
    auto eFail = py::class_<sp::Fail>(m, "ProviderFail");
    eFail.def_readonly("error", &sp::Fail::error);
    auto eResponseEnvelope = py::class_<sp::ResponseEnvelope>(m, "ProviderResponseEnvelope");
    ePartHeader.def_property_readonly("wire_metadata", [](const sp::PartHeader& v) { return document_view(v.wire_metadata); });
    eMessageSeal.def_property_readonly("wire_output", [](const sp::MessageSeal& v) { return document_view(v.wire_output); });
    eResponseEnvelope.def_property_readonly("payload", [](const sp::ResponseEnvelope& v) { return document_view(v.payload); });
    eMessageBegin.def_property_readonly("native_context", [](const sp::MessageBegin& v) -> py::object { return v.native_context ? py::cast(NativeContextView{v.native_context}) : py::none(); });
    py::class_<OwnedDelta>(m, "ProviderPartDelta")
        .def_readonly("part", &OwnedDelta::part).def_readonly("kind", &OwnedDelta::kind)
        .def_readonly("bytes", &OwnedDelta::bytes).def_readonly("channel", &OwnedDelta::channel);
    py::class_<OwnedSeal>(m, "ProviderPartSeal")
        .def_readonly("part", &OwnedSeal::part).def_readonly("snapshot", &OwnedSeal::snapshot)
        .def_property_readonly("wire_metadata", [](const OwnedSeal& v) { return document_view(v.wire_metadata); });
    py::class_<OwnedEvent>(m, "ProviderEvent")
        .def_property_readonly("kind", [](const OwnedEvent& v) {
            static constexpr const char* names[] = {"Begin", "MessageBegin", "PartBegin", "PartDelta", "PartSeal", "MessageSeal", "UsageUpdate", "Stop", "Commit", "Fail", "RawWire", "ResponseEnvelope"};
            return names[v.value.index()];
        })
        .def_property_readonly("value", [](const OwnedEvent& v) { return std::visit([](const auto& item) { return py::cast(item, py::return_value_policy::copy); }, v.value); });
    m.def("portable_message", &portable_message, py::arg("message"));
    m.def("project_message", &project_message, py::arg("message"));
    py::class_<sp::NativeArchiveLimits>(m, "NativeArchiveLimits").def(py::init<>())
        .def_readwrite("max_bytes", &sp::NativeArchiveLimits::max_bytes)
        .def_readwrite("max_messages", &sp::NativeArchiveLimits::max_messages)
        .def_readwrite("max_parts", &sp::NativeArchiveLimits::max_parts)
        .def_readwrite("max_json_depth", &sp::NativeArchiveLimits::max_json_depth)
        .def_readwrite("max_records", &sp::NativeArchiveLimits::max_records)
        .def_readwrite("max_store_bytes", &sp::NativeArchiveLimits::max_store_bytes);
    py::class_<sp::NativeArchive, std::shared_ptr<sp::NativeArchive>>(m, "NativeArchive")
        .def_static("provision", &sp::NativeArchive::provision, py::arg("directory"), py::arg("independent_key_file"), py::arg("owner_scope"), py::arg("descriptor"), py::arg("limits") = sp::NativeArchiveLimits{}, py::call_guard<py::gil_scoped_release>())
        .def_static("open", &sp::NativeArchive::open, py::arg("directory"), py::arg("independent_key_file"), py::arg("owner_scope"), py::arg("descriptor"), py::arg("limits") = sp::NativeArchiveLimits{}, py::call_guard<py::gil_scoped_release>())
        .def("save", &sp::NativeArchive::save, py::arg("messages"), py::arg("binding") = "", py::call_guard<py::gil_scoped_release>())
        .def("load", &sp::NativeArchive::load, py::arg("reference"), py::arg("binding") = "", py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("owner_scope", [](const sp::NativeArchive& v) { return std::string(v.owner_scope()); })
        .def("matches_descriptor", &sp::NativeArchive::matches_descriptor);
}
} // namespace neograph::pybind
