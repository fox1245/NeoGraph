#pragma once

// A Python exception raised by a node body, carried through the engine as a
// typed node failure (#364). The node failure classifier sees an ordinary
// failure with the author code the exception declared; Python callers still
// receive the original exception object, because provider_cause() unwraps
// python_error().

#include <neograph/graph/types.h>

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace neograph::pybind {

class PythonNodeFailure : public graph::NodeFailureError {
public:
    /// `what` is the Python error's text, so NodeExecutionError messages do
    /// not change. `code` is empty or an author code.
    PythonNodeFailure(std::string what, std::string code, std::string public_message,
                      std::optional<bool> retryable, std::exception_ptr python_error)
        : NodeFailureError(std::move(what), std::move(code), std::move(public_message), retryable),
          python_error_(std::move(python_error)) {}

    /// The captured pybind11::error_already_set.
    const std::exception_ptr& python_error() const noexcept { return python_error_; }

private:
    std::exception_ptr python_error_;
};

/// A Program diagnostic raised by a Python node body (ProgramDiagnosticError,
/// ProgramCompileError or ProgramAdmissionError): a configuration verdict,
/// NG.ConfigError for the classifier, like the C++ exception (#364).
class PythonNodeConfigurationFailure final : public PythonNodeFailure,
                                             public graph::ConfigurationFailure {
public:
    using PythonNodeFailure::PythonNodeFailure;
};

/// A Python exception raised by a tool body, as the failure the tool
/// dispatcher reports (#545). what() is the text the model reads: the exception
/// type and message, never the traceback, whose file paths and frames are the
/// host's. The traceback goes to the `neograph_engine.tools` logger.
class PythonToolFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace neograph::pybind
