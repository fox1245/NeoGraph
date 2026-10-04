// Provider admission, dispatch and immutable outcome bindings. Python subclasses
// may delegate preparation but cannot manufacture native replay authority.
#include "json_bridge.h"
#include "opaque_types.h"
#include "provider_bridge.h"
#include <neograph/tool.h>
#include <neograph/graph/cancel.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>
#include <chrono>
#ifdef NEOGRAPH_PYBIND_HAS_LLM
#include <neograph/llm/schema_provider.h>
#endif
namespace py = pybind11;
namespace neograph::pybind {
namespace {
struct DescriptorPolicyView { sp::descriptor::PolicySnapshot value; };
struct RuntimePolicyView { sp::configuration::PolicySnapshot value; };
struct PythonProviderLease {
    std::shared_ptr<Provider> native;
    py::object python;
};
class PyProvider : public Provider {
public:
    explicit PyProvider(std::string family) : family_(std::move(family)) {}
    std::string_view family() const noexcept override { return family_; }
    std::string get_name() const override {
        PYBIND11_OVERRIDE_PURE(std::string, Provider, get_name,);
    }
    PreparedProviderRequest prepare(ProviderRequest request) override {
        py::gil_scoped_acquire gil;
        auto override = py::get_override(static_cast<const Provider*>(this), "prepare");
        if (!override) py::pybind11_fail("Provider.prepare must be overridden");
        auto handle = override(std::move(request)).cast<std::shared_ptr<PreparedHandle>>();
        if (!handle) throw py::type_error("Provider.prepare must return an authentic PreparedProviderRequest");
        return handle->take();
    }
private:
    const std::string family_;
};
PyObject* outcome_error_type = nullptr;
PyObject* observer_error_type = nullptr;
PyObject* settlement_error_type = nullptr;
void set_outcome_error(PyObject* type, const ProviderOutcomeError& error) {
    py::object exception = py::reinterpret_borrow<py::object>(type)(error.what());
    exception.attr("outcome") = py::cast(error.outcome());
    auto cause = provider_cause(error.cause());
    exception.attr("cause") = cause;
    exception.attr("__cause__") = cause;
    if (const auto* observer = dynamic_cast<const ProviderObserverError*>(&error))
        exception.attr("error_kind") = py::cast(observer->error_kind());
    if (const auto* settlement = dynamic_cast<const ProviderBudgetSettlementError*>(&error))
        exception.attr("dispatch_error") = provider_cause(settlement->dispatch_error());
    PyErr_SetObject(type, exception.ptr());
}
}
std::shared_ptr<Provider> own_python_provider(py::object provider) {
    if (provider.is_none()) return {};
    auto native = provider.cast<std::shared_ptr<Provider>>();
    if (!native) throw py::type_error("Provider must own a native provider");
    auto* pointer = native.get();
    auto lease = std::shared_ptr<PythonProviderLease>(
        new PythonProviderLease{std::move(native), std::move(provider)},
        [](PythonProviderLease* owner) {
            py::gil_scoped_acquire gil;
            delete owner;
        });
    // Preserve the real provider address and virtual dispatch. Every native
    // copy retains this exact Python owner without a global or patient list.
    return std::shared_ptr<Provider>(std::move(lease), pointer);
}
void init_provider(py::module_& m) {
    py::class_<ToolCall>(m, "ToolCall")
        .def(py::init<>())
        .def(py::init([](std::string id, std::string name, std::string arguments) { return ToolCall{std::move(id), std::move(name), std::move(arguments)}; }), py::arg("id") = "", py::arg("name") = "", py::arg("arguments") = "")
        .def_readwrite("id", &ToolCall::id).def_readwrite("name", &ToolCall::name).def_readwrite("arguments", &ToolCall::arguments);
    py::class_<ChatMessage>(m, "ChatMessage", "Portable graph message projection; not native replay custody.")
        .def(py::init<>())
        .def(py::init([](std::string role, std::string content, std::vector<ToolCall> calls, std::string call_id, std::string tool_name, std::vector<std::string> images) {
            ChatMessage value; value.role=std::move(role); value.content=std::move(content); value.tool_calls=std::move(calls); value.tool_call_id=std::move(call_id); value.tool_name=std::move(tool_name); value.image_urls=std::move(images); return value;
        }), py::arg("role") = "", py::arg("content") = "", py::arg("tool_calls") = std::vector<ToolCall>{}, py::arg("tool_call_id") = "", py::arg("tool_name") = "", py::arg("image_urls") = std::vector<std::string>{})
        .def_readwrite("role", &ChatMessage::role).def_readwrite("content", &ChatMessage::content)
        .def_readwrite("tool_calls", &ChatMessage::tool_calls).def_readwrite("tool_call_id", &ChatMessage::tool_call_id)
        .def_readwrite("tool_name", &ChatMessage::tool_name).def_readwrite("image_urls", &ChatMessage::image_urls)
        .def_readwrite("tool_status", &ChatMessage::tool_status).def_readwrite("tool_retryable", &ChatMessage::tool_retryable)
        .def_readwrite("tool_effect_uncertain", &ChatMessage::tool_effect_uncertain)
        .def_readwrite("reasoning", &ChatMessage::reasoning)
        .def_property("reasoning_details", [](const ChatMessage& v) { return json_to_py(v.reasoning_details); }, [](ChatMessage& v, py::object x) { v.reasoning_details = py_to_json(x); });
    py::class_<ChatTool>(m, "ChatTool")
        .def(py::init<>())
        .def(py::init([](std::string name, std::string description, py::object parameters) { return ChatTool{std::move(name), std::move(description), py_to_json(parameters)}; }), py::arg("name") = "", py::arg("description") = "", py::arg("parameters") = py::dict())
        .def_readwrite("name", &ChatTool::name).def_readwrite("description", &ChatTool::description)
        .def_property("parameters", [](const ChatTool& v) { return json_to_py(v.parameters); }, [](ChatTool& v, py::object x) { v.parameters=py_to_json(x); });
    py::class_<GeneratedArtifact>(m, "GeneratedArtifact").def(py::init<>())
        .def_readwrite("kind", &GeneratedArtifact::kind).def_readwrite("mime_type", &GeneratedArtifact::mime_type)
        .def_readwrite("base64_data", &GeneratedArtifact::base64_data).def_readwrite("url", &GeneratedArtifact::url).def_readwrite("file_id", &GeneratedArtifact::file_id)
        .def_property("metadata", [](const GeneratedArtifact& v) { return json_to_py(v.metadata); }, [](GeneratedArtifact& v, py::object x) { v.metadata=py_to_json(x); });
    init_provider_values(m);
    auto error = py::reinterpret_steal<py::object>(PyErr_NewException("neograph_engine._neograph.ProviderOutcomeError", PyExc_RuntimeError, nullptr));
    outcome_error_type = error.ptr(); m.add_object("ProviderOutcomeError", error);
    auto observer = py::reinterpret_steal<py::object>(PyErr_NewException("neograph_engine._neograph.ProviderObserverError", error.ptr(), nullptr));
    observer_error_type = observer.ptr(); m.add_object("ProviderObserverError", observer);
    auto settlement = py::reinterpret_steal<py::object>(PyErr_NewException("neograph_engine._neograph.ProviderBudgetSettlementError", error.ptr(), nullptr));
    settlement_error_type = settlement.ptr(); m.add_object("ProviderBudgetSettlementError", settlement);
    py::register_local_exception_translator([](std::exception_ptr value) {
        try { if (value) std::rethrow_exception(value); }
        catch (const ProviderBudgetSettlementError& e) { set_outcome_error(settlement_error_type, e); }
        catch (const ProviderObserverError& e) { set_outcome_error(observer_error_type, e); }
        catch (const ProviderOutcomeError& e) { set_outcome_error(outcome_error_type, e); }
    });
    py::enum_<ProviderMode>(m, "ProviderMode").value("Collect", ProviderMode::Collect).value("Stream", ProviderMode::Stream);
    py::class_<sp::OpenRouterRouting>(m, "OpenRouterRouting")
        .def(py::init<>())
        .def_readwrite("zdr", &sp::OpenRouterRouting::zdr).def_readwrite("allow_fallbacks", &sp::OpenRouterRouting::allow_fallbacks)
        .def_readwrite("require_parameters", &sp::OpenRouterRouting::require_parameters)
        .def_readwrite("only", &sp::OpenRouterRouting::only).def_readwrite("order", &sp::OpenRouterRouting::order)
        .def_readwrite("ignore", &sp::OpenRouterRouting::ignore).def_readwrite("data_collection", &sp::OpenRouterRouting::data_collection);
    auto format = py::class_<sp::ResponseFormat>(m, "ProviderResponseFormat").def(py::init<>())
        .def_readwrite("kind", &sp::ResponseFormat::kind).def_readwrite("name", &sp::ResponseFormat::name)
        .def_readwrite("description", &sp::ResponseFormat::description).def_readwrite("strict", &sp::ResponseFormat::strict)
        .def_property("schema", [](const sp::ResponseFormat& v) -> py::object { return v.schema ? py::module_::import("json").attr("loads")(v.schema->root().dump()) : py::none(); }, [](sp::ResponseFormat& v, py::object x) {
            if (x.is_none()) { v.schema.reset(); return; }
            auto parsed = sp::json::parse(py_to_json(x).dump());
            auto* document = std::get_if<sp::json::Document>(&parsed);
            if (!document) throw py::value_error("Invalid response schema JSON");
            v.schema = std::make_shared<const sp::json::Document>(std::move(*document));
        });
    py::enum_<sp::ResponseFormat::Kind>(format, "Kind").value("JsonObject", sp::ResponseFormat::Kind::JsonObject).value("JsonSchema", sp::ResponseFormat::Kind::JsonSchema);
    init_provider_controls(m);
    auto controls = py::class_<ProviderControls>(m, "ProviderControls").def(py::init<>());
    controls.def_readwrite("max_output_tokens", &ProviderControls::max_output_tokens);
    controls.def_readwrite("max_tool_calls", &ProviderControls::max_tool_calls);
    controls.def_readwrite("temperature", &ProviderControls::temperature);
    controls.def_readwrite("top_p", &ProviderControls::top_p);
    controls.def_readwrite("reasoning_effort", &ProviderControls::reasoning_effort);
    controls.def_readwrite("reasoning_summary", &ProviderControls::reasoning_summary);
    controls.def_readwrite("thinking_budget", &ProviderControls::thinking_budget);
    controls.def_readwrite("include_thoughts", &ProviderControls::include_thoughts);
    controls.def_readwrite("thinking_level", &ProviderControls::thinking_level);
    controls.def_readwrite("thinking_summaries", &ProviderControls::thinking_summaries);
    controls.def_readwrite("service_tier", &ProviderControls::service_tier);
    controls.def_readwrite("required_tool", &ProviderControls::required_tool);
    controls.def_property("provider", [](const ProviderControls& v) { return v.provider; },
        [](ProviderControls& v, std::optional<sp::OpenRouterRouting> routing) { v.provider = std::move(routing); });
    controls.def_property("response_format", [](const ProviderControls& v) { return v.response_format; },
        [](ProviderControls& v, std::optional<sp::ResponseFormat> format) { v.response_format = std::move(format); });
    controls.def_readwrite("store", &ProviderControls::store);
    controls.def_readwrite("account_scope", &ProviderControls::account_scope);
    controls.def_readwrite("system", &ProviderControls::system);
    controls.def_property("chat_reasoning", [](const ProviderControls& v) { return v.chat_reasoning; },
        [](ProviderControls& v, std::optional<sp::chat::ReasoningOptions> value) { v.chat_reasoning = std::move(value); });
    controls.def_readwrite("include_reasoning", &ProviderControls::include_reasoning);
    controls.def_readwrite("usage_include", &ProviderControls::usage_include);
    controls.def_property("models", [](const ProviderControls& v) { return v.models; },
        [](ProviderControls& v, std::vector<std::string> value) { v.models = std::move(value); });
    controls.def_readwrite("previous_response_id", &ProviderControls::previous_response_id);
    controls.def_property("previous_response_history", [](const ProviderControls& v) { return v.previous_response_history; },
        [](ProviderControls& v, std::vector<sp::Message> value) { v.previous_response_history = std::move(value); });
    controls.def_readwrite("parallel_tool_calls", &ProviderControls::parallel_tool_calls);
    controls.def_readwrite("verbosity", &ProviderControls::verbosity);
    controls.def_readwrite("truncation", &ProviderControls::truncation);
    controls.def_property("responses_include", [](const ProviderControls& v) { return v.responses_include; },
        [](ProviderControls& v, std::optional<std::vector<sp::responses::Include>> value) { v.responses_include = std::move(value); });
    controls.def_readwrite("thinking_mode", &ProviderControls::thinking_mode);
    controls.def_readwrite("output_effort", &ProviderControls::output_effort);
    controls.def_property("cache_control", [](const ProviderControls& v) { return v.cache_control; },
        [](ProviderControls& v, std::optional<sp::messages::CacheControl> value) { v.cache_control = std::move(value); });
    controls.def_property("messages_tool_choice", [](const ProviderControls& v) { return v.messages_tool_choice; },
        [](ProviderControls& v, std::optional<sp::messages::ToolChoice> value) { v.messages_tool_choice = std::move(value); });
    controls.def_readwrite("gemini_history_mode", &ProviderControls::gemini_history_mode);
    controls.def_readwrite("gemini_thinking_level", &ProviderControls::gemini_thinking_level);
    controls.def_property("safety_settings", [](const ProviderControls& v) { return v.safety_settings; },
        [](ProviderControls& v, std::vector<sp::gemini::SafetySetting> value) { v.safety_settings = std::move(value); });
    controls.def_property("gemini_tool_choice", [](const ProviderControls& v) { return v.gemini_tool_choice; },
        [](ProviderControls& v, std::optional<sp::gemini::ToolChoice> value) { v.gemini_tool_choice = std::move(value); });
    py::class_<ProviderObserverLimits>(m, "ProviderObserverLimits").def(py::init<>())
        .def_readwrite("max_events", &ProviderObserverLimits::max_events).def_readwrite("max_bytes", &ProviderObserverLimits::max_bytes);
    py::class_<sp::runtime::RetryPolicy>(m, "ProviderRetryPolicy").def(py::init<>())
        .def_readwrite("enabled", &sp::runtime::RetryPolicy::enabled)
        .def_readwrite("allow_duplicate_billing_risk", &sp::runtime::RetryPolicy::allow_duplicate_billing_risk)
        .def_readwrite("max_attempts", &sp::runtime::RetryPolicy::max_attempts)
        .def_property("base_delay_ms", [](const sp::runtime::RetryPolicy& v) { return v.base_delay.count(); }, [](sp::runtime::RetryPolicy& v, std::int64_t x) { v.base_delay = std::chrono::milliseconds(x); })
        .def_property("max_delay_ms", [](const sp::runtime::RetryPolicy& v) { return v.max_delay.count(); }, [](sp::runtime::RetryPolicy& v, std::int64_t x) { v.max_delay = std::chrono::milliseconds(x); });
    py::class_<ProviderRequest>(m, "ProviderRequest")
        .def_readwrite("mode", &ProviderRequest::mode)
        .def_readwrite("cancel_token", &ProviderRequest::cancel_token)
        .def_readwrite("observer_limits", &ProviderRequest::observer_limits)
        .def_property("messages", [](const ProviderRequest& v) { return provider_request_messages(v); }, [](ProviderRequest& v, std::vector<sp::Message> x) { set_provider_request_messages(v, std::move(x)); })
        .def_property("on_event",
            [](const ProviderRequest& v) { return provider_observer_function(v.on_event); },
            [](ProviderRequest& v, py::object callback) { v.on_event = provider_observer(std::move(callback)); })
        .def_property("timeout_ms", [](const ProviderRequest& v) -> py::object {
            if (!v.options.deadline) return py::none();
            const auto now = std::chrono::steady_clock::now();
            if (*v.options.deadline <= now) return py::int_(0);
            return py::cast(std::chrono::duration_cast<std::chrono::milliseconds>(*v.options.deadline - now).count());
        }, [](ProviderRequest& v, std::optional<std::int64_t> ms) {
            if (!ms) { v.options.deadline.reset(); return; }
            const auto now = std::chrono::steady_clock::now();
            const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(
                sp::runtime::SteadyTime::max() - now).count();
            if (*ms < 0 || *ms > maximum)
                throw py::value_error("timeout_ms must be a representable nonnegative duration");
            v.options.deadline = now + std::chrono::milliseconds(*ms);
        })
        .def_property("retry", [](const ProviderRequest& v) { return v.options.retry; }, [](ProviderRequest& v, std::optional<sp::runtime::RetryPolicy> x) { v.options.retry = std::move(x); });
    py::class_<PreparedHandle, std::shared_ptr<PreparedHandle>>(m, "PreparedProviderRequest")
        .def_property_readonly("consumed", [](const PreparedHandle& v) { return !v.value; })
        .def_property_readonly("valid", [](const PreparedHandle& v) { return v.value && v.value->valid(); })
        .def_property_readonly("error", [](const PreparedHandle& v) -> py::object { const auto* error = v.get().error(); return error ? py::cast(*error, py::return_value_policy::copy) : py::none(); })
        .def_property_readonly("family", [](const PreparedHandle& v) { return std::string(v.get().family()); })
        .def_property_readonly("model", [](const PreparedHandle& v) { return std::string(v.get().model()); })
        .def_property_readonly("encoded_body", [](const PreparedHandle& v) { return std::string(v.get().encoded_body()); }, "Sensitive admitted wire body; not a portable replay authority.")
        .def_property_readonly("mode", [](const PreparedHandle& v) { return v.get().mode(); })
        .def_property_readonly("is_cancelled", [](const PreparedHandle& v) { return v.get().is_cancelled(); })
        .def_property_readonly("max_output_tokens", [](const PreparedHandle& v) { return v.get().max_output_tokens(); })
        .def_property_readonly("requires_native_custody", [](const PreparedHandle& v) { return v.get().requires_native_custody(); })
        .def_property_readonly("descriptor", [](const PreparedHandle& v) -> py::object { auto* descriptor = v.get().admitted_descriptor(); return descriptor ? py::cast(*descriptor, py::return_value_policy::copy) : py::none(); });
    py::class_<Provider, PyProvider, std::shared_ptr<Provider>>(m, "Provider")
        .def(py::init_alias<std::string>(), py::arg("family"))
        .def("get_name", &Provider::get_name)
        .def("family", [](const Provider& v) { return std::string(v.family()); })
        .def("prepare", [](Provider& self, ProviderRequest request) { return std::make_shared<PreparedHandle>(self.prepare(std::move(request))); }, py::arg("request"))
        .def("dispatch", [](Provider& self, PreparedHandle& prepared) { auto request = prepared.take(); py::gil_scoped_release release; return self.dispatch(std::move(request)); }, py::arg("prepared"))
        .def("invoke", [](Provider& self, ProviderRequest request) { py::gil_scoped_release release; return self.invoke(std::move(request)); }, py::arg("request"))
        .def_static("request_digest", [](const PreparedHandle& request) { return Provider::request_digest(request.get()); })
        .def_static("conservative_token_upper_bound", [](const PreparedHandle& request) { return Provider::conservative_token_upper_bound(request.get()); });
    m.def("make_provider_request", &make_provider_request, py::arg("provider"), py::arg("model"), py::arg("messages"), py::arg("tools") = std::vector<ChatTool>{}, py::arg("controls") = ProviderControls{}, py::arg("mode") = ProviderMode::Collect);
    py::class_<Tool, std::shared_ptr<Tool>>(m, "Tool", "Native callable tool; Python tools use the package Tool wrapper.");
    py::class_<sp::descriptor::ValidatedDescriptor>(m, "ValidatedDescriptor")
        .def_property_readonly("id", [](const sp::descriptor::ValidatedDescriptor& v) { return std::string(v.id()); })
        .def_property_readonly("revision", &sp::descriptor::ValidatedDescriptor::revision)
        .def_property_readonly("family", [](const sp::descriptor::ValidatedDescriptor& v) { return std::string(v.family()); })
        .def_property_readonly("base_url", [](const sp::descriptor::ValidatedDescriptor& v) { return std::string(v.base_url()); });
    py::class_<DescriptorPolicyView>(m, "ProviderDescriptorPolicy")
        .def_property_readonly("identity", [](const DescriptorPolicyView& v) {
            const auto digest = v.value->identity();
            return py::bytes(digest.data(), digest.size());
        });
    m.def("builtin_provider_policy", [] { return DescriptorPolicyView{sp::descriptor::builtin_policy()}; });
    m.def("provider_policy_json", [] { return std::string(sp::config_defaults::descriptor_policy_json); });
    m.def("provider_codec_defaults_json", [] { return std::string(sp::config_defaults::codec_defaults_json); });
    m.def("load_provider_policy", [](const std::string& families, const std::string& resources) {
        auto result = sp::descriptor::load_policy(families, resources);
        if (auto* error = std::get_if<sp::descriptor::ConfigError>(&result))
            throw py::value_error(error->pointer + ": " + error->message);
        return DescriptorPolicyView{std::get<sp::descriptor::PolicySnapshot>(std::move(result))};
    }, py::arg("family_json"), py::arg("resource_json"));
    m.def("load_provider_descriptor", [](const std::string& source, std::optional<DescriptorPolicyView> policy) {
        auto result = sp::descriptor::load(source, policy ? policy->value : sp::descriptor::builtin_policy());
        if (auto* error = std::get_if<sp::descriptor::ConfigError>(&result))
            throw py::value_error(error->pointer + ": " + error->message);
        return std::get<sp::descriptor::ValidatedDescriptor>(std::move(result));
    }, py::arg("source"), py::arg("policy") = py::none());
    py::class_<sp::descriptor::DeploymentHeaderEnvironment>(m, "ProviderDeploymentHeaderEnvironment")
        .def(py::init<>())
        .def_readwrite("anthropic_workspace_id", &sp::descriptor::DeploymentHeaderEnvironment::anthropic_workspace_id)
        .def_readwrite("anthropic_beta", &sp::descriptor::DeploymentHeaderEnvironment::anthropic_beta);
    m.def("load_provider_descriptor_with_environment_headers",
        [](const std::string& source, const std::vector<std::pair<std::string, std::string>>& overrides,
           std::optional<DescriptorPolicyView> policy) {
            auto result = sp::descriptor::load_with_environment_headers(
                source, overrides, policy ? policy->value : sp::descriptor::builtin_policy());
            if (auto* error = std::get_if<sp::descriptor::ConfigError>(&result))
                throw py::value_error(error->pointer + ": " + error->message);
            return std::get<sp::descriptor::ValidatedDescriptor>(std::move(result));
        }, py::arg("source"), py::arg("overrides") = std::vector<std::pair<std::string, std::string>>{},
           py::arg("policy") = py::none());
    m.def("load_provider_descriptor_with_deployment_headers",
        [](const std::string& source, const std::vector<std::pair<std::string, std::string>>& overrides,
           const sp::descriptor::DeploymentHeaderEnvironment& environment,
           std::optional<DescriptorPolicyView> policy) {
            auto result = sp::descriptor::load_with_deployment_headers(
                source, overrides, environment, policy ? policy->value : sp::descriptor::builtin_policy());
            if (auto* error = std::get_if<sp::descriptor::ConfigError>(&result))
                throw py::value_error(error->pointer + ": " + error->message);
            return std::get<sp::descriptor::ValidatedDescriptor>(std::move(result));
        }, py::arg("source"), py::arg("overrides"), py::arg("environment"), py::arg("policy") = py::none());
    py::class_<RuntimePolicyView>(m, "ProviderRuntimePolicy");
    m.def("builtin_provider_runtime_policy", [] { return RuntimePolicyView{sp::configuration::builtin_runtime_policy()}; });
    m.def("load_provider_runtime_policy", [](const std::string& runtime, const std::string& errors) {
        auto result = sp::configuration::load_runtime_policy(runtime, errors);
        if (auto* error = std::get_if<sp::descriptor::ConfigError>(&result))
            throw py::value_error(error->pointer + ": " + error->message);
        return RuntimePolicyView{std::get<sp::configuration::PolicySnapshot>(std::move(result))};
    }, py::arg("runtime_json"), py::arg("error_json"));
    py::enum_<sp::transport::HttpVersion>(m, "ProviderHttpVersion")
        .value("Auto", sp::transport::HttpVersion::Auto).value("Http1_1", sp::transport::HttpVersion::Http1_1)
        .value("Http2PriorKnowledge", sp::transport::HttpVersion::Http2PriorKnowledge)
        .value("Http3Preferred", sp::transport::HttpVersion::Http3Preferred).value("Http3Only", sp::transport::HttpVersion::Http3Only);
    auto options = py::class_<sp::runtime::Options>(m, "ProviderRuntimeOptions")
        .def(py::init([](std::string key, std::int64_t timeout, std::string ca_file, std::size_t workers) {
            sp::runtime::Options options; options.api_key=std::move(key); options.default_timeout=std::chrono::milliseconds(timeout); options.ca_file=std::move(ca_file); options.workers=workers; return options;
        }), py::arg("api_key") = "", py::arg("default_timeout_ms") = sp::config_defaults::defaults_default_timeout_ms, py::arg("ca_file") = "", py::arg("workers") = sp::config_defaults::defaults_workers)
        .def_readwrite("api_key", &sp::runtime::Options::api_key).def_readwrite("ca_file", &sp::runtime::Options::ca_file)
        .def_readwrite("workers", &sp::runtime::Options::workers).def_readwrite("http_version", &sp::runtime::Options::http_version)
        .def_readwrite("limits", &sp::runtime::Options::limits)
        .def_readwrite("transport", &sp::runtime::Options::transport)
        .def_readwrite("retry_tokens", &sp::runtime::Options::retry_tokens).def_readwrite("retry_tokens_per_second", &sp::runtime::Options::retry_tokens_per_second)
        .def_property("default_timeout_ms", [](const sp::runtime::Options& v) { return v.default_timeout.count(); }, [](sp::runtime::Options& v, std::int64_t x) { v.default_timeout=std::chrono::milliseconds(x); });
    options.def_property("policy", [](const sp::runtime::Options& v) { return RuntimePolicyView{v.policy}; },
        [](sp::runtime::Options& v, RuntimePolicyView policy) { v.policy = std::move(policy.value); });
    options.def_property("slow_callback_threshold_ms",
        [](const sp::runtime::Options& v) { return v.slow_callback_threshold.count(); },
        [](sp::runtime::Options& v, std::int64_t x) { v.slow_callback_threshold = std::chrono::milliseconds(x); });
    py::class_<sp::transport::TransportOptions>(m, "ProviderTransportOptions").def(py::init<>())
        .def_readwrite("io_threads", &sp::transport::TransportOptions::io_threads)
        .def_readwrite("max_host_connections", &sp::transport::TransportOptions::max_host_connections)
        .def_readwrite("max_head_bytes", &sp::transport::TransportOptions::max_head_bytes)
        .def_readwrite("resolver_threads", &sp::transport::TransportOptions::resolver_threads)
        .def_readwrite("resolve", &sp::transport::TransportOptions::resolve)
        .def_property("dns_ttl_seconds", [](const sp::transport::TransportOptions& v) { return v.dns_ttl.count(); },
            [](sp::transport::TransportOptions& v, std::int64_t x) { v.dns_ttl = std::chrono::seconds(x); });
    py::class_<sp::transport::SseLimits>(m, "ProviderSseLimits").def(py::init<>())
        .def_readwrite("max_line_bytes", &sp::transport::SseLimits::max_line_bytes)
        .def_readwrite("max_event_bytes", &sp::transport::SseLimits::max_event_bytes)
        .def_readwrite("max_total_bytes", &sp::transport::SseLimits::max_total_bytes);
    py::class_<sp::SemanticLimits>(m, "ProviderSemanticLimits").def(py::init<>())
        .def_readwrite("max_parts", &sp::SemanticLimits::max_parts).def_readwrite("max_content_bytes", &sp::SemanticLimits::max_content_bytes)
        .def_readwrite("max_tool_bytes", &sp::SemanticLimits::max_tool_bytes).def_readwrite("max_json_depth", &sp::SemanticLimits::max_json_depth);
    py::class_<sp::runtime::Limits>(m, "ProviderRuntimeLimits").def(py::init<>())
        .def_readwrite("max_operations", &sp::runtime::Limits::max_operations)
        .def_readwrite("queued_body_chunks", &sp::runtime::Limits::queued_body_chunks).def_readwrite("queued_body_bytes", &sp::runtime::Limits::queued_body_bytes)
        .def_readwrite("max_response_bytes", &sp::runtime::Limits::max_response_bytes).def_readwrite("max_error_bytes", &sp::runtime::Limits::max_error_bytes)
        .def_readwrite("semantic", &sp::runtime::Limits::semantic)
        .def_readwrite("sse", &sp::runtime::Limits::sse);
#ifdef NEOGRAPH_PYBIND_HAS_LLM
    py::class_<llm::SchemaProvider::Defaults>(m, "SchemaProviderDefaults").def(py::init<>())
        .def_property("provider", [](const llm::SchemaProvider::Defaults& v) { return v.provider; },
            [](llm::SchemaProvider::Defaults& v, std::optional<sp::OpenRouterRouting> routing) { v.provider = std::move(routing); })
        .def_readwrite("responses_store", &llm::SchemaProvider::Defaults::responses_store);
    py::class_<llm::SchemaProvider, Provider, std::shared_ptr<llm::SchemaProvider>>(m, "SchemaProvider")
        .def(py::init<sp::descriptor::ValidatedDescriptor, sp::runtime::Options, llm::SchemaProvider::Defaults>(), py::arg("descriptor"), py::arg("options") = sp::runtime::Options{}, py::arg("defaults") = llm::SchemaProvider::Defaults{});
#endif
}
} // namespace neograph::pybind
