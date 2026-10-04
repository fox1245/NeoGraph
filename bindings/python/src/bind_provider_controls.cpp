// Family-specific controls are the SDK's closed typed contract. These classes
// carry caller data; their use is admitted by the actual provider encoder.
#include "provider_bridge.h"
#include <pybind11/stl.h>

namespace py = pybind11;
namespace neograph::pybind {

void init_provider_controls(py::module_& m) {
    py::class_<sp::chat::ReasoningOptions>(m, "ChatReasoningOptions")
        .def(py::init<>())
        .def_readwrite("effort", &sp::chat::ReasoningOptions::effort)
        .def_readwrite("max_tokens", &sp::chat::ReasoningOptions::max_tokens)
        .def_readwrite("exclude", &sp::chat::ReasoningOptions::exclude)
        .def_readwrite("enabled", &sp::chat::ReasoningOptions::enabled);

    py::enum_<sp::responses::Verbosity>(m, "ResponsesVerbosity")
        .value("Low", sp::responses::Verbosity::Low)
        .value("Medium", sp::responses::Verbosity::Medium)
        .value("High", sp::responses::Verbosity::High);
    py::enum_<sp::responses::Truncation>(m, "ResponsesTruncation")
        .value("Disabled", sp::responses::Truncation::Disabled)
        .value("Auto", sp::responses::Truncation::Auto);
    py::enum_<sp::responses::Include>(m, "ResponsesInclude")
        .value("ReasoningEncryptedContent", sp::responses::Include::ReasoningEncryptedContent)
        .value("WebSearchSources", sp::responses::Include::WebSearchSources)
        .value("FileSearchResults", sp::responses::Include::FileSearchResults)
        .value("MessageOutputTextLogprobs", sp::responses::Include::MessageOutputTextLogprobs)
        .value("ComputerCallOutputImageUrl", sp::responses::Include::ComputerCallOutputImageUrl)
        .value("CodeInterpreterCallOutputs", sp::responses::Include::CodeInterpreterCallOutputs);

    py::enum_<sp::messages::ThinkingMode>(m, "MessagesThinkingMode")
        .value("Manual", sp::messages::ThinkingMode::Manual)
        .value("Adaptive", sp::messages::ThinkingMode::Adaptive)
        .value("Disabled", sp::messages::ThinkingMode::Disabled);
    py::enum_<sp::messages::OutputEffort>(m, "MessagesOutputEffort")
        .value("Low", sp::messages::OutputEffort::Low)
        .value("Medium", sp::messages::OutputEffort::Medium)
        .value("High", sp::messages::OutputEffort::High)
        .value("Max", sp::messages::OutputEffort::Max);
    py::enum_<sp::messages::CacheTtl>(m, "MessagesCacheTtl")
        .value("FiveMinutes", sp::messages::CacheTtl::FiveMinutes)
        .value("OneHour", sp::messages::CacheTtl::OneHour);
    py::class_<sp::messages::CacheControl>(m, "MessagesCacheControl")
        .def(py::init<>())
        .def_readwrite("ttl", &sp::messages::CacheControl::ttl);
    py::enum_<sp::messages::ToolChoiceMode>(m, "MessagesToolChoiceMode")
        .value("Auto", sp::messages::ToolChoiceMode::Auto)
        .value("Any", sp::messages::ToolChoiceMode::Any)
        .value("None_", sp::messages::ToolChoiceMode::None)
        .value("Tool", sp::messages::ToolChoiceMode::Tool);
    py::class_<sp::messages::ToolChoice>(m, "MessagesToolChoice")
        .def(py::init<>())
        .def_readwrite("mode", &sp::messages::ToolChoice::mode)
        .def_readwrite("name", &sp::messages::ToolChoice::name)
        .def_readwrite("disable_parallel_tool_use", &sp::messages::ToolChoice::disable_parallel_tool_use);

    py::enum_<sp::gemini::HistoryMode>(m, "GeminiHistoryMode")
        .value("NativeOnly", sp::gemini::HistoryMode::NativeOnly)
        .value("PortableForeign", sp::gemini::HistoryMode::PortableForeign);
    py::enum_<sp::gemini::ThinkingLevel>(m, "GeminiThinkingLevel")
        .value("Minimal", sp::gemini::ThinkingLevel::Minimal)
        .value("Low", sp::gemini::ThinkingLevel::Low)
        .value("Medium", sp::gemini::ThinkingLevel::Medium)
        .value("High", sp::gemini::ThinkingLevel::High);
    py::enum_<sp::gemini::SafetyCategory>(m, "GeminiSafetyCategory")
        .value("Harassment", sp::gemini::SafetyCategory::Harassment)
        .value("HateSpeech", sp::gemini::SafetyCategory::HateSpeech)
        .value("SexuallyExplicit", sp::gemini::SafetyCategory::SexuallyExplicit)
        .value("DangerousContent", sp::gemini::SafetyCategory::DangerousContent)
        .value("CivicIntegrity", sp::gemini::SafetyCategory::CivicIntegrity);
    py::enum_<sp::gemini::SafetyThreshold>(m, "GeminiSafetyThreshold")
        .value("BlockNone", sp::gemini::SafetyThreshold::BlockNone)
        .value("BlockOnlyHigh", sp::gemini::SafetyThreshold::BlockOnlyHigh)
        .value("BlockMediumAndAbove", sp::gemini::SafetyThreshold::BlockMediumAndAbove)
        .value("BlockLowAndAbove", sp::gemini::SafetyThreshold::BlockLowAndAbove)
        .value("Off", sp::gemini::SafetyThreshold::Off);
    py::class_<sp::gemini::SafetySetting>(m, "GeminiSafetySetting")
        .def(py::init<>())
        .def_readwrite("category", &sp::gemini::SafetySetting::category)
        .def_readwrite("threshold", &sp::gemini::SafetySetting::threshold);
    py::enum_<sp::gemini::ToolChoiceMode>(m, "GeminiToolChoiceMode")
        .value("Auto", sp::gemini::ToolChoiceMode::Auto)
        .value("Any", sp::gemini::ToolChoiceMode::Any)
        .value("None_", sp::gemini::ToolChoiceMode::None)
        .value("Validated", sp::gemini::ToolChoiceMode::Validated);
    py::class_<sp::gemini::ToolChoice>(m, "GeminiToolChoice")
        .def(py::init<>())
        .def_readwrite("mode", &sp::gemini::ToolChoice::mode)
        .def_property("allowed_function_names",
            [](const sp::gemini::ToolChoice& v) { return v.allowed_function_names; },
            [](sp::gemini::ToolChoice& v, std::vector<std::string> names) {
                v.allowed_function_names = std::move(names);
            });
}

} // namespace neograph::pybind
