#pragma once

#include <neograph/json.h>
#include <neograph/program/source.h>

#include <stdexcept>
#include <string>

namespace neograph::program {
struct JavaScriptCompileLimits;
}

namespace neograph::program::detail {

class JavaScriptCompileError final : public std::runtime_error {
public:
    JavaScriptCompileError(std::string code, std::string message, json witness = json::object());

    const std::string& code() const noexcept;
    const json&        witness() const noexcept;

private:
    std::string code_;
    json        witness_;
};

// Evaluates a sealed SourceKind::JavaScript source in the private QuickJS
// compiler sandbox and returns the single document submitted through
// neograph.define(). It never exposes a VM, bytecode, or host capability to
// the Program runtime.
json evaluate_javascript_source(const ProgramSource& source, const JavaScriptCompileLimits& limits);

}  // namespace neograph::program::detail
