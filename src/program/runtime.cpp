#include <neograph/program/diagnostic.h>
#include <neograph/program/runtime.h>

#include "canonical_json.h"
#include "catalog_access.h"
#include "run_control.h"
#include <asio/co_spawn.hpp>
#include <asio/dispatch.hpp>
#include <asio/error.hpp>
#include <asio/redirect_error.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Diagnostic runtime_diagnostic(std::string code,
                              std::string message,
                              json        witness = json::object()) {
    Diagnostic diagnostic;
    diagnostic.phase    = CompilePhase::Seal;
    diagnostic.code     = std::move(code);
    diagnostic.severity = DiagnosticSeverity::Error;
    diagnostic.primary  = {"runtime", "", std::nullopt};
    diagnostic.message  = std::move(message);
    diagnostic.witness  = std::move(witness);
    return diagnostic;
}

[[noreturn]] void throw_runtime_diagnostic(std::string code,
                                           std::string message,
                                           json        witness = json::object()) {
    throw ProgramDiagnosticError(
        runtime_diagnostic(std::move(code), std::move(message), std::move(witness)));
}

std::string generate_run_id() {
    static std::mutex      random_mutex;
    static std::mt19937_64 random(std::random_device{}());
    std::lock_guard        lock(random_mutex);
    static constexpr char  hex[] = "0123456789abcdef";
    std::string            id    = "run-";
    id.reserve(36);
    for (int i = 0; i < 32; ++i)
        id.push_back(hex[random() & 0xf]);
    return id;
}

std::string core_thread_identity(std::string_view run_id, std::string_view generation_id) {
    std::string identity(run_id);
    identity.push_back('\0');
    identity.append("root");
    identity.push_back('\0');
    identity.append(generation_id);
    return detail::sha256_identity("program-core-thread/v1", identity);
}

void complete_attempt_launch_failure(const std::shared_ptr<detail::RunControl>& control,
                                     const std::optional<std::string>&          checkpoint_id,
                                     std::exception_ptr                         error) noexcept {
    std::string message = "Program execution coroutine failed";
    if (error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& exception) {
            message += ": ";
            message += exception.what();
        } catch (...) {
            message += " with a non-standard exception";
        }
    }

    detail::RunOutcome outcome;
    outcome.status                   = ProgramTerminalStatus::Failed;
    outcome.remaining_budget         = control->granted_budget;
    outcome.usage.program_operations = control->attempt == 1 ? 1 : 0;
    if (control->attempt == 1) outcome.remaining_budget.max_program_operations = 0;
    outcome.failure =
        ProgramFailure{"P_RUNTIME_CORE_FAILURE", std::move(message), "root", "", 0, json::object()};
    if (checkpoint_id) {
        outcome.checkpoint = CoreCheckpointIdentity{
            control->materialized->root->core_name,
            control->materialized->root->compiled_plan_identity, control->core_thread_id,
            *checkpoint_id, graph::CHECKPOINT_SCHEMA_VERSION};
    }
    control->complete(std::move(outcome));
}

void spawn_run_attempt(asio::thread_pool&                         pool,
                       const std::shared_ptr<detail::RunControl>& control,
                       json                                       input,
                       std::optional<std::string>                 checkpoint_id) noexcept {
    try {
        asio::co_spawn(pool, detail::execute_run_attempt(control, std::move(input), checkpoint_id),
                       [control, checkpoint_id](std::exception_ptr error) {
                           if (error)
                               complete_attempt_launch_failure(control, checkpoint_id, error);
                       });
    } catch (...) {
        complete_attempt_launch_failure(control, checkpoint_id, std::current_exception());
    }
}
std::uint64_t budget_value(const RunBudget& budget, std::string_view resource) {
    if (resource == "wall_time_ms") return budget.wall_time_ms;
    if (resource == "model_tokens") return budget.model_tokens;
    if (resource == "monetary_microunits") return budget.monetary_microunits;
    if (resource == "max_concurrency") return budget.max_concurrency;
    if (resource == "max_program_operations") return budget.max_program_operations;
    if (resource == "max_core_steps") return budget.max_core_steps;
    if (resource == "max_dynamic_compiles") return budget.max_dynamic_compiles;
    if (resource == "max_child_depth") return budget.max_child_depth;
    if (resource == "max_total_children") return budget.max_total_children;
    throw_runtime_diagnostic("P_START_BUDGET", "Unknown declared budget resource",
                             json{{"resource", std::string(resource)}});
}

std::uint64_t ceiling_value(const BudgetLimits& budget, std::string_view resource) {
    if (resource == "wall_time_ms") return budget.wall_time_ms;
    if (resource == "model_tokens") return budget.model_tokens;
    if (resource == "monetary_microunits") return budget.monetary_microunits;
    if (resource == "max_concurrency") return budget.max_concurrency;
    if (resource == "max_program_operations") return budget.max_program_operations;
    if (resource == "max_core_steps") return budget.max_core_steps;
    if (resource == "max_dynamic_compiles") return budget.max_dynamic_compiles;
    if (resource == "max_child_depth") return budget.max_child_depth;
    if (resource == "max_total_children") return budget.max_total_children;
    return 0;
}

void validate_invocation(const detail::MaterializedProgram& materialized,
                         const ProgramInvocation&           invocation) {
    if (!invocation.input.is_object()) {
        throw_runtime_diagnostic("P_START_INPUT", "Program invocation input must be an object");
    }
    const auto&    budget = invocation.budget;
    constexpr auto max_safe_wall_time_ms =
        static_cast<std::uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max()) / 2;
    if (budget.wall_time_ms == 0 || budget.wall_time_ms > max_safe_wall_time_ms ||
        budget.max_concurrency != 1 || budget.max_program_operations != 1 ||
        budget.max_core_steps == 0 ||
        budget.max_core_steps > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
        budget.max_dynamic_compiles != 0 || budget.max_child_depth != 0 ||
        budget.max_total_children != 0) {
        throw_runtime_diagnostic("P_START_BUDGET",
                                 "PR6 requires positive wall/Core limits, one "
                                 "operation/concurrency slot, and no dynamic or child budget");
    }

    const auto ceiling = materialized.version.policy_snapshot().budget_ceiling();
    for (const auto& requirement : materialized.bundle.declared_budget_requirements()) {
        const auto granted = budget_value(budget, requirement.resource);
        const auto allowed = ceiling_value(ceiling, requirement.resource);
        if (granted < requirement.minimum || granted > requirement.maximum || granted > allowed) {
            throw_runtime_diagnostic("P_START_BUDGET",
                                     "Program invocation budget is outside its admitted bounds",
                                     json{{"resource", requirement.resource},
                                          {"minimum", requirement.minimum},
                                          {"maximum", requirement.maximum},
                                          {"policy_ceiling", allowed},
                                          {"granted", granted}});
        }
    }
}

ProgramJournalRecord initial_record(const detail::RunControl& control) {
    return ProgramJournalRecord::create(ProgramJournalRecordData{
        "", control.run_id, control.program_version_id, control.bundle_id, 1,
        ProgramContinuation{"root", ContinuationState::Running, 1}, control.granted_budget,
        control.granted_budget, std::nullopt, now_ms()});
}

ProgramJournalRecord resumed_record(const detail::RunControl&   control,
                                    const ProgramJournalRecord& previous) {
    return ProgramJournalRecord::create(ProgramJournalRecordData{
        previous.id, control.run_id, control.program_version_id, control.bundle_id,
        previous.sequence + 1,
        ProgramContinuation{"root", ContinuationState::Running, control.attempt},
        previous.remaining_budget, previous.remaining_budget, previous.core_checkpoint, now_ms()});
}

ContinuationState continuation_state(ProgramTerminalStatus status) {
    switch (status) {
        case ProgramTerminalStatus::Completed:
            return ContinuationState::Completed;
        case ProgramTerminalStatus::Interrupted:
            return ContinuationState::Interrupted;
        case ProgramTerminalStatus::Cancelled:
            return ContinuationState::Cancelled;
        case ProgramTerminalStatus::BudgetExhausted:
            return ContinuationState::BudgetExhausted;
        case ProgramTerminalStatus::TimedOut:
            return ContinuationState::TimedOut;
        case ProgramTerminalStatus::CheckpointIncompatible:
            return ContinuationState::CheckpointIncompatible;
        case ProgramTerminalStatus::Failed:
        case ProgramTerminalStatus::AmbiguousEffect:
            return ContinuationState::Failed;
    }
    return ContinuationState::Failed;
}

bool publish_terminal_record(const detail::RunControl& control, const detail::RunOutcome& outcome) {
    for (int retry = 0; retry < 3; ++retry) {
        try {
            const auto previous = control.journal->latest(control.run_id);
            if (!previous || previous->continuation.state != ContinuationState::Running ||
                previous->continuation.attempt != control.attempt) {
                return false;
            }
            auto       record   = ProgramJournalRecord::create(ProgramJournalRecordData{
                previous->id, control.run_id, control.program_version_id, control.bundle_id,
                previous->sequence + 1,
                ProgramContinuation{"root", continuation_state(outcome.status), control.attempt},
                outcome.remaining_budget, RunBudget{}, outcome.checkpoint, now_ms()});
            const auto appended = control.journal->compare_append(previous->id, std::move(record));
            if (appended == JournalAppendResult::Appended ||
                appended == JournalAppendResult::AlreadyPresent) {
                return true;
            }
        } catch (...) {
            return false;
        }
    }
    return false;
}

bool publish_failure_correction(const detail::RunControl& control,
                                const detail::RunOutcome& outcome) {
    for (int retry = 0; retry < 3; ++retry) {
        try {
            const auto previous = control.journal->latest(control.run_id);
            if (!previous || previous->continuation.state == ContinuationState::Running ||
                previous->continuation.attempt != control.attempt) {
                return false;
            }
            auto       record   = ProgramJournalRecord::create(ProgramJournalRecordData{
                previous->id, control.run_id, control.program_version_id, control.bundle_id,
                previous->sequence + 1,
                ProgramContinuation{"root", ContinuationState::Failed, control.attempt},
                outcome.remaining_budget, RunBudget{}, outcome.checkpoint, now_ms()});
            const auto appended = control.journal->compare_append(previous->id, std::move(record));
            if (appended == JournalAppendResult::Appended ||
                appended == JournalAppendResult::AlreadyPresent) {
                return true;
            }
        } catch (...) {
            return false;
        }
    }
    return false;
}

}  // namespace

namespace detail {

RunControl::RunControl(std::string                                run,
                       std::uint64_t                              attempt_value,
                       std::shared_ptr<const MaterializedProgram> pinned,
                       RunBudget                                  budget,
                       std::string                                thread,
                       std::string                                trace,
                       std::shared_ptr<ProgramEventSink>          sink,
                       asio::any_io_executor                      deadline_executor_value,
                       std::shared_ptr<graph::CheckpointStore>    checkpoint_store,
                       std::shared_ptr<graph::Store>              store,
                       std::shared_ptr<ProgramJournal>            program_journal)
    : run_id(std::move(run)),
      program_version_id(pinned->version.id()),
      bundle_id(pinned->bundle.id()),
      attempt(attempt_value),
      materialized(std::move(pinned)),
      granted_budget(budget),
      started_at(std::chrono::steady_clock::now()),
      deadline(started_at + std::chrono::milliseconds(budget.wall_time_ms)),
      deadline_executor(std::move(deadline_executor_value)),
      waiter_strand(asio::make_strand(asio::system_executor{})),
      core_thread_id(std::move(thread)),
      trace_id(std::move(trace)),
      checkpoints(std::move(checkpoint_store)),
      state_store(std::move(store)),
      journal(std::move(program_journal)),
      cancel_token(std::make_shared<graph::CancelToken>()),
      sink_(std::move(sink)) {}

bool RunControl::cancel(CancellationCause cause) noexcept {
    {
        std::lock_guard lock(mutex_);
        if (result_ || terminal_decided_ || cancellation_cause_ != CancellationCause::None)
            return false;
        cancellation_cause_ = cause;
    }
    cancel_token->cancel();
    return true;
}

CancellationCause RunControl::cancellation_cause() const noexcept {
    std::lock_guard lock(mutex_);
    return cancellation_cause_;
}

CancellationCause RunControl::seal_terminal_cause() noexcept {
    std::lock_guard lock(mutex_);
    terminal_decided_ = true;
    return cancellation_cause_;
}

ProgramEvent RunControl::make_event(ProgramEventKind kind, ProgramEventPayload payload) {
    ProgramEvent event;
    event.sequence           = next_sequence_++;
    event.timestamp_ms       = now_ms();
    event.run_id             = run_id;
    event.program_version_id = program_version_id;
    event.bundle_id          = bundle_id;
    event.operation_id       = operation_id;
    event.core_generation_id = materialized->root->compiled_plan_identity;
    event.core_run_id        = core_thread_id;
    event.trace_id           = trace_id;
    event.attempt            = attempt;
    event.kind               = kind;
    event.payload            = std::move(payload);
    return event;
}

void RunControl::emit(ProgramEventKind kind, ProgramEventPayload payload) {
    std::shared_ptr<ProgramEventSink> sink;
    ProgramEvent                      event;
    {
        std::lock_guard lock(mutex_);
        if (result_) return;
        event = make_event(kind, std::move(payload));
        events_.push_back(event);
        sink = sink_;
    }
    if (!sink) return;
    try {
        sink->on_event(event);
    } catch (const std::exception& error) {
        {
            std::lock_guard lock(mutex_);
            if (sink_ == sink) sink_.reset();
        }
        cancel(CancellationCause::EventSink);
        throw EventSinkError(error.what());
    } catch (...) {
        {
            std::lock_guard lock(mutex_);
            if (sink_ == sink) sink_.reset();
        }
        cancel(CancellationCause::EventSink);
        throw EventSinkError("Program event sink threw");
    }
}

ProgramResult RunControl::make_result(RunOutcome outcome) const {
    return ProgramResult(ProgramResult::ConstructionData{
        outcome.status, run_id, program_version_id, bundle_id, operation_id, attempt,
        std::move(outcome.output), outcome.usage, outcome.remaining_budget,
        std::move(outcome.checkpoint), std::move(outcome.interrupt), std::move(outcome.failure),
        std::move(outcome.execution_trace)});
}

void RunControl::complete(RunOutcome outcome) noexcept {
    try {
        seal_terminal_cause();
        {
            std::lock_guard lock(mutex_);
            if (result_) return;
        }

        bool journal_published = publish_terminal_record(*this, outcome);
        if (!journal_published) {
            outcome.status  = ProgramTerminalStatus::Failed;
            outcome.failure = ProgramFailure{
                "P_JOURNAL_CONFLICT", "Program terminal journal CAS failed", "root", "", 0,
                json::object()};
            outcome.checkpoint.reset();
        } else if (outcome.checkpoint) {
            std::lock_guard lock(mutex_);
            latest_checkpoint_ = outcome.checkpoint;
        }

        const auto fail_event_delivery = [&](std::string message) {
            const auto prior_status  = outcome.status;
            const auto prior_failure = outcome.failure;
            outcome.status           = ProgramTerminalStatus::Failed;
            outcome.failure =
                ProgramFailure{"P_EVENT_SINK", std::move(message), "root", "", 0, json::object()};
            if (journal_published && prior_status != ProgramTerminalStatus::Failed &&
                !publish_failure_correction(*this, outcome)) {
                outcome.status  = prior_status;
                outcome.failure = prior_failure;
            }
        };

        if (journal_published && outcome.checkpoint) {
            try {
                emit(ProgramEventKind::CheckpointPublished,
                     ProgramCheckpointEvent{*outcome.checkpoint});
            } catch (const EventSinkError& error) {
                fail_event_delivery(error.what());
            }
        }

        if (journal_published) {
            try {
                emit(ProgramEventKind::Terminal, ProgramTerminalEvent{outcome.status});
            } catch (const EventSinkError& error) {
                fail_event_delivery(error.what());
                std::lock_guard lock(mutex_);
                if (!events_.empty() && events_.back().kind == ProgramEventKind::Terminal) {
                    events_.back().payload = ProgramTerminalEvent{outcome.status};
                }
            }
        }
        {
            std::lock_guard lock(mutex_);
            sink_.reset();
        }

        auto                     result = make_result(std::move(outcome));
        std::vector<AsyncWaiter> waiters;
        {
            std::lock_guard lock(mutex_);
            if (result_) return;
            result_ = std::move(result);
            waiters.swap(waiters_);
            cv_.notify_all();
        }
        asio::dispatch(waiter_strand, [waiters = std::move(waiters)] {
            for (const auto& waiter : waiters) {
                if (auto timer = waiter.timer.lock()) timer->cancel();
            }
        });
    } catch (...) {
        std::terminate();
    }
}

ProgramResult RunControl::wait() const {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return result_.has_value(); });
    return *result_;
}

asio::awaitable<ProgramResult> RunControl::wait_async() const {
    auto timer = std::make_shared<asio::steady_timer>(waiter_strand);
    timer->expires_at((asio::steady_timer::time_point::max)());
    co_await asio::co_spawn(
        waiter_strand,
        [this, timer]() -> asio::awaitable<void> {
            {
                std::lock_guard lock(mutex_);
                if (result_) co_return;
                waiters_.push_back(AsyncWaiter{timer});
            }
            asio::error_code error;
            co_await         timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
        },
        asio::use_awaitable);

    std::lock_guard lock(mutex_);
    if (!result_) throw std::runtime_error("Program wait ended before terminal publication");
    co_return* result_;
}

std::optional<ProgramResult> RunControl::try_result() const {
    std::lock_guard lock(mutex_);
    return result_;
}

std::vector<ProgramEvent> RunControl::events_after(std::uint64_t sequence) const {
    std::lock_guard           lock(mutex_);
    std::vector<ProgramEvent> result;
    const auto                first = std::upper_bound(
        events_.begin(), events_.end(), sequence,
        [](std::uint64_t value, const ProgramEvent& event) { return value < event.sequence; });
    result.assign(first, events_.end());
    return result;
}

std::optional<CoreCheckpointIdentity> RunControl::latest_checkpoint() const {
    std::lock_guard lock(mutex_);
    return latest_checkpoint_;
}

}  // namespace detail
namespace {

asio::awaitable<ProgramResult> wait_for_control(std::shared_ptr<detail::RunControl> control) {
    co_return co_await control->wait_async();
}

}  // namespace

ProgramHandle::ProgramHandle(std::shared_ptr<detail::RunControl> control)
    : control_(std::move(control)) {}
ProgramHandle::~ProgramHandle() = default;
const std::string& ProgramHandle::run_id() const noexcept {
    return control_->run_id;
}
const std::string& ProgramHandle::program_version_id() const noexcept {
    return control_->program_version_id;
}
std::uint64_t ProgramHandle::attempt() const noexcept {
    return control_->attempt;
}
bool ProgramHandle::cancel() noexcept {
    return control_->cancel(detail::CancellationCause::User);
}
ProgramResult ProgramHandle::wait() const {
    return control_->wait();
}
asio::awaitable<ProgramResult> ProgramHandle::wait_async() const {
    return wait_for_control(control_);
}
std::optional<ProgramResult> ProgramHandle::try_result() const {
    return control_->try_result();
}
std::vector<ProgramEvent> ProgramHandle::events_after(std::uint64_t sequence) const {
    return control_->events_after(sequence);
}
std::optional<CoreCheckpointIdentity> ProgramHandle::latest_checkpoint() const {
    return control_->latest_checkpoint();
}

struct ProgramRuntime::Impl {
    explicit Impl(RuntimeConfig runtime_config)
        : config(std::move(runtime_config)), pool(config.scheduler_threads) {}

    void register_control(const std::shared_ptr<detail::RunControl>& control) {
        std::lock_guard lock(mutex);
        if (stopping) throw std::runtime_error("ProgramRuntime is stopping");
        std::erase_if(controls, [](const std::weak_ptr<detail::RunControl>& weak) {
            const auto live = weak.lock();
            return !live || live->try_result().has_value();
        });
        controls.push_back(control);
    }

    void shutdown() noexcept {
        try {
            std::vector<std::shared_ptr<detail::RunControl>> live;
            {
                std::lock_guard lock(mutex);
                if (stopping) return;
                stopping = true;
                for (auto& weak : controls) {
                    if (auto control = weak.lock()) live.push_back(std::move(control));
                }
            }
            for (auto& control : live)
                control->cancel(detail::CancellationCause::RuntimeShutdown);
            pool.join();
            deadline_pool.join();
        } catch (...) {
            std::terminate();
        }
    }

    RuntimeConfig                                  config;
    asio::thread_pool                              pool;
    asio::thread_pool                              deadline_pool{1};
    std::mutex                                     mutex;
    bool                                           stopping = false;
    std::vector<std::weak_ptr<detail::RunControl>> controls;
};

ProgramRuntime::ProgramRuntime(RuntimeConfig config) {
    if (!config.catalog || !config.checkpoints || !config.journal ||
        config.scheduler_threads == 0) {
        throw std::invalid_argument(
            "ProgramRuntime requires catalog, checkpoints, journal, and at least one scheduler "
            "thread");
    }
    impl_ = std::make_unique<Impl>(std::move(config));
}

ProgramRuntime::ProgramRuntime(ProgramRuntime&&) noexcept = default;

ProgramRuntime& ProgramRuntime::operator=(ProgramRuntime&& other) noexcept {
    if (this == &other) return *this;
    if (impl_) impl_->shutdown();
    impl_ = std::move(other.impl_);
    return *this;
}

ProgramRuntime::~ProgramRuntime() {
    if (impl_) impl_->shutdown();
}

ProgramHandle ProgramRuntime::start(const ProgramVersion& version, ProgramInvocation invocation) {
    auto pinned = detail::CatalogRuntimeAccess::pin(*impl_->config.catalog, version);
    validate_invocation(*pinned, invocation);
    const auto run_id         = generate_run_id();
    const auto core_thread_id = core_thread_identity(run_id, pinned->root->compiled_plan_identity);
    auto       control        = std::make_shared<detail::RunControl>(
        run_id, 1, std::move(pinned), invocation.budget, core_thread_id,
        std::move(invocation.trace_id), std::move(invocation.events),
        impl_->deadline_pool.get_executor(), impl_->config.checkpoints, impl_->config.state_store,
        impl_->config.journal);

    if (control->journal->compare_append("", initial_record(*control)) !=
        JournalAppendResult::Appended) {
        throw_runtime_diagnostic("P_JOURNAL_CONFLICT", "Initial Program journal CAS failed");
    }

    impl_->register_control(control);

    try {
        control->emit(ProgramEventKind::Started, ProgramStartedEvent{invocation.budget});
    } catch (const std::exception& error) {
        detail::RunOutcome failed;
        failed.status                                  = ProgramTerminalStatus::Failed;
        failed.remaining_budget                        = invocation.budget;
        failed.usage.program_operations                = 1;
        failed.remaining_budget.max_program_operations = 0;
        failed.failure =
            ProgramFailure{"P_EVENT_SINK", error.what(), "root", "", 0, json::object()};
        control->complete(std::move(failed));
        return ProgramHandle(std::move(control));
    }

    spawn_run_attempt(impl_->pool, control, std::move(invocation.input), std::nullopt);
    return ProgramHandle(std::move(control));
}

ProgramHandle ProgramRuntime::resume(std::string_view run_id, ProgramResume resume_value) {
    if (run_id.empty()) throw std::invalid_argument("Program resume run_id must not be empty");
    const auto previous = impl_->config.journal->latest(run_id);
    if (!previous || previous->continuation.state != ContinuationState::Interrupted ||
        !previous->core_checkpoint) {
        throw_runtime_diagnostic("P_RESUME_STATE", "Only an interrupted Program run may resume");
    }
    const auto version = impl_->config.catalog->find_version(previous->program_version_id);
    if (!version) {
        throw_runtime_diagnostic("P_START_NOT_ADMITTED",
                                 "Program resume version is no longer materialized");
    }
    auto pinned = detail::CatalogRuntimeAccess::pin(*impl_->config.catalog, *version);
    bool checkpoint_compatible =
        pinned->bundle.id() == previous->bundle_id &&
        pinned->root->compiled_plan_identity == previous->core_checkpoint->core_generation_id &&
        pinned->root->core_name == previous->core_checkpoint->core_name;
    std::string checkpoint_error;
    if (!checkpoint_compatible) {
        checkpoint_error = "Program resume identity binding is incompatible";
    }
    try {
        if (checkpoint_compatible) {
            const auto stored =
                impl_->config.checkpoints->load_by_id(previous->core_checkpoint->checkpoint_id);
            checkpoint_compatible =
                stored && stored->id == previous->core_checkpoint->checkpoint_id &&
                stored->thread_id == previous->core_checkpoint->core_thread_id &&
                stored->schema_version == previous->core_checkpoint->checkpoint_schema_version &&
                stored->schema_version == graph::CHECKPOINT_SCHEMA_VERSION;
            if (!checkpoint_compatible) {
                checkpoint_error = "Journal-published Core checkpoint is absent or incompatible";
            }
        }
    } catch (const std::exception& error) {
        checkpoint_compatible = false;
        checkpoint_error      = error.what();
    } catch (...) {
        checkpoint_compatible = false;
        checkpoint_error      = "Checkpoint store failed with a non-standard exception";
    }

    auto control = std::make_shared<detail::RunControl>(
        previous->run_id, previous->continuation.attempt + 1, std::move(pinned),
        previous->remaining_budget, previous->core_checkpoint->core_thread_id,
        std::move(resume_value.trace_id), std::move(resume_value.events),
        impl_->deadline_pool.get_executor(), impl_->config.checkpoints, impl_->config.state_store,
        impl_->config.journal);
    const auto running = resumed_record(*control, *previous);
    if (control->journal->compare_append(previous->id, running) != JournalAppendResult::Appended) {
        throw_runtime_diagnostic("P_RESUME_CONFLICT", "Program resume lost the journal CAS");
    }

    impl_->register_control(control);
    try {
        control->emit(ProgramEventKind::Started, ProgramStartedEvent{previous->remaining_budget});
    } catch (const std::exception& error) {
        detail::RunOutcome failed;
        failed.status           = ProgramTerminalStatus::Failed;
        failed.remaining_budget = previous->remaining_budget;
        failed.checkpoint       = previous->core_checkpoint;
        failed.failure =
            ProgramFailure{"P_EVENT_SINK", error.what(), "root", "", 0, json::object()};
        control->complete(std::move(failed));
        return ProgramHandle(std::move(control));
    }

    if (!checkpoint_compatible) {
        detail::RunOutcome incompatible;
        incompatible.status           = ProgramTerminalStatus::CheckpointIncompatible;
        incompatible.remaining_budget = previous->remaining_budget;
        incompatible.checkpoint       = previous->core_checkpoint;
        incompatible.failure =
            ProgramFailure{"P_CHECKPOINT_INCOMPATIBLE",
                           checkpoint_error,
                           "root",
                           "",
                           0,
                           json{{"checkpoint_id", previous->core_checkpoint->checkpoint_id}}};
        control->complete(std::move(incompatible));
        return ProgramHandle(std::move(control));
    }

    spawn_run_attempt(impl_->pool, control, std::move(resume_value.value),
                      previous->core_checkpoint->checkpoint_id);
    return ProgramHandle(std::move(control));
}

asio::awaitable<ProgramResult> ProgramRuntime::run_async(ProgramVersion    version,
                                                         ProgramInvocation invocation) {
    auto               handle = start(version, std::move(invocation));
    co_return co_await handle.wait_async();
}

asio::awaitable<ProgramResult> ProgramRuntime::resume_async(std::string   run_id,
                                                            ProgramResume resume_value) {
    auto               handle = resume(run_id, std::move(resume_value));
    co_return co_await handle.wait_async();
}

ProgramResult ProgramRuntime::run(const ProgramVersion& version, ProgramInvocation invocation) {
    return start(version, std::move(invocation)).wait();
}

}  // namespace neograph::program
