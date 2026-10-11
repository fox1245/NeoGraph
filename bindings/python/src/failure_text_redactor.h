#pragma once

// The #364 failure-text redactor for Python hosts: a Python callable that
// receives a failed node's structured record and decides the free text the
// failure record keeps. Used by GraphEngine.compile and LocalProgramHost.

#include <neograph/graph/types.h>

#include <pybind11/pybind11.h>

#include <memory>
#include <string>
#include <utility>

namespace neograph::pybind {

// Called with a dict {"node", "code", "kind", "attempts", "retryable",
// "public_message"}; returns the text to keep (a str), or None or "" for no
// text. An exception in the callable keeps no text. None wraps to no redactor.
inline graph::FailureTextRedactor python_failure_text_redactor(pybind11::object callable) {
    namespace py = pybind11;
    if (callable.is_none()) return {};
    auto held = std::shared_ptr<py::object>(new py::object(std::move(callable)), [](py::object* p) {
        py::gil_scoped_acquire gil;
        delete p;
    });
    return [held](const graph::NodeFailure& failure) -> std::string {
        py::gil_scoped_acquire gil;
        try {
            py::dict record;
            record["node"]           = failure.node;
            record["code"]           = failure.code;
            record["kind"]           = std::string(graph::to_string(failure.kind));
            record["attempts"]       = failure.attempts;
            record["retryable"]      = failure.retryable;
            record["public_message"] = failure.public_message;
            py::object text = (*held)(std::move(record));
            if (py::isinstance<py::str>(text)) return text.cast<std::string>();
        } catch (const py::error_already_set&) {
            PyErr_Clear();
        } catch (const std::exception&) {
        }
        return {};
    };
}

}  // namespace neograph::pybind
