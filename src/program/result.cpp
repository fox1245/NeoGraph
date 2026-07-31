#include <neograph/program/result.h>

#include <utility>

namespace neograph::program {

struct ProgramResult::Impl {
    explicit Impl(const ConstructionData& value)
        : status(value.status),
          run_id(value.run_id),
          program_version_id(value.program_version_id),
          bundle_id(value.bundle_id),
          operation_id(value.operation_id),
          attempt(value.attempt),
          output(value.output),
          usage(value.usage),
          remaining_budget(value.remaining_budget),
          checkpoint(value.checkpoint),
          interrupt(value.interrupt),
          failure(value.failure),
          execution_trace(value.execution_trace) {}

    ProgramTerminalStatus                 status;
    std::string                           run_id;
    std::string                           program_version_id;
    std::string                           bundle_id;
    std::string                           operation_id;
    std::uint64_t                         attempt;
    json                                  output;
    ProgramUsage                          usage;
    RunBudget                             remaining_budget;
    std::optional<CoreCheckpointIdentity> checkpoint;
    std::optional<ProgramInterrupt>       interrupt;
    std::optional<ProgramFailure>         failure;
    std::vector<std::string>              execution_trace;
};

ProgramResult::ProgramResult()
    : ProgramResult(ConstructionData{
          ProgramTerminalStatus::Failed,
          "",
          "",
          "",
          "root",
          0,
          json::object(),
          {},
          {},
          std::nullopt,
          std::nullopt,
          ProgramFailure{"P_RESULT_EMPTY", "Empty Program result", "root", "", 0, json::object()},
          {}}) {}

ProgramResult::ProgramResult(ConstructionData data) : impl_(std::make_shared<const Impl>(data)) {}

ProgramTerminalStatus ProgramResult::status() const noexcept {
    return impl_->status;
}

const std::string& ProgramResult::run_id() const noexcept {
    return impl_->run_id;
}

const std::string& ProgramResult::program_version_id() const noexcept {
    return impl_->program_version_id;
}

const std::string& ProgramResult::bundle_id() const noexcept {
    return impl_->bundle_id;
}

const std::string& ProgramResult::operation_id() const noexcept {
    return impl_->operation_id;
}

std::uint64_t ProgramResult::attempt() const noexcept {
    return impl_->attempt;
}

json ProgramResult::output() const {
    return impl_->output;
}

ProgramUsage ProgramResult::usage() const noexcept {
    return impl_->usage;
}

RunBudget ProgramResult::remaining_budget() const noexcept {
    return impl_->remaining_budget;
}

std::optional<CoreCheckpointIdentity> ProgramResult::checkpoint() const {
    return impl_->checkpoint;
}

std::optional<ProgramInterrupt> ProgramResult::interrupt() const {
    return impl_->interrupt;
}

std::optional<ProgramFailure> ProgramResult::failure() const {
    return impl_->failure;
}

std::vector<std::string> ProgramResult::execution_trace() const {
    return impl_->execution_trace;
}

}  // namespace neograph::program
