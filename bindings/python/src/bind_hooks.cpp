#include "json_bridge.h"

#include <neograph/hook.h>
#include <neograph/hook_outbox.h>
#include <neograph/hook_runtime.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace neograph::pybind {
namespace {

class PythonHookTarget final : public Tool {
public:
    explicit PythonHookTarget(std::string name) : name_(std::move(name)) {}
    ChatTool get_definition() const override {
        ChatTool value;
        value.name = name_;
        value.description = "Python lifecycle Hook target";
        value.parameters = json{{"type", "object"}};
        return value;
    }
    std::string execute(const json&) override {
        throw std::logic_error("Python Hook targets execute only through HookExecutionBackend");
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
};

struct PythonHookRuntimeOwner {
    std::vector<std::unique_ptr<PythonHookTarget>> targets;
    std::shared_ptr<HookRuntime> runtime;
};

std::shared_ptr<HookRuntime> make_python_hook_runtime(
    std::vector<HookDefinition> definitions,
    py::dict callbacks,
    std::uint64_t timeout_ms) {
    if (!timeout_ms) throw std::invalid_argument("Hook timeout_ms must be positive");
    auto owner = std::make_shared<PythonHookRuntimeOwner>();
    auto contracts = std::make_shared<std::map<std::string, HookTargetContract, std::less<>>>();

    std::map<std::string, std::set<std::string>, std::less<>> capabilities;
    std::map<std::string, ToolEffectClass, std::less<>> maximum_effect;
    for (const auto& definition : definitions) {
        const auto& data = definition.data();
        if (!callbacks.contains(py::str(data.target_id))) {
            throw std::invalid_argument("Missing Python Hook callback for target: " +
                                        data.target_id);
        }
        capabilities[data.target_id].insert(data.required_capabilities.begin(),
                                             data.required_capabilities.end());
        auto& effect = maximum_effect[data.target_id];
        if (static_cast<unsigned>(data.effect) > static_cast<unsigned>(effect))
            effect = data.effect;
    }
    for (const auto& [target, required] : capabilities) {
        owner->targets.push_back(std::make_unique<PythonHookTarget>(target));
        (*contracts)[target] = HookTargetContract{
            owner->targets.back().get(), required, maximum_effect.at(target)};
    }
    HookTargetResolver resolver = [contracts](std::string_view target)
        -> std::optional<HookTargetContract> {
        const auto found = contracts->find(std::string(target));
        if (found == contracts->end()) return std::nullopt;
        return found->second;
    };
    auto registry = std::make_shared<HookRegistry>(resolver);
    for (auto& definition : definitions) registry->admit(std::move(definition));

    auto held_callbacks = std::shared_ptr<py::dict>(
        new py::dict(std::move(callbacks)),
        [](py::dict* value) {
            py::gil_scoped_acquire acquire;
            delete value;
        });
    HookExecutionBackend backend =
        [held_callbacks](const HookInvocation& invocation,
                         const RuntimeEvent& event,
                         std::uint32_t attempt,
                         ToolExecutionContext) -> asio::awaitable<HookExecutionAttempt> {
        HookExecutionState state = HookExecutionState::Succeeded;
        std::string error;
        {
            py::gil_scoped_acquire acquire;
            try {
                py::object callback = (*held_callbacks)[py::str(invocation.data().target_id)];
                callback(json_to_py(invocation.data().arguments),
                         py::str(event.type()), json_to_py(event.data()));
            } catch (const py::error_already_set& exception) {
                state = HookExecutionState::Failed;
                error = exception.what();
                PyErr_Clear();
            }
        }
        const bool succeeded = state == HookExecutionState::Succeeded;
        auto receipt = HookExecutionReceipt::create(
            {invocation.id(), attempt, state,
             ExternalEffectReceipt{invocation.id(), true, succeeded,
                                   succeeded ? "python hook completed" : error},
             error});
        co_return HookExecutionAttempt{std::move(receipt), {}};
    };
    auto runner = std::make_shared<MandatoryHookRunner>(
        std::make_shared<InMemoryHookJournal>(), std::move(registry),
        std::move(backend));
    owner->runtime = std::make_shared<HookRuntime>(
        std::move(runner), std::chrono::milliseconds(timeout_ms));
    return std::shared_ptr<HookRuntime>(owner, owner->runtime.get());
}

}  // namespace

void init_hooks(py::module_& m) {
    py::enum_<HookPhase>(m, "HookPhase")
        .value("MessageAdmitted", HookPhase::MessageAdmitted)
        .value("BeforeProviderRequest", HookPhase::BeforeProviderRequest)
        .value("AfterProviderResponse", HookPhase::AfterProviderResponse)
        .value("BeforeToolExecution", HookPhase::BeforeToolExecution)
        .value("AfterToolExecution", HookPhase::AfterToolExecution)
        .value("CheckpointPublished", HookPhase::CheckpointPublished)
        .value("BeforeTerminalPublication", HookPhase::BeforeTerminalPublication)
        .value("RunFailed", HookPhase::RunFailed)
        .value("RunCancelled", HookPhase::RunCancelled);
    py::enum_<HookDelivery>(m, "HookDelivery")
        .value("BlockingMandatory", HookDelivery::BlockingMandatory)
        .value("DurableObservational", HookDelivery::DurableObservational);
    py::enum_<HookFailureMode>(m, "HookFailureMode")
        .value("FailClosed", HookFailureMode::FailClosed)
        .value("Continue", HookFailureMode::Continue);
    py::enum_<HookIdempotency>(m, "HookIdempotency")
        .value("Idempotent", HookIdempotency::Idempotent)
        .value("NonIdempotent", HookIdempotency::NonIdempotent);
    py::enum_<HookPredicateKind>(m, "HookPredicateKind")
        .value("True", HookPredicateKind::True)
        .value("All", HookPredicateKind::All)
        .value("Any", HookPredicateKind::Any)
        .value("Not", HookPredicateKind::Not)
        .value("Equals", HookPredicateKind::Equals)
        .value("Exists", HookPredicateKind::Exists);
    py::enum_<RuntimeEventField>(m, "RuntimeEventField")
        .value("Type", RuntimeEventField::Type)
        .value("OwnerScope", RuntimeEventField::OwnerScope)
        .value("RunId", RuntimeEventField::RunId)
        .value("DataPointer", RuntimeEventField::DataPointer);
    py::enum_<HookInputMapperKind>(m, "HookInputMapperKind")
        .value("JsonPointer", HookInputMapperKind::JsonPointer)
        .value("Template", HookInputMapperKind::Template)
        .value("HostMapper", HookInputMapperKind::HostMapper);
    py::enum_<ToolEffectClass>(m, "ToolEffectClass")
        .value("Unknown", ToolEffectClass::Unknown)
        .value("ReadOnly", ToolEffectClass::ReadOnly)
        .value("ExternalRead", ToolEffectClass::ExternalRead)
        .value("WorkspaceWrite", ToolEffectClass::WorkspaceWrite)
        .value("ExternalWrite", ToolEffectClass::ExternalWrite);

    py::class_<HookPredicate>(m, "HookPredicate")
        .def(py::init<>())
        .def_readwrite("kind", &HookPredicate::kind)
        .def_readwrite("field", &HookPredicate::field)
        .def_readwrite("pointer", &HookPredicate::pointer)
        .def_property("value",
            [](const HookPredicate& value) { return json_to_py(value.value); },
            [](HookPredicate& value, py::object input) { value.value = py_to_json(input); })
        .def_readwrite("children", &HookPredicate::children);
    py::class_<HookInputMapper>(m, "HookInputMapper")
        .def(py::init<>())
        .def_readwrite("kind", &HookInputMapper::kind)
        .def_readwrite("json_pointer", &HookInputMapper::json_pointer)
        .def_property("value_template",
            [](const HookInputMapper& value) { return json_to_py(value.value_template); },
            [](HookInputMapper& value, py::object input) {
                value.value_template = py_to_json(input);
            })
        .def_readwrite("host_mapper_id", &HookInputMapper::host_mapper_id);
    py::class_<HookDefinitionData>(m, "HookDefinitionData")
        .def(py::init<>())
        .def_readwrite("definition_id", &HookDefinitionData::definition_id)
        .def_readwrite("priority", &HookDefinitionData::priority)
        .def_readwrite("phase", &HookDefinitionData::phase)
        .def_readwrite("delivery", &HookDefinitionData::delivery)
        .def_readwrite("failure_mode", &HookDefinitionData::failure_mode)
        .def_readwrite("idempotency", &HookDefinitionData::idempotency)
        .def_readwrite("effect", &HookDefinitionData::effect)
        .def_readwrite("target_id", &HookDefinitionData::target_id)
        .def_readwrite("required_capabilities", &HookDefinitionData::required_capabilities)
        .def_readwrite("predicate", &HookDefinitionData::predicate)
        .def_readwrite("input_mapper", &HookDefinitionData::input_mapper);
    py::class_<HookDefinition> definition(m, "HookDefinition");
    definition.def_static("create", &HookDefinition::create)
        .def_static("parse", &HookDefinition::parse)
        .def_property_readonly("id", &HookDefinition::id)
        .def_property_readonly("data", &HookDefinition::data)
        .def("serialize_canonical", &HookDefinition::serialize_canonical);

    py::class_<HookRuntime, std::shared_ptr<HookRuntime>>(m, "HookRuntime");
    py::register_exception<HookBoundaryBlocked>(m, "HookBoundaryBlocked");
    m.def("create_hook_runtime", &make_python_hook_runtime,
          py::arg("definitions"), py::arg("callbacks"),
          py::arg("timeout_ms") = 30'000,
          "Create a mandatory lifecycle Hook runtime backed by Python callbacks.");
}

}  // namespace neograph::pybind
