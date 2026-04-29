// A2A (Agent-to-Agent protocol) — Python bindings.
//
// v0.2.1 surface:
//   - neograph_engine.a2a.AgentCard  — discovery payload (read-only data class).
//   - neograph_engine.a2a.Task       — a2a Task with status + history.
//   - neograph_engine.a2a.Message    — one user-or-agent turn.
//   - neograph_engine.a2a.Part       — text/file/data fragment.
//   - neograph_engine.a2a.TaskState  — submitted/working/completed/...
//   - neograph_engine.a2a.Role       — user / agent.
//   - neograph_engine.a2a.A2AClient  — JSON-RPC client over Streamable HTTP.
//
// Deferred (planned for v0.3):
//   - A2AServer  (graph-as-A2A-endpoint)
//     Needs lifecycle / signal handling that doesn't mix well with
//     CPython's signal handler — designing a clean GIL-aware shutdown
//     contract is a follow-up.
//   - send_message_stream  (SSE consumer)
//     Wants either a Python callback that fires from the asio thread
//     (GIL juggling) or an async-iterator bridge — also follow-up.

#include <neograph/a2a/client.h>
#include <neograph/a2a/types.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/chrono.h>

#include <chrono>
#include <memory>
#include <string>

namespace py = pybind11;

namespace neograph::pybind {

void init_a2a(py::module_& m) {
    auto a2a = m.def_submodule("a2a",
        "Agent-to-Agent protocol — call remote agents via JSON-RPC over "
        "Streamable HTTP. AgentCard discovery, message/send, tasks/get, "
        "tasks/cancel. Spec: https://a2a-protocol.org/");

    // ── TaskState ─────────────────────────────────────────────────────
    py::enum_<neograph::a2a::TaskState>(a2a, "TaskState",
        "Lifecycle state of a Task. Spec § 6.3 (kebab-case strings on the wire).")
        .value("Submitted",     neograph::a2a::TaskState::Submitted)
        .value("Working",       neograph::a2a::TaskState::Working)
        .value("InputRequired", neograph::a2a::TaskState::InputRequired)
        .value("Completed",     neograph::a2a::TaskState::Completed)
        .value("Canceled",      neograph::a2a::TaskState::Canceled)
        .value("Failed",        neograph::a2a::TaskState::Failed)
        .value("Rejected",      neograph::a2a::TaskState::Rejected)
        .value("AuthRequired",  neograph::a2a::TaskState::AuthRequired)
        .value("Unknown",       neograph::a2a::TaskState::Unknown);

    // ── Role ───────────────────────────────────────────────────────────
    py::enum_<neograph::a2a::Role>(a2a, "Role",
        "Sender of a Message: user (the calling agent) or agent (this agent).")
        .value("User",  neograph::a2a::Role::User)
        .value("Agent", neograph::a2a::Role::Agent);

    // ── Part ───────────────────────────────────────────────────────────
    py::class_<neograph::a2a::Part>(a2a, "Part",
        "One content fragment of a Message or Artifact. `kind` is "
        "'text', 'file', or 'data'.")
        .def(py::init<>())
        .def_static("text_part", &neograph::a2a::Part::text_part,
            py::arg("text"),
            "Build a TextPart with the given content.")
        .def_readwrite("kind", &neograph::a2a::Part::kind)
        .def_readwrite("text", &neograph::a2a::Part::text)
        .def("__repr__", [](const neograph::a2a::Part& p) {
            std::string preview = p.text.size() > 40
                ? p.text.substr(0, 40) + "…"
                : p.text;
            return "<a2a.Part kind=" + p.kind +
                   (p.kind == "text" ? " text=\"" + preview + "\"" : "") + ">";
        });

    // ── Message ────────────────────────────────────────────────────────
    py::class_<neograph::a2a::Message>(a2a, "Message",
        "A single user-or-agent turn. `parts` is a list of Part fragments.")
        .def(py::init<>())
        .def_readwrite("message_id", &neograph::a2a::Message::message_id)
        .def_readwrite("role",       &neograph::a2a::Message::role)
        .def_readwrite("parts",      &neograph::a2a::Message::parts)
        .def_property("task_id",
            [](const neograph::a2a::Message& m) -> std::string {
                return m.task_id.value_or("");
            },
            [](neograph::a2a::Message& m, const std::string& v) {
                if (v.empty()) m.task_id.reset();
                else m.task_id = v;
            })
        .def_property("context_id",
            [](const neograph::a2a::Message& m) -> std::string {
                return m.context_id.value_or("");
            },
            [](neograph::a2a::Message& m, const std::string& v) {
                if (v.empty()) m.context_id.reset();
                else m.context_id = v;
            });

    // ── Task ───────────────────────────────────────────────────────────
    py::class_<neograph::a2a::Task>(a2a, "Task",
        "A unit of work tracked by the agent. "
        "`status.state` is the most useful field; `history` lists every "
        "Message exchanged so far.")
        .def(py::init<>())
        .def_readwrite("id",         &neograph::a2a::Task::id)
        .def_readwrite("context_id", &neograph::a2a::Task::context_id)
        .def_property_readonly("state",
            [](const neograph::a2a::Task& t) { return t.status.state; },
            "Convenience: the TaskState (same as `task.status.state`).")
        .def_readwrite("history",    &neograph::a2a::Task::history)
        .def("__repr__", [](const neograph::a2a::Task& t) {
            return "<a2a.Task id=" + t.id +
                   " state=" + neograph::a2a::task_state_to_string(t.status.state) +
                   ">";
        });

    // ── AgentCard ──────────────────────────────────────────────────────
    py::class_<neograph::a2a::AgentCard>(a2a, "AgentCard",
        "Discovery payload returned from /.well-known/agent-card.json. "
        "Names the agent, its endpoint, capabilities, and skills.")
        .def(py::init<>())
        .def_readonly("name",                &neograph::a2a::AgentCard::name)
        .def_readonly("description",         &neograph::a2a::AgentCard::description)
        .def_readonly("url",                 &neograph::a2a::AgentCard::url)
        .def_readonly("version",             &neograph::a2a::AgentCard::version)
        .def_readonly("protocol_version",    &neograph::a2a::AgentCard::protocol_version)
        .def_readonly("preferred_transport", &neograph::a2a::AgentCard::preferred_transport)
        .def_readonly("default_input_modes", &neograph::a2a::AgentCard::default_input_modes)
        .def_readonly("default_output_modes",&neograph::a2a::AgentCard::default_output_modes)
        .def_readonly("streaming",           &neograph::a2a::AgentCard::streaming)
        .def_readonly("push_notifications",  &neograph::a2a::AgentCard::push_notifications)
        .def_readonly("skills",              &neograph::a2a::AgentCard::skill_names)
        .def("__repr__", [](const neograph::a2a::AgentCard& c) {
            return "<a2a.AgentCard name=" + c.name +
                   " url=" + c.url +
                   " skills=[" + std::to_string(c.skill_names.size()) + "]>";
        });

    // ── A2AClient ──────────────────────────────────────────────────────
    py::class_<neograph::a2a::A2AClient,
               std::shared_ptr<neograph::a2a::A2AClient>>(a2a, "A2AClient",
        "Client for an A2A endpoint. Each method opens an ephemeral HTTP "
        "connection — the client is thread-safe with no shared session "
        "state.")
        .def(py::init<std::string>(), py::arg("base_url"),
            "Construct a client against an agent's base URL "
            "(e.g. 'http://agent.example.com'). The /.well-known card "
            "path is appended automatically on discovery.")
        .def("set_timeout",
            [](neograph::a2a::A2AClient& self, double seconds) {
                self.set_timeout(std::chrono::seconds(static_cast<int>(seconds)));
            },
            py::arg("seconds"),
            "Override the default 30 s per-request timeout.")
        .def("fetch_agent_card",
            [](neograph::a2a::A2AClient& self, bool force) {
                py::gil_scoped_release release;
                return self.fetch_agent_card(force);
            },
            py::arg("force") = false,
            "GET /.well-known/agent-card.json. Cached after first call "
            "unless `force=True`.")
        .def("send_message",
            [](neograph::a2a::A2AClient& self,
               const std::string& text,
               const std::string& task_id,
               const std::string& context_id) {
                py::gil_scoped_release release;
                return self.send_message_sync(text, task_id, context_id);
            },
            py::arg("text"),
            py::arg("task_id")    = "",
            py::arg("context_id") = "",
            "Send `text` as a single TextPart; returns the resulting "
            "Task. Pass `task_id`/`context_id` to continue an existing "
            "conversation.")
        .def("get_task",
            [](neograph::a2a::A2AClient& self,
               const std::string& task_id, int history_length) {
                py::gil_scoped_release release;
                return self.get_task(task_id, history_length);
            },
            py::arg("task_id"),
            py::arg("history_length") = 0,
            "Fetch a snapshot of an existing task by id.")
        .def("cancel_task",
            [](neograph::a2a::A2AClient& self, const std::string& task_id) {
                py::gil_scoped_release release;
                return self.cancel_task(task_id);
            },
            py::arg("task_id"),
            "Request cancellation of a running task.")
        .def_property_readonly("base_url",
            &neograph::a2a::A2AClient::base_url);
}

} // namespace neograph::pybind
