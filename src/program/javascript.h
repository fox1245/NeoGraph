#pragma once

#include <neograph/json.h>
#include <neograph/program/command.h>
#include <neograph/program/source.h>

#include <memory>
#include <chrono>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
namespace neograph::program {
struct JavaScriptCompileLimits;
}

namespace neograph::program::detail {

class JavaScriptCompileError final : public std::runtime_error {
public:
    JavaScriptCompileError(std::string code, std::string message, json witness = json::object());
    JavaScriptCompileError(std::string code,
                           std::string message,
                           json        witness,
                           std::optional<SourceSpan> source_span);

    const std::string& code() const noexcept;
    const json&        witness() const noexcept;
    const std::optional<SourceSpan>& source_span() const noexcept;

private:
    std::string code_;
    json        witness_;
    std::optional<SourceSpan> source_span_;
};

struct JavaScriptSourceEvaluation {
    json document;
    bool has_control_generator = false;
};

// Evaluates a sealed SourceKind::JavaScript source in the private QuickJS
// compiler sandbox and returns the single document submitted through
// neograph.define(), plus whether it exports the opt-in main() control entry.
JavaScriptSourceEvaluation evaluate_javascript_source(const ProgramSource&           source,
                                                      const JavaScriptCompileLimits& limits);

struct JavaScriptGeneratorStep {
    bool done = false;
    json value;
    /** Present only when the yielded value is a host-sealed command object. */
    std::optional<JavaScriptCommand> command;
};

/**
 * A bounded, single-threaded JavaScript generator session. It exposes no host
 * capability beyond construction of declarative yielded commands.
 */
class JavaScriptGenerator final {
public:
    static std::optional<JavaScriptGenerator> open(const ProgramSource&           source,
                                                   json                           input,
                                                   const JavaScriptCompileLimits& limits,
                                                   std::function<bool()>          cancellation_requested = {},
                                                   std::optional<std::chrono::steady_clock::time_point>
                                                       deadline = std::nullopt);

    JavaScriptGenerator(JavaScriptGenerator&&) noexcept;
    JavaScriptGenerator& operator=(JavaScriptGenerator&&) noexcept;
    JavaScriptGenerator(const JavaScriptGenerator&)            = delete;
    JavaScriptGenerator& operator=(const JavaScriptGenerator&) = delete;
    ~JavaScriptGenerator();

    JavaScriptGeneratorStep next(std::optional<json> response = std::nullopt);

    struct Impl;

private:
    explicit JavaScriptGenerator(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program::detail
