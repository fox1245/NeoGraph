// Provider-neutral generated-artifact value bindings.
//
// The C++ ArtifactProvider contract is async-native. These bindings expose the
// durable request, result, event, and cancellation values that graph-facing
// Python integrations need, while a concrete C++ adapter remains responsible
// for transport ownership and its executor lifetime.
//
// pybind11 v2.13.6 virtual-class docs: a Python-subclassable pure virtual
// interface needs a trampoline for every virtual. ArtifactProvider's protected
// async primitives have no safe synchronous Python override, so expose native
// adapters only rather than publishing a callback path that blocks an executor.
// Source: https://raw.githubusercontent.com/pybind/pybind11/v2.13.6/docs/advanced/classes.rst
// Checked 2026-08-04.

#include "json_bridge.h"

#include <neograph/artifact_provider.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>
#include <memory>
#include <utility>

namespace py = pybind11;

namespace neograph::pybind {

void init_artifact(py::module_& m) {
    py::enum_<ArtifactExecutionMode>(m, "ArtifactExecutionMode",
        "How a generated-artifact provider delivers its result.")
        .value("OneShot", ArtifactExecutionMode::OneShot)
        .value("Stream", ArtifactExecutionMode::Stream)
        .value("LongRunning", ArtifactExecutionMode::LongRunning);

    py::enum_<ArtifactOperationStatus>(m, "ArtifactOperationStatus",
        "Provider-neutral lifecycle state of a generated-artifact operation.")
        .value("Queued", ArtifactOperationStatus::Queued)
        .value("Running", ArtifactOperationStatus::Running)
        .value("Succeeded", ArtifactOperationStatus::Succeeded)
        .value("Failed", ArtifactOperationStatus::Failed)
        .value("Cancelled", ArtifactOperationStatus::Cancelled)
        .value("TimedOut", ArtifactOperationStatus::TimedOut);

    py::enum_<ArtifactEventKind>(m, "ArtifactEventKind",
        "Incremental event kind emitted by a streaming artifact provider.")
        .value("Progress", ArtifactEventKind::Progress)
        .value("Artifact", ArtifactEventKind::Artifact)
        .value("State", ArtifactEventKind::State);

    py::class_<Artifact>(m, "Artifact",
        "Durable reference to generated content. Bytes remain at `uri`.")
        .def(py::init<>())
        .def(py::init([](std::string identity, std::string uri,
                         std::string media_type, std::uint64_t size_bytes,
                         py::object metadata) {
            Artifact artifact;
            artifact.identity = std::move(identity);
            artifact.uri = std::move(uri);
            artifact.media_type = std::move(media_type);
            artifact.size_bytes = size_bytes;
            artifact.metadata = py_to_json(metadata);
            return artifact;
        }),
             py::arg("identity") = "", py::arg("uri") = "",
             py::arg("media_type") = "", py::arg("size_bytes") = 0,
             py::arg("metadata") = py::dict())
        .def_readwrite("identity", &Artifact::identity)
        .def_readwrite("uri", &Artifact::uri)
        .def_readwrite("media_type", &Artifact::media_type)
        .def_readwrite("size_bytes", &Artifact::size_bytes)
        .def_property("metadata",
            [](const Artifact& artifact) { return json_to_py(artifact.metadata); },
            [](Artifact& artifact, py::object value) { artifact.metadata = py_to_json(value); });

    py::class_<ArtifactEvent>(m, "ArtifactEvent",
        "One ordered streaming artifact event.")
        .def(py::init<>())
        .def_readwrite("kind", &ArtifactEvent::kind)
        .def_readwrite("operation_id", &ArtifactEvent::operation_id)
        .def_readwrite("sequence", &ArtifactEvent::sequence)
        .def_readwrite("artifact", &ArtifactEvent::artifact)
        .def_property("data",
            [](const ArtifactEvent& event) { return json_to_py(event.data); },
            [](ArtifactEvent& event, py::object value) { event.data = py_to_json(value); });

    py::class_<ArtifactPollPolicy>(m, "ArtifactPollPolicy",
        "Bounded polling policy for one long-running artifact request.")
        .def(py::init<>())
        .def(py::init([](std::uint32_t max_attempts, std::int64_t interval_ms) {
            ArtifactPollPolicy policy;
            policy.max_attempts = max_attempts;
            policy.interval = std::chrono::milliseconds(interval_ms);
            return policy;
        }), py::arg("max_attempts") = 60, py::arg("interval_ms") = 1000)
        .def_readwrite("max_attempts", &ArtifactPollPolicy::max_attempts)
        .def_property("interval_ms",
            [](const ArtifactPollPolicy& policy) { return policy.interval.count(); },
            [](ArtifactPollPolicy& policy, std::int64_t milliseconds) {
                policy.interval = std::chrono::milliseconds(milliseconds);
            });

    py::class_<ArtifactRequest>(m, "ArtifactRequest",
        "Owned request envelope for a one-shot, streaming, or long-running artifact operation.")
        .def(py::init<>())
        .def(py::init([](std::string operation, py::object input,
                         ArtifactExecutionMode mode, std::string idempotency_key,
                         ArtifactPollPolicy poll) {
            ArtifactRequest request;
            request.operation = std::move(operation);
            request.input = py_to_json(input);
            request.mode = mode;
            request.idempotency_key = std::move(idempotency_key);
            request.poll = std::move(poll);
            return request;
        }),
             py::arg("operation") = "", py::arg("input") = py::dict(),
             py::arg("mode") = ArtifactExecutionMode::OneShot,
             py::arg("idempotency_key") = "", py::arg("poll") = ArtifactPollPolicy{})
        .def_readwrite("operation", &ArtifactRequest::operation)
        .def_property("input",
            [](const ArtifactRequest& request) { return json_to_py(request.input); },
            [](ArtifactRequest& request, py::object value) { request.input = py_to_json(value); })
        .def_readwrite("idempotency_key", &ArtifactRequest::idempotency_key)
        .def_readwrite("mode", &ArtifactRequest::mode)
        .def_readwrite("poll", &ArtifactRequest::poll)
        .def_readwrite("cancel_token", &ArtifactRequest::cancel_token);

    py::class_<ArtifactOperation>(m, "ArtifactOperation",
        "Latest or terminal result of a generated-artifact operation.")
        .def(py::init<>())
        .def_readwrite("id", &ArtifactOperation::id)
        .def_readwrite("status", &ArtifactOperation::status)
        .def_readwrite("artifacts", &ArtifactOperation::artifacts)
        .def_property("output",
            [](const ArtifactOperation& operation) { return json_to_py(operation.output); },
            [](ArtifactOperation& operation, py::object value) { operation.output = py_to_json(value); })
        .def_readwrite("error_code", &ArtifactOperation::error_code)
        .def_readwrite("error_message", &ArtifactOperation::error_message)
        .def("terminal", &ArtifactOperation::terminal)
        .def("succeeded", &ArtifactOperation::succeeded)
        .def("failed", &ArtifactOperation::failed);

    py::class_<ArtifactProvider, std::shared_ptr<ArtifactProvider>>(m, "ArtifactProvider",
        "Async-native C++ artifact-provider interface. Concrete provider adapters own transport; this binding exposes their synchronous bridge when supplied by another NeoGraph component.")
        .def("get_name", &ArtifactProvider::get_name)
        .def("execute", [](ArtifactProvider& provider, ArtifactRequest request) {
            py::gil_scoped_release release;
            return provider.execute(std::move(request));
        }, py::arg("request"));
}

} // namespace neograph::pybind
