// A2A data, discovery-selected dialects, RPC failures and native SSE dispatch.
// Callback snapshots own their data; Python never borrows a parsed stream frame.
#include "json_bridge.h"
#include "opaque_types.h"

#include <neograph/a2a/client.h>
#include <neograph/a2a/types.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <string>

namespace py = pybind11;
namespace neograph::pybind {
namespace {
PyObject* rpc_error_type = nullptr;

neograph::a2a::A2AClient::EventCallback a2a_observer(py::function callback) {
    auto held = std::shared_ptr<py::function>(new py::function(std::move(callback)),
        [](py::function* function) { py::gil_scoped_acquire gil; delete function; });
    return [held](const neograph::a2a::StreamEvent& event) {
        py::gil_scoped_acquire gil;
        return (*held)(py::cast(event, py::return_value_policy::copy)).cast<bool>();
    };
}
}

void init_a2a(py::module_& m) {
    using namespace neograph::a2a;
    auto a2a = m.def_submodule("a2a", "Native A2A JSON-RPC discovery, tasks and streaming over HTTP.");
    auto rpc_error = py::register_exception<A2ARpcError>(a2a, "A2ARpcError", PyExc_RuntimeError);
    rpc_error_type = rpc_error.ptr();
    py::register_local_exception_translator([](std::exception_ptr value) {
        try { if (value) std::rethrow_exception(value); }
        catch (const A2ARpcError& error) {
            auto exception = py::reinterpret_borrow<py::object>(rpc_error_type)(error.what());
            exception.attr("code") = error.code();
            PyErr_SetObject(rpc_error_type, exception.ptr());
        }
    });
    py::enum_<WireDialect>(a2a, "WireDialect")
        .value("V0_3", WireDialect::V0_3)
        .value("V1_0", WireDialect::V1_0);
    py::enum_<TaskState>(a2a, "TaskState")
        .value("Submitted", TaskState::Submitted)
        .value("Working", TaskState::Working)
        .value("InputRequired", TaskState::InputRequired)
        .value("Completed", TaskState::Completed)
        .value("Canceled", TaskState::Canceled)
        .value("Failed", TaskState::Failed)
        .value("Rejected", TaskState::Rejected)
        .value("AuthRequired", TaskState::AuthRequired)
        .value("Unknown", TaskState::Unknown);
    py::enum_<Role>(a2a, "Role").value("User", Role::User).value("Agent", Role::Agent);

    py::class_<Part>(a2a, "Part", "Text, file or data fragment; media_type is a 1.0 field.")
        .def(py::init<>())
        .def_static("text_part", &Part::text_part, py::arg("text"))
        .def_readwrite("kind", &Part::kind)
        .def_readwrite("text", &Part::text)
        .def_readwrite("media_type", &Part::media_type)
        .def_property("file", [](const Part& v) { return json_to_py(v.file); },
            [](Part& v, py::object value) { v.file = py_to_json(value); })
        .def_property("data", [](const Part& v) { return json_to_py(v.data); },
            [](Part& v, py::object value) { v.data = py_to_json(value); })
        .def_property("metadata", [](const Part& v) { return json_to_py(v.metadata); },
            [](Part& v, py::object value) { v.metadata = py_to_json(value); })
        .def("__repr__", [](const Part& v) {
            const auto preview = v.text.size() > 40 ? v.text.substr(0, 40) + "..." : v.text;
            return "<a2a.Part kind=" + v.kind + (v.kind == "text" ? " text=\"" + preview + "\"" : "") + ">";
        });
    py::class_<Message>(a2a, "Message", "Owned user-or-agent turn; assign detached parts to update it.")
        .def(py::init<>())
        .def_readwrite("message_id", &Message::message_id)
        .def_readwrite("role", &Message::role)
        .def_property("parts", [](const Message& v) { return v.parts; },
            [](Message& v, std::vector<Part> value) { v.parts = std::move(value); })
        .def_property("task_id", [](const Message& v) { return v.task_id.value_or(""); },
            [](Message& v, const std::string& value) { v.task_id = value.empty() ? std::nullopt : std::optional<std::string>(value); })
        .def_property("context_id", [](const Message& v) { return v.context_id.value_or(""); },
            [](Message& v, const std::string& value) { v.context_id = value.empty() ? std::nullopt : std::optional<std::string>(value); })
        .def_readwrite("reference_task_ids", &Message::reference_task_ids)
        .def_readwrite("extensions", &Message::extensions)
        .def_property("metadata", [](const Message& v) { return json_to_py(v.metadata); },
            [](Message& v, py::object value) { v.metadata = py_to_json(value); });
    py::class_<Artifact>(a2a, "Artifact")
        .def(py::init<>())
        .def_readwrite("artifact_id", &Artifact::artifact_id)
        .def_readwrite("name", &Artifact::name)
        .def_readwrite("description", &Artifact::description)
        .def_property("parts", [](const Artifact& v) { return v.parts; },
            [](Artifact& v, std::vector<Part> value) { v.parts = std::move(value); })
        .def_property("metadata", [](const Artifact& v) { return json_to_py(v.metadata); },
            [](Artifact& v, py::object value) { v.metadata = py_to_json(value); });
    py::class_<TaskStatus>(a2a, "TaskStatus")
        .def(py::init<>())
        .def_readwrite("state", &TaskStatus::state)
        .def_readwrite("timestamp", &TaskStatus::timestamp)
        .def_property("message", [](const TaskStatus& v) { return v.message; },
            [](TaskStatus& v, std::optional<Message> value) { v.message = std::move(value); });
    py::class_<Task>(a2a, "Task", "Native task result with status, artifacts and full message history.")
        .def(py::init<>())
        .def_readwrite("id", &Task::id)
        .def_readwrite("context_id", &Task::context_id)
        .def_property_readonly("state", [](const Task& v) { return v.status.state; })
        .def_readwrite("status", &Task::status)
        .def_property("history", [](const Task& v) { return v.history; },
            [](Task& v, std::vector<Message> value) { v.history = std::move(value); })
        .def_property("artifacts", [](const Task& v) { return v.artifacts; },
            [](Task& v, std::vector<Artifact> value) { v.artifacts = std::move(value); })
        .def_property("metadata", [](const Task& v) { return json_to_py(v.metadata); },
            [](Task& v, py::object value) { v.metadata = py_to_json(value); })
        .def("__repr__", [](const Task& v) { return "<a2a.Task id=" + v.id + " state=" + task_state_to_string(v.status.state) + ">"; });
    py::class_<MessageSendConfiguration>(a2a, "MessageSendConfiguration")
        .def(py::init<>())
        .def_readwrite("accepted_output_modes", &MessageSendConfiguration::accepted_output_modes)
        .def_readwrite("blocking", &MessageSendConfiguration::blocking)
        .def_readwrite("history_length", &MessageSendConfiguration::history_length);
    py::class_<MessageSendParams>(a2a, "MessageSendParams")
        .def(py::init<>())
        .def_readwrite("message", &MessageSendParams::message)
        .def_property("configuration", [](const MessageSendParams& v) { return v.configuration; },
            [](MessageSendParams& v, std::optional<MessageSendConfiguration> value) { v.configuration = std::move(value); })
        .def_property("metadata", [](const MessageSendParams& v) { return json_to_py(v.metadata); },
            [](MessageSendParams& v, py::object value) { v.metadata = py_to_json(value); });
    py::class_<TaskStatusUpdateEvent>(a2a, "TaskStatusUpdateEvent")
        .def_readonly("task_id", &TaskStatusUpdateEvent::task_id)
        .def_readonly("context_id", &TaskStatusUpdateEvent::context_id)
        .def_readonly("final", &TaskStatusUpdateEvent::final)
        .def_property_readonly("status", [](const TaskStatusUpdateEvent& v) { return v.status; })
        .def_property_readonly("metadata", [](const TaskStatusUpdateEvent& v) { return json_to_py(v.metadata); });
    py::class_<TaskArtifactUpdateEvent>(a2a, "TaskArtifactUpdateEvent")
        .def_readonly("task_id", &TaskArtifactUpdateEvent::task_id)
        .def_readonly("context_id", &TaskArtifactUpdateEvent::context_id)
        .def_readonly("append", &TaskArtifactUpdateEvent::append)
        .def_readonly("last_chunk", &TaskArtifactUpdateEvent::last_chunk)
        .def_property_readonly("artifact", [](const TaskArtifactUpdateEvent& v) { return v.artifact; })
        .def_property_readonly("metadata", [](const TaskArtifactUpdateEvent& v) { return json_to_py(v.metadata); });
    auto stream_event = py::class_<StreamEvent>(a2a, "StreamEvent", "Owned native SSE event snapshot.")
        .def_readonly("type", &StreamEvent::type)
        .def_property_readonly("status_update", [](const StreamEvent& v) { return v.status_update; })
        .def_property_readonly("artifact_update", [](const StreamEvent& v) { return v.artifact_update; })
        .def_property_readonly("task", [](const StreamEvent& v) { return v.task; })
        .def("is_final", &StreamEvent::is_final);
    py::enum_<StreamEvent::Type>(stream_event, "Type")
        .value("StatusUpdate", StreamEvent::Type::StatusUpdate)
        .value("ArtifactUpdate", StreamEvent::Type::ArtifactUpdate)
        .value("Task", StreamEvent::Type::Task);
    py::class_<AgentInterface>(a2a, "AgentInterface")
        .def(py::init<>())
        .def_readwrite("url", &AgentInterface::url)
        .def_readwrite("protocol_binding", &AgentInterface::protocol_binding)
        .def_readwrite("protocol_version", &AgentInterface::protocol_version)
        .def_readwrite("tenant", &AgentInterface::tenant);
    py::class_<AgentCard>(a2a, "AgentCard", "Discovery observation; client caches its independent native copy.")
        .def(py::init<>())
        .def_readonly("name", &AgentCard::name)
        .def_readonly("description", &AgentCard::description)
        .def_readonly("url", &AgentCard::url)
        .def_readonly("version", &AgentCard::version)
        .def_readonly("protocol_version", &AgentCard::protocol_version)
        .def_readonly("preferred_transport", &AgentCard::preferred_transport)
        .def_readonly("default_input_modes", &AgentCard::default_input_modes)
        .def_readonly("default_output_modes", &AgentCard::default_output_modes)
        .def_readonly("streaming", &AgentCard::streaming)
        .def_readonly("push_notifications", &AgentCard::push_notifications)
        .def_readonly("extended_card", &AgentCard::extended_card)
        .def_readonly("supports_authenticated_extended", &AgentCard::supports_authenticated_extended)
        .def_property_readonly("supported_interfaces", [](const AgentCard& v) { return v.supported_interfaces; })
        .def_readonly("skills", &AgentCard::skill_names)
        .def_property_readonly("raw", [](const AgentCard& v) { return json_to_py(v.raw); })
        .def("__repr__", [](const AgentCard& v) {
            return "<a2a.AgentCard name=" + v.name + " url=" + v.url + " skills=[" + std::to_string(v.skill_names.size()) + "]>";
        });
    py::class_<A2AClient, std::shared_ptr<A2AClient>>(a2a, "A2AClient",
        "Thread-safe native A2A client; discovery and successful probes select the actual wire dialect.")
        .def(py::init<std::string>(), py::arg("base_url"))
        .def("set_timeout", [](A2AClient& self, double seconds) {
            if (!std::isfinite(seconds) || seconds < std::numeric_limits<int>::min() || seconds > std::numeric_limits<int>::max())
                throw py::value_error("seconds must fit the whole-second timeout range");
            self.set_timeout(std::chrono::seconds(static_cast<int>(seconds)));
        }, py::arg("seconds"))
        .def("wire_dialect", &A2AClient::wire_dialect,
            "None until the first card-selected RPC or successful probe; forced discovery resets selection.")
        .def("set_authorization_header", &A2AClient::set_authorization_header, py::arg("authorization_header"))
        .def("fetch_agent_card", [](A2AClient& self, bool force) {
            py::gil_scoped_release release;
            return self.fetch_agent_card(force);
        }, py::arg("force") = false)
        .def("send_message", [](A2AClient& self, const std::string& text, const std::string& task_id, const std::string& context_id) {
            py::gil_scoped_release release;
            return self.send_message_sync(text, task_id, context_id);
        }, py::arg("text"), py::arg("task_id") = "", py::arg("context_id") = "")
        .def("send_message", [](A2AClient& self, const MessageSendParams& params) {
            py::gil_scoped_release release;
            return self.send_message_sync(params);
        }, py::arg("params"))
        .def("send_message_stream", [](A2AClient& self, const std::string& text, py::function callback,
                                       const std::string& task_id, const std::string& context_id) {
            auto observer = a2a_observer(std::move(callback));
            py::gil_scoped_release release;
            return self.send_message_stream(text, std::move(observer), task_id, context_id);
        }, py::arg("text"), py::arg("on_event"), py::arg("task_id") = "", py::arg("context_id") = "")
        .def("send_message_stream", [](A2AClient& self, const MessageSendParams& params, py::function callback) {
            auto observer = a2a_observer(std::move(callback));
            py::gil_scoped_release release;
            return self.send_message_stream(params, std::move(observer));
        }, py::arg("params"), py::arg("on_event"))
        .def("get_task", [](A2AClient& self, const std::string& id, int history_length) {
            py::gil_scoped_release release;
            return self.get_task(id, history_length);
        }, py::arg("task_id"), py::arg("history_length") = 0)
        .def("cancel_task", [](A2AClient& self, const std::string& id) {
            py::gil_scoped_release release;
            return self.cancel_task(id);
        }, py::arg("task_id"))
        .def_property_readonly("base_url", &A2AClient::base_url);
}

} // namespace neograph::pybind
