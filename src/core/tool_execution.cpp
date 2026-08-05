#include <neograph/tool_execution.h>

#include <neograph/graph/cancel.h>
#include <neograph/tool.h>

#include <asio/experimental/awaitable_operators.hpp>
#include <asio/async_result.hpp>
#include <asio/bind_executor.hpp>
#include <asio/error.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <cctype>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace neograph {
namespace {

constexpr std::size_t kMaximumResourceKeyBytes = 512;
constexpr std::size_t kMaximumResourceComponentBytes = 256;
constexpr std::size_t kMaximumPolicyQueueDepth = 65'536;
constexpr std::size_t kMaximumOutputBytes = 64U * 1024U * 1024U;

struct SingleFlightState {
    mutable std::mutex mutex;
    bool completed = false;
    ToolExecutionResult result;
    std::uint32_t waiters = 0;
};

bool is_identifier_char(unsigned char value) {
    return std::isalnum(value) || value == '_' || value == '-';
}

void validate_resource_template(std::string_view value) {
    if (value.empty() || value.size() > kMaximumResourceKeyBytes) {
        throw std::invalid_argument("tool resource key template must be 1..512 bytes");
    }

    for (std::size_t pos = 0; pos < value.size();) {
        const auto open = value.find('{', pos);
        const auto close = value.find('}', pos);
        if (close != std::string_view::npos
            && (open == std::string_view::npos || close < open)) {
            throw std::invalid_argument("tool resource key template has an unmatched '}'");
        }
        if (open == std::string_view::npos) break;
        const auto end = value.find('}', open + 1);
        if (end == std::string_view::npos || end == open + 1) {
            throw std::invalid_argument("tool resource key template has an unterminated placeholder");
        }
        const auto token = value.substr(open + 1, end - open - 1);
        if (token == "tool" || token == "owner" || token == "root" || token == "thread") {
            pos = end + 1;
            continue;
        }
        constexpr std::string_view kArgumentPrefix{"arg:"};
        if (token.starts_with(kArgumentPrefix)) {
            const auto name = token.substr(kArgumentPrefix.size());
            if (name.empty()
                || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
                       return is_identifier_char(c);
                   })) {
                throw std::invalid_argument("tool resource argument placeholder is invalid");
            }
            pos = end + 1;
            continue;
        }
        throw std::invalid_argument("tool resource key template has an unsupported placeholder");
    }
}

std::string scalar_component(const json& value, std::string_view label) {
    std::string raw;
    if (value.is_string()) {
        raw = value.get<std::string>();
    } else if (value.is_boolean() || value.is_number()) {
        raw = value.dump();
    } else {
        throw std::invalid_argument("tool resource " + std::string(label)
                                    + " must be a scalar JSON value");
    }
    if (raw.empty() || raw.size() > kMaximumResourceComponentBytes) {
        throw std::invalid_argument("tool resource " + std::string(label)
                                    + " must be 1..256 bytes");
    }

    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(raw.size());
    for (const unsigned char ch : raw) {
        if (std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[ch >> 4U]);
            encoded.push_back(hex[ch & 0x0fU]);
        }
    }
    return encoded;
}

std::string required_identity_component(std::string_view value, std::string_view label) {
    if (value.empty()) {
        throw std::invalid_argument("tool resource template requires " + std::string(label)
                                    + " identity");
    }
    return scalar_component(json(std::string(value)), label);
}

std::string derive_resource_key(const ToolExecutionPolicy& policy,
                                std::string_view tool_name,
                                const json& arguments,
                                const ToolExecutionIdentity& identity) {
    validate_resource_template(policy.resource_key_template);

    std::string key;
    key.reserve(policy.resource_key_template.size() + tool_name.size());
    for (std::size_t pos = 0; pos < policy.resource_key_template.size();) {
        const auto open = policy.resource_key_template.find('{', pos);
        if (open == std::string::npos) {
            key.append(policy.resource_key_template, pos, std::string::npos);
            break;
        }
        key.append(policy.resource_key_template, pos, open - pos);
        const auto end = policy.resource_key_template.find('}', open + 1);
        const auto token = std::string_view(policy.resource_key_template).substr(
            open + 1, end - open - 1);
        if (token == "tool") {
            key += required_identity_component(tool_name, "tool");
        } else if (token == "owner") {
            const auto owner = identity.owner_scope.empty()
                             ? std::string_view{"anonymous"}
                             : std::string_view(identity.owner_scope);
            key += required_identity_component(owner, "owner");
        } else if (token == "root") {
            key += required_identity_component(identity.root_run_id, "root");
        } else if (token == "thread") {
            key += required_identity_component(identity.thread_id, "thread");
        } else {
            constexpr std::string_view kArgumentPrefix{"arg:"};
            const auto name = token.substr(kArgumentPrefix.size());
            if (!arguments.is_object() || !arguments.contains(std::string(name))) {
                throw std::invalid_argument("tool resource template requires argument '"
                                            + std::string(name) + "'");
            }
            key += scalar_component(arguments.at(std::string(name)), name);
        }
        pos = end + 1;
    }
    if (key.empty() || key.size() > kMaximumResourceKeyBytes) {
        throw std::invalid_argument("derived tool resource key must be 1..512 bytes");
    }
    return key;
}

HostAdmissionRequest host_request_for(const ToolExecutionPolicy& policy,
                                      std::string_view tool_name,
                                      const ToolExecutionIdentity& identity) {
    HostAdmissionRequest request;
    request.owner_scope = identity.owner_scope.empty() ? "anonymous" : identity.owner_scope;
    request.operation_id = std::string(tool_name);
    request.resources = policy.host_resources;
    if (request.resources.empty()) {
        // The host controller is opt-in. Once opted in, every tool consumes at
        // least one explicit global tool slot instead of bypassing admission.
        request.resources.tool_slots = 1;
    }
    // Host queue depth belongs to the shared host controller. Reusing the
    // per-tool key queue's limit would reject a safe controller with a tighter
    // global backlog cap rather than inheriting that cap.
    request.max_pending = 0;
    request.priority = policy.host_priority;
    request.queue_timeout = policy.queue_timeout;
    return request;
}

asio::thread_pool& blocking_tool_pool() {
    // This is intentionally one bounded shared pool, never one thread per tool
    // call. Resource leases bound the work admitted to it; the pool only keeps
    // legacy synchronous implementations from pinning the graph event loop.
    static asio::thread_pool pool([] {
        const auto hardware = std::thread::hardware_concurrency();
        return std::max<std::size_t>(2, std::min<std::size_t>(hardware == 0 ? 4 : hardware, 16));
    }());
    return pool;
}

} // namespace

std::string_view to_string(const ToolTerminalStatus status) noexcept {
    switch (status) {
        case ToolTerminalStatus::Succeeded: return "succeeded";
        case ToolTerminalStatus::Failed: return "failed";
        case ToolTerminalStatus::CancelledBeforeStart: return "cancelled_before_start";
        case ToolTerminalStatus::CancellationRequested: return "cancellation_requested";
        case ToolTerminalStatus::Expired: return "expired";
        case ToolTerminalStatus::TimedOut: return "timed_out";
        case ToolTerminalStatus::Killed: return "killed";
        case ToolTerminalStatus::InputRequired: return "input_required";
        case ToolTerminalStatus::ReconciliationRequired: return "reconciliation_required";
        case ToolTerminalStatus::Rejected: return "rejected";
    }
    return "unknown";
}

ToolTerminalStatus tool_terminal_status_from_string(const std::string_view value) {
    if (value == "succeeded") return ToolTerminalStatus::Succeeded;
    if (value == "failed") return ToolTerminalStatus::Failed;
    if (value == "cancelled_before_start") return ToolTerminalStatus::CancelledBeforeStart;
    if (value == "cancellation_requested") return ToolTerminalStatus::CancellationRequested;
    if (value == "expired") return ToolTerminalStatus::Expired;
    if (value == "timed_out") return ToolTerminalStatus::TimedOut;
    if (value == "killed") return ToolTerminalStatus::Killed;
    if (value == "input_required") return ToolTerminalStatus::InputRequired;
    if (value == "reconciliation_required") return ToolTerminalStatus::ReconciliationRequired;
    if (value == "rejected") return ToolTerminalStatus::Rejected;
    throw std::invalid_argument("unknown tool terminal status");
}

void ToolExecutionPolicy::validate() const {
    if (schema_version != SCHEMA_VERSION) {
        throw std::invalid_argument("unsupported tool execution policy schema version");
    }
    if (max_pending == 0 || max_pending > kMaximumPolicyQueueDepth) {
        throw std::invalid_argument("tool execution max_pending must be 1..65536");
    }
    if (queue_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("tool execution queue_timeout must be positive");
    }
    if (execution_timeout < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("tool execution execution_timeout must not be negative");
    }
    if (retry_max_attempts == 0) {
        throw std::invalid_argument("tool execution retry_max_attempts must be positive");
    }
    if (retry_backoff < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("tool execution retry_backoff must not be negative");
    }
    if (output_limit_bytes == 0 || output_limit_bytes > kMaximumOutputBytes) {
        throw std::invalid_argument("tool execution output_limit_bytes must be 1..64MiB");
    }
    if (concurrency == ToolConcurrency::KeyedExclusive
        || concurrency == ToolConcurrency::Exclusive
        || concurrency == ToolConcurrency::SingleFlight
        || concurrency == ToolConcurrency::ExternalLimited) {
        if (capacity != 1
            && concurrency != ToolConcurrency::ExternalLimited) {
            throw std::invalid_argument("exclusive tool policy must have capacity 1");
        }
        if (concurrency != ToolConcurrency::Exclusive) {
            validate_resource_template(resource_key_template);
        }
    } else if (concurrency == ToolConcurrency::Capacity && capacity == 0) {
        throw std::invalid_argument("capacity tool policy must have positive capacity");
    }
    if (concurrency == ToolConcurrency::ExternalLimited && capacity == 0) {
        throw std::invalid_argument("external-limited tool policy must have positive capacity");
    }
    if (concurrency == ToolConcurrency::Reentrant && capacity != 1) {
        throw std::invalid_argument("reentrant tool policy cannot declare a capacity");
    }
    if (concurrency == ToolConcurrency::SingleFlight
        && (idempotency != ToolIdempotency::Idempotent
            || (effect != ToolEffectClass::ReadOnly
                && effect != ToolEffectClass::ExternalRead))) {
        throw std::invalid_argument(
            "single-flight requires an idempotent read-only tool effect");
    }
    if (retry_max_attempts > 1
        && (idempotency == ToolIdempotency::Unknown
            || idempotency == ToolIdempotency::NonIdempotent)
        && effect != ToolEffectClass::ReadOnly
        && effect != ToolEffectClass::ExternalRead) {
        throw std::invalid_argument(
            "automatic retries require idempotent or read-only tool effects");
    }
}

namespace detail {

asio::awaitable<std::string> execute_blocking_tool_async(Tool& tool, json arguments) {
    struct Result {
        std::string value;
        std::exception_ptr error;
    };

    const auto caller_executor = co_await asio::this_coro::executor;
    auto result = std::make_shared<Result>();
    auto completion_token = asio::bind_executor(caller_executor, asio::use_awaitable);
    co_await asio::async_initiate<decltype(completion_token), void()>(
        [&tool, arguments = std::move(arguments), result, caller_executor](auto handler) mutable {
            using Handler = std::decay_t<decltype(handler)>;
            auto completion = std::make_shared<Handler>(std::move(handler));
            const auto completion_executor = caller_executor;
            asio::post(blocking_tool_pool().get_executor(),
                       [&tool, arguments = std::move(arguments), result,
                        completion = std::move(completion), completion_executor]() mutable {
                           try {
                               result->value = tool.execute(arguments);
                           } catch (...) {
                               result->error = std::current_exception();
                           }
                           asio::post(completion_executor,
                                      [completion = std::move(completion)]() mutable {
                                          (*completion)();
                                      });
                       });
        },
        completion_token);

    if (result->error) std::rethrow_exception(result->error);
    co_return std::move(result->value);
}

class ProcessBridgeError final : public std::runtime_error {
public:
    ProcessBridgeError(ToolTerminalStatus status, std::string message,
                       std::string output = {}, std::optional<int> exit_code = {},
                       std::optional<int> signal_number = {}, bool uncertain = false,
                       bool output_truncated = false)
        : std::runtime_error(std::move(message)),
          status(status),
          output(std::move(output)),
          exit_code(exit_code),
          signal_number(signal_number),
          effect_uncertain(uncertain),
          output_truncated(output_truncated) {}

    ToolTerminalStatus        status;
    std::string               output;
    std::optional<int>        exit_code;
    std::optional<int>        signal_number;
    bool                      effect_uncertain = false;
    bool                      output_truncated = false;
};

#if defined(__unix__) || defined(__APPLE__)
namespace {

void close_fd(int& fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

[[noreturn]] void throw_process_setup(std::string message) {
    throw ProcessBridgeError(ToolTerminalStatus::Failed, std::move(message));
}

void terminate_process_group(pid_t pid, int signal) noexcept {
    if (pid > 0) {
        (void)::kill(-pid, signal);
    }
}

struct ProcessExit {
    std::string output;
    std::optional<int> exit_code;
    std::optional<int> signal_number;
};

ProcessExit run_process(const ToolProcessSpec& spec,
                        std::size_t output_limit_bytes,
                        std::chrono::milliseconds timeout,
                        const std::shared_ptr<graph::CancelToken>& cancel_token) {
    if (spec.argv.empty() || spec.argv.front().empty()) {
        throw_process_setup("process tool requires a non-empty argv");
    }
    if (spec.interactive) {
        throw ProcessBridgeError(ToolTerminalStatus::InputRequired,
                                 "process tool requires an interactive input turn");
    }
    if (spec.stdin_data.size() > kMaximumOutputBytes) {
        throw_process_setup("process tool stdin exceeds the safety limit");
    }

    int output_pipe[2] = {-1, -1};
    int input_pipe[2] = {-1, -1};
    if (::pipe(output_pipe) != 0 || ::pipe(input_pipe) != 0) {
        close_fd(output_pipe[0]);
        close_fd(output_pipe[1]);
        close_fd(input_pipe[0]);
        close_fd(input_pipe[1]);
        throw_process_setup("process tool pipe creation failed");
    }

    const pid_t child = ::fork();
    if (child < 0) {
        close_fd(output_pipe[0]);
        close_fd(output_pipe[1]);
        close_fd(input_pipe[0]);
        close_fd(input_pipe[1]);
        throw_process_setup("process tool fork failed");
    }
    if (child == 0) {
        (void)::setpgid(0, 0);
        if (!spec.working_directory.empty()
            && ::chdir(spec.working_directory.c_str()) != 0) {
            _exit(126);
        }
        if (::dup2(output_pipe[1], STDOUT_FILENO) < 0
            || ::dup2(output_pipe[1], STDERR_FILENO) < 0
            || ::dup2(input_pipe[0], STDIN_FILENO) < 0) {
            _exit(126);
        }
        close_fd(output_pipe[0]);
        close_fd(output_pipe[1]);
        close_fd(input_pipe[0]);
        close_fd(input_pipe[1]);
        for (const auto& entry : spec.environment) {
            const auto separator = entry.find('=');
            if (separator == std::string::npos || separator == 0) _exit(126);
            const auto key = entry.substr(0, separator);
            const auto value = entry.substr(separator + 1);
            if (::setenv(key.c_str(), value.c_str(), 1) != 0) _exit(126);
        }

        std::vector<char*> argv;
        argv.reserve(spec.argv.size() + 1);
        for (const auto& argument : spec.argv) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv.front(), argv.data());
        _exit(127);
    }

    (void)::setpgid(child, child);
    close_fd(output_pipe[1]);
    close_fd(input_pipe[0]);
    const bool has_input = !spec.stdin_data.empty();
    if (!has_input) close_fd(input_pipe[1]);
    const auto set_nonblocking = [](int fd) {
        const auto flags = ::fcntl(fd, F_GETFL, 0);
        return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    };
    if (!set_nonblocking(output_pipe[0])
        || (has_input && !set_nonblocking(input_pipe[1]))) {
        terminate_process_group(child, SIGKILL);
        (void)::waitpid(child, nullptr, 0);
        close_fd(output_pipe[0]);
        close_fd(input_pipe[1]);
        throw_process_setup("process tool failed to configure nonblocking pipes");
    }

    std::string output;
    output.reserve(std::min<std::size_t>(output_limit_bytes, 4096));
    std::size_t input_offset = 0;
    int status = 0;
    bool output_open = true;
    bool input_open = has_input;
    bool child_reaped = false;
    const auto started_at = std::chrono::steady_clock::now();
    const auto deadline = timeout > std::chrono::milliseconds::zero()
                              ? started_at + timeout
                              : std::chrono::steady_clock::time_point::max();

    const auto reap_after_kill = [&](int signal, ToolTerminalStatus terminal,
                                     std::string message,
                                     bool output_truncated = false) -> ProcessExit {
        terminate_process_group(child, signal);
        const auto grace_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
        while (!child_reaped && std::chrono::steady_clock::now() < grace_deadline) {
            const auto result = ::waitpid(child, &status, WNOHANG);
            if (result == child) {
                child_reaped = true;
                break;
            }
            if (result < 0 && errno != EINTR) break;
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        if (!child_reaped) {
            terminate_process_group(child, SIGKILL);
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            child_reaped = true;
        }
        close_fd(output_pipe[0]);
        close_fd(input_pipe[1]);
        std::optional<int> exit_code;
        std::optional<int> signal_number;
        if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
        if (WIFSIGNALED(status)) signal_number = WTERMSIG(status);
        throw ProcessBridgeError(terminal, std::move(message), std::move(output),
                                 exit_code, signal_number, true, output_truncated);
    };

    while (!child_reaped || output_open || input_open) {
        if (cancel_token && cancel_token->is_cancelled()) {
            return reap_after_kill(SIGTERM, ToolTerminalStatus::CancellationRequested,
                                   "process cancellation requested");
        }
        if (!child_reaped && std::chrono::steady_clock::now() >= deadline) {
            return reap_after_kill(SIGTERM, ToolTerminalStatus::TimedOut,
                                   "process execution deadline expired");
        }

        pollfd descriptors[2]{};
        nfds_t count = 0;
        int output_index = -1;
        int input_index = -1;
        if (output_open) {
            output_index = static_cast<int>(count);
            descriptors[count++] = pollfd{output_pipe[0], POLLIN | POLLHUP, 0};
        }
        if (input_open) {
            input_index = static_cast<int>(count);
            descriptors[count++] = pollfd{input_pipe[1], POLLOUT | POLLHUP, 0};
        }
        const auto poll_result = ::poll(descriptors, count, 10);
        if (poll_result < 0 && errno != EINTR) {
            return reap_after_kill(SIGKILL, ToolTerminalStatus::Killed,
                                   "process output polling failed");
        }
        if (output_index >= 0
            && (descriptors[output_index].revents & (POLLIN | POLLHUP))) {
            char buffer[8192];
            for (;;) {
                const auto read_count = ::read(output_pipe[0], buffer, sizeof(buffer));
                if (read_count > 0) {
                    output.append(buffer, static_cast<std::size_t>(read_count));
                    if (output.size() > output_limit_bytes) {
                        return reap_after_kill(SIGTERM, ToolTerminalStatus::Killed,
                                               "process output exceeded the policy limit", true);
                    }
                    continue;
                }
                if (read_count == 0 || (read_count < 0 && errno != EAGAIN && errno != EINTR)) {
                    close_fd(output_pipe[0]);
                    output_open = false;
                }
                break;
            }
        }
        if (input_index >= 0
            && (descriptors[input_index].revents & (POLLOUT | POLLHUP))) {
            while (input_offset < spec.stdin_data.size()) {
                const auto written = ::write(input_pipe[1], spec.stdin_data.data() + input_offset,
                                             spec.stdin_data.size() - input_offset);
                if (written > 0) {
                    input_offset += static_cast<std::size_t>(written);
                    continue;
                }
                if (written < 0 && (errno == EAGAIN || errno == EINTR)) break;
                input_open = false;
                close_fd(input_pipe[1]);
                break;
            }
            if (input_offset == spec.stdin_data.size()) {
                input_open = false;
                close_fd(input_pipe[1]);
            }
        }
        if (!child_reaped) {
            const auto result = ::waitpid(child, &status, WNOHANG);
            if (result == child) child_reaped = true;
            else if (result < 0 && errno != EINTR) {
                return reap_after_kill(SIGKILL, ToolTerminalStatus::Killed,
                                       "process wait failed");
            }
        }
        if (child_reaped && !output_open) break;
    }
    close_fd(output_pipe[0]);
    close_fd(input_pipe[1]);
    std::optional<int> exit_code;
    std::optional<int> signal_number;
    if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
    if (WIFSIGNALED(status)) signal_number = WTERMSIG(status);
    if (signal_number || !exit_code || *exit_code != 0) {
        const auto message = exit_code
            ? "process exited with status " + std::to_string(*exit_code)
            : "process exited by signal " + std::to_string(signal_number.value_or(0));
        throw ProcessBridgeError(ToolTerminalStatus::Failed, message, std::move(output),
                                 exit_code, signal_number);
    }
    return ProcessExit{std::move(output), exit_code, signal_number};
}

} // namespace
#endif

asio::awaitable<std::string> execute_process_tool_async(
    Tool& tool, json arguments, ToolExecutionContext context,
    std::size_t output_limit_bytes, std::chrono::milliseconds timeout) {
    auto* process_tool = dynamic_cast<ProcessTool*>(&tool);
    if (!process_tool) {
        throw std::invalid_argument(
            "blocking_process policy requires a ProcessTool implementation");
    }
    const auto spec = process_tool->prepare_process(arguments, context);
#if defined(__unix__) || defined(__APPLE__)
    struct Result {
        std::string value;
        std::exception_ptr error;
    };
    const auto caller_executor = co_await asio::this_coro::executor;
    auto result = std::make_shared<Result>();
    auto completion_token = asio::bind_executor(caller_executor, asio::use_awaitable);
    co_await asio::async_initiate<decltype(completion_token), void()>(
        [spec, output_limit_bytes, timeout, cancel_token = context.cancel_token,
         result, caller_executor](auto handler) mutable {
            using Handler = std::decay_t<decltype(handler)>;
            auto completion = std::make_shared<Handler>(std::move(handler));
            asio::post(blocking_tool_pool().get_executor(),
                       [spec, output_limit_bytes, timeout, cancel_token = std::move(cancel_token),
                        result, completion = std::move(completion), caller_executor]() mutable {
                           try {
                               result->value = run_process(spec, output_limit_bytes, timeout,
                                                           cancel_token).output;
                           } catch (...) {
                               result->error = std::current_exception();
                           }
                           asio::post(caller_executor,
                                      [completion = std::move(completion)]() mutable {
                                          (*completion)();
                                      });
                       });
        },
        completion_token);
    if (result->error) std::rethrow_exception(result->error);
    co_return std::move(result->value);
#else
    (void)arguments;
    (void)context;
    (void)output_limit_bytes;
    (void)timeout;
    throw std::runtime_error("process tools are unsupported on this platform");
#endif
}

class ResourceArbiterImpl final : public std::enable_shared_from_this<ResourceArbiterImpl> {
public:
    struct Waiter {
        enum class State : std::uint8_t { Queued, Granted, Claimed, Cancelled };

        std::string resource_key;
        State state = State::Queued;
        std::uint8_t priority = 0;
        std::uint8_t fairness_weight = 1;
        std::uint64_t sequence = 0;
        std::chrono::steady_clock::time_point submitted_at;
    };

    struct Resource {
        std::uint32_t capacity = 0;
        std::uint32_t active = 0;
        std::deque<std::shared_ptr<Waiter>> waiters;
    };

    struct Registration {
        ResourceLease immediate;
        std::shared_ptr<Waiter> waiter;
    };

    Registration enqueue(ResourceRequest request) {
        if (!valid_request(request)) {
            throw std::invalid_argument("invalid resource admission request");
        }

        std::lock_guard<std::mutex> lock(mu_);
        if (stopping_) {
            throw asio::system_error(asio::error::operation_aborted);
        }
        auto [found, inserted] = resources_.try_emplace(
            request.resource_key, Resource{request.capacity, 0, {}});
        auto& resource = found->second;
        if (!inserted && resource.capacity != request.capacity) {
            throw std::invalid_argument(
                "one resource key cannot be admitted with conflicting capacities");
        }
        if (resource.active < resource.capacity && resource.waiters.empty()) {
            ++resource.active;
            return Registration{ResourceLease(shared_from_this(), found->first), {}};
        }
        if (resource.waiters.size() >= request.max_pending) {
            throw asio::system_error(asio::error::no_buffer_space);
        }

        auto waiter = std::make_shared<Waiter>();
        waiter->resource_key = found->first;
        waiter->priority = request.priority;
        waiter->fairness_weight = std::max<std::uint8_t>(1, request.fairness_weight);
        waiter->sequence = next_sequence_++;
        waiter->submitted_at = std::chrono::steady_clock::now();
        resource.waiters.push_back(waiter);
        return Registration{{}, std::move(waiter)};
    }

    [[nodiscard]] Waiter::State state(const std::shared_ptr<Waiter>& waiter) const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        return waiter->state;
    }

    ResourceLease claim(const std::shared_ptr<Waiter>& waiter) {
        std::lock_guard<std::mutex> lock(mu_);
        if (waiter->state != Waiter::State::Granted) {
            throw asio::system_error(asio::error::operation_aborted);
        }
        waiter->state = Waiter::State::Claimed;
        return ResourceLease(shared_from_this(), waiter->resource_key);
    }

    /// Remove an unclaimed waiter or return a granted capacity slot on unwind.
    void abandon(const std::shared_ptr<Waiter>& waiter) noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        if (waiter->state == Waiter::State::Queued) {
            const auto resource = resources_.find(waiter->resource_key);
            if (resource != resources_.end()) {
                auto& waiters = resource->second.waiters;
                const auto found = std::find(waiters.begin(), waiters.end(), waiter);
                if (found != waiters.end()) waiters.erase(found);
            }
            waiter->state = Waiter::State::Cancelled;
        } else if (waiter->state == Waiter::State::Granted) {
            waiter->state = Waiter::State::Cancelled;
            release_one_locked(waiter->resource_key);
        }
    }

    void release(const std::string& resource_key) noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        release_one_locked(resource_key);
    }

    void shutdown() noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopping_) return;
        stopping_ = true;
        for (auto& [_, resource] : resources_) {
            for (auto& waiter : resource.waiters) {
                if (waiter->state == Waiter::State::Queued) {
                    waiter->state = Waiter::State::Cancelled;
                }
            }
            resource.waiters.clear();
        }
    }

private:
    static bool valid_request(const ResourceRequest& request) {
        return !request.resource_key.empty()
            && request.resource_key.size() <= kMaximumResourceKeyBytes
            && request.capacity > 0
            && request.max_pending > 0
            && request.max_pending <= kMaximumPolicyQueueDepth
            && request.queue_timeout > std::chrono::milliseconds::zero();
    }

    static std::uint8_t effective_priority(
        const Waiter& waiter, const std::chrono::steady_clock::time_point now) noexcept {
        const auto elapsed = now > waiter.submitted_at
                           ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - waiter.submitted_at)
                           : std::chrono::milliseconds::zero();
        const auto age = static_cast<std::uint64_t>(elapsed.count()) / 1000U;
        const auto score = static_cast<std::uint64_t>(waiter.priority)
                         + age * std::max<std::uint8_t>(1, waiter.fairness_weight);
        return static_cast<std::uint8_t>(std::min<std::uint64_t>(255, score));
    }

    void release_one_locked(const std::string& resource_key) noexcept {
        const auto found = resources_.find(resource_key);
        if (found == resources_.end() || found->second.active == 0) return;
        auto& resource = found->second;
        --resource.active;
        if (stopping_) return;
        while (resource.active < resource.capacity && !resource.waiters.empty()) {
            const auto now = std::chrono::steady_clock::now();
            auto best = resource.waiters.end();
            std::uint8_t best_priority = 0;
            std::uint64_t best_sequence = std::numeric_limits<std::uint64_t>::max();
            for (auto candidate = resource.waiters.begin();
                 candidate != resource.waiters.end(); ++candidate) {
                if ((*candidate)->state != Waiter::State::Queued) continue;
                const auto priority = effective_priority(*(*candidate), now);
                if (best == resource.waiters.end()
                    || priority > best_priority
                    || (priority == best_priority
                        && (*candidate)->sequence < best_sequence)) {
                    best = candidate;
                    best_priority = priority;
                    best_sequence = (*candidate)->sequence;
                }
            }
            if (best == resource.waiters.end()) break;
            auto waiter = std::move(*best);
            resource.waiters.erase(best);
            waiter->state = Waiter::State::Granted;
            ++resource.active;
        }
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, Resource> resources_;
    std::uint64_t next_sequence_ = 1;
    bool stopping_ = false;
};

} // namespace detail

ResourceLease::ResourceLease(std::shared_ptr<detail::ResourceArbiterImpl> impl,
                             std::string resource_key) noexcept
    : impl_(std::move(impl)), resource_key_(std::move(resource_key)) {}

ResourceLease::ResourceLease(ResourceLease&& other) noexcept
    : impl_(std::move(other.impl_)), resource_key_(std::move(other.resource_key_)) {}

ResourceLease& ResourceLease::operator=(ResourceLease&& other) noexcept {
    if (this != &other) {
        release();
        impl_ = std::move(other.impl_);
        resource_key_ = std::move(other.resource_key_);
    }
    return *this;
}

ResourceLease::~ResourceLease() noexcept {
    release();
}

bool ResourceLease::held() const noexcept {
    return static_cast<bool>(impl_);
}

std::string_view ResourceLease::resource_key() const noexcept {
    return resource_key_;
}

void ResourceLease::release() noexcept {
    if (impl_) {
        impl_->release(resource_key_);
        impl_.reset();
        resource_key_.clear();
    }
}

ResourceArbiter::ResourceArbiter()
    : impl_(std::make_shared<detail::ResourceArbiterImpl>()) {}

ResourceArbiter::~ResourceArbiter() {
    impl_->shutdown();
}

asio::awaitable<ResourceLease> ResourceArbiter::acquire_async(
    ResourceRequest request, std::shared_ptr<graph::CancelToken> cancel_token) {
    if (cancel_token) cancel_token->throw_if_cancelled("while waiting for a tool resource");
    struct PendingGuard {
        std::shared_ptr<detail::ResourceArbiterImpl> impl;
        std::shared_ptr<detail::ResourceArbiterImpl::Waiter> waiter;

        ~PendingGuard() {
            if (waiter) impl->abandon(waiter);
        }

        ResourceLease claim() {
            auto lease = impl->claim(waiter);
            waiter.reset();
            return lease;
        }
    };

    const auto queue_timeout = request.queue_timeout;
    auto registration = impl_->enqueue(std::move(request));
    if (registration.immediate.held()) {
        if (cancel_token) {
            cancel_token->throw_if_cancelled("while waiting for a tool resource");
        }
        co_return std::move(registration.immediate);
    }

    PendingGuard pending{impl_, std::move(registration.waiter)};
    const auto executor = co_await asio::this_coro::executor;
    asio::steady_timer poll(executor);
    const auto deadline = std::chrono::steady_clock::now() + queue_timeout;

    for (;;) {
        if (cancel_token && cancel_token->is_cancelled()) {
            throw graph::CancelledException("while waiting for a tool resource");
        }

        switch (impl_->state(pending.waiter)) {
            case detail::ResourceArbiterImpl::Waiter::State::Granted:
                co_return pending.claim();
            case detail::ResourceArbiterImpl::Waiter::State::Cancelled:
                if (cancel_token && cancel_token->is_cancelled()) {
                    throw graph::CancelledException("while waiting for a tool resource");
                }
                throw asio::system_error(asio::error::operation_aborted);
            case detail::ResourceArbiterImpl::Waiter::State::Claimed:
                throw std::logic_error("tool resource waiter was claimed twice");
            case detail::ResourceArbiterImpl::Waiter::State::Queued:
                break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) throw asio::system_error(asio::error::timed_out);
        poll.expires_after(std::min(std::chrono::milliseconds{1},
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                        deadline - now)));
        asio::error_code wait_error;
        co_await poll.async_wait(asio::redirect_error(asio::use_awaitable, wait_error));
        if (wait_error && wait_error != asio::error::operation_aborted) {
            throw asio::system_error(wait_error);
        }
        if (wait_error == asio::error::operation_aborted) {
            throw asio::system_error(wait_error);
        }
    }
}

class ToolExecutionPolicyRegistry::Impl final {
public:
    mutable std::mutex mu;
    ToolExecutionPolicy default_policy;
    std::unordered_map<std::string, ToolExecutionPolicy> by_tool;
};

ToolExecutionPolicyRegistry::ToolExecutionPolicyRegistry()
    : impl_(std::make_unique<Impl>()) {}

ToolExecutionPolicyRegistry::~ToolExecutionPolicyRegistry() = default;

void ToolExecutionPolicyRegistry::upsert(std::string tool_name, ToolExecutionPolicy policy) {
    if (tool_name.empty()) throw std::invalid_argument("tool execution policy requires a tool name");
    policy.validate();
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->by_tool.insert_or_assign(std::move(tool_name), std::move(policy));
}

void ToolExecutionPolicyRegistry::erase(std::string_view tool_name) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->by_tool.erase(std::string(tool_name));
}

void ToolExecutionPolicyRegistry::set_default(ToolExecutionPolicy policy) {
    policy.validate();
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->default_policy = std::move(policy);
}

ToolExecutionPolicy ToolExecutionPolicyRegistry::resolve(std::string_view tool_name) const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    const auto found = impl_->by_tool.find(std::string(tool_name));
    return found == impl_->by_tool.end() ? impl_->default_policy : found->second;
}
class ToolExecutionController::Impl final {
public:
    std::shared_ptr<ToolExecutionPolicyRegistry> policies;
    std::shared_ptr<ResourceArbiter> arbiter;
    std::shared_ptr<HostAdmissionController> host_admission;
    std::mutex singleflight_mutex;
    std::unordered_map<std::string, std::shared_ptr<SingleFlightState>> singleflight;
};

ToolExecutionController::ToolExecutionController(ToolExecutionControllerConfig config)
    : impl_(std::make_unique<Impl>()) {
    impl_->policies = config.policies ? std::move(config.policies)
                                      : std::make_shared<ToolExecutionPolicyRegistry>();
    impl_->arbiter = config.arbiter ? std::move(config.arbiter)
                                    : std::make_shared<ResourceArbiter>();
    impl_->host_admission = std::move(config.host_admission);
}

ToolExecutionController::~ToolExecutionController() = default;

asio::awaitable<ToolExecutionResult> ToolExecutionController::execute_result_async(
    Tool& tool, json arguments, ToolExecutionContext context) {
    const auto policy = impl_->policies->resolve(tool.get_name());
    bool started = false;
    std::shared_ptr<SingleFlightState> singleflight_state;
    std::string singleflight_key;
    bool singleflight_leader = false;
    auto publish_singleflight = [&, this](ToolExecutionResult result) {
        if (!singleflight_leader) return result;
        std::lock_guard<std::mutex> map_lock(impl_->singleflight_mutex);
        const auto found = impl_->singleflight.find(singleflight_key);
        if (found != impl_->singleflight.end()
            && found->second == singleflight_state) {
            std::lock_guard<std::mutex> state_lock(singleflight_state->mutex);
            singleflight_state->result = result;
            singleflight_state->completed = true;
            impl_->singleflight.erase(found);
        }
        return result;
    };
    try {
        policy.validate();
        const auto identity = context.identity;

        if (policy.concurrency == ToolConcurrency::SingleFlight) {
            singleflight_key = derive_resource_key(
                policy, tool.get_name(), arguments, identity);
            {
                std::lock_guard<std::mutex> lock(impl_->singleflight_mutex);
                const auto found = impl_->singleflight.find(singleflight_key);
                if (found != impl_->singleflight.end()) {
                    singleflight_state = found->second;
                    std::lock_guard<std::mutex> state_lock(singleflight_state->mutex);
                    if (singleflight_state->waiters >= policy.max_pending) {
                        throw asio::system_error(asio::error::no_buffer_space);
                    }
                    ++singleflight_state->waiters;
                } else {
                    singleflight_state = std::make_shared<SingleFlightState>();
                    impl_->singleflight.emplace(singleflight_key, singleflight_state);
                    singleflight_leader = true;
                }
            }

            if (!singleflight_leader) {
                const auto executor = co_await asio::this_coro::executor;
                asio::steady_timer poll(executor);
                const auto deadline =
                    std::chrono::steady_clock::now() + policy.queue_timeout;
                for (;;) {
                    {
                        std::lock_guard<std::mutex> lock(singleflight_state->mutex);
                        if (singleflight_state->completed) {
                            co_return singleflight_state->result;
                        }
                    }
                    if (context.cancel_token && context.cancel_token->is_cancelled()) {
                        throw graph::CancelledException("while waiting for single-flight result");
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) {
                        throw asio::system_error(asio::error::timed_out);
                    }
                    poll.expires_after(std::min(
                        std::chrono::milliseconds{1},
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - now)));
                    asio::error_code wait_error;
                    co_await poll.async_wait(
                        asio::redirect_error(asio::use_awaitable, wait_error));
                    if (wait_error && wait_error != asio::error::operation_aborted) {
                        throw asio::system_error(wait_error);
                    }
                }
            }
        }


        auto invoke = [&tool, &policy, &arguments, &context,
                       &started]() -> asio::awaitable<std::string> {
            if (context.cancel_token) {
                context.cancel_token->throw_if_cancelled("before tool execution");
            }
            started = true;
            if (policy.implementation == ToolExecutionImplementation::BlockingProcess) {
                co_return co_await detail::execute_process_tool_async(
                    tool, arguments, context, policy.output_limit_bytes,
                    policy.execution_timeout);
            }
            // ContextualAsyncTool has no safe synchronous fallback by design:
            // its context-bearing entry point is the contract it opted into.
            if (auto* contextual = dynamic_cast<ContextualAsyncTool*>(&tool)) {
                co_return co_await contextual->execute_async(arguments, context);
            }
            if (policy.implementation == ToolExecutionImplementation::BlockingThread) {
                co_return co_await detail::execute_blocking_tool_async(tool, arguments);
            }
            co_return co_await tool.execute_async(arguments);
        };

        auto check_output = [&policy](std::string value) -> std::string {
            if (value.size() > policy.output_limit_bytes) {
                throw std::length_error("tool result exceeds policy output_limit_bytes");
            }
            return value;
        };

        auto invoke_with_timeout = [&]() -> asio::awaitable<std::string> {
            if (policy.execution_timeout <= std::chrono::milliseconds::zero()
                || policy.implementation != ToolExecutionImplementation::NativeAsync
                || !policy.cancellable) {
                co_return co_await invoke();
            }
            using asio::experimental::awaitable_operators::operator||;
            const auto executor = co_await asio::this_coro::executor;
            asio::steady_timer timer(executor);
            timer.expires_after(policy.execution_timeout);
            try {
                auto result = co_await (
                    invoke() || timer.async_wait(asio::use_awaitable));
                if (result.index() == 1) {
                    throw asio::system_error(asio::error::timed_out,
                                             "tool execution deadline expired");
                }
                co_return std::get<0>(std::move(result));
            } catch (const asio::multiple_exceptions&) {
                throw asio::system_error(asio::error::timed_out,
                                         "tool execution deadline expired");
            }
        };
        auto invoke_with_retry = [&]() -> asio::awaitable<std::string> {
            const auto max_attempts = std::max<std::uint32_t>(1, policy.retry_max_attempts);
            std::exception_ptr last_error;
            for (std::uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
                std::optional<std::string> output;
                try {
                    output.emplace(co_await invoke_with_timeout());
                } catch (const graph::CancelledException&) {
                    throw;
                } catch (const asio::system_error& error) {
                    if (error.code() == asio::error::operation_aborted) throw;
                    last_error = std::current_exception();
                } catch (...) {
                    last_error = std::current_exception();
                }
                if (output) co_return std::move(*output);
                if (attempt + 1 >= max_attempts) {
                    std::rethrow_exception(last_error);
                }
                if (policy.retry_backoff > std::chrono::milliseconds::zero()) {
                    auto executor = co_await asio::this_coro::executor;
                    asio::steady_timer backoff(executor);
                    backoff.expires_after(policy.retry_backoff);
                    co_await backoff.async_wait(asio::use_awaitable);
                }
            }
            throw std::logic_error("tool retry loop exhausted without a result");
        };

        const auto queue_deadline = std::chrono::steady_clock::now() + policy.queue_timeout;
        const auto host_admission = impl_->host_admission;
        auto reserve_host = [host_admission, &policy, &tool, &identity, &context,
                             queue_deadline]() -> asio::awaitable<HostResourceLease> {
            const auto now = std::chrono::steady_clock::now();
            if (now >= queue_deadline) throw asio::system_error(asio::error::timed_out);

            auto request = host_request_for(policy, tool.get_name(), identity);
            const auto inherited = std::max(context.requested_priority,
                                             context.inherited_priority);
            request.priority = std::min(policy.priority_ceiling,
                                        std::max(request.priority, inherited));
            request.fairness_weight = policy.fairness_weight;
            request.queue_timeout = std::min(
                request.queue_timeout,
                std::max(std::chrono::milliseconds{1},
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             queue_deadline - now)));
            co_return co_await host_admission->reserve_async(
                std::move(request), context.cancel_token);
        };

        if (policy.concurrency == ToolConcurrency::Reentrant) {
            std::optional<HostResourceLease> host_lease;
            if (host_admission) host_lease.emplace(co_await reserve_host());
            if (context.cancel_token) {
                context.cancel_token->throw_if_cancelled("after tool resource admission");
            }
            ToolExecutionResult result{
                ToolTerminalStatus::Succeeded,
                check_output(co_await invoke_with_retry()), {}, false, false};
            if (policy.implementation == ToolExecutionImplementation::BlockingProcess) {
                result.exit_code = 0;
            }
            co_return publish_singleflight(std::move(result));
        }

        ResourceRequest request;
        request.resource_key =
            policy.concurrency == ToolConcurrency::Exclusive
                ? "tool:" + required_identity_component(tool.get_name(), "tool")
                : derive_resource_key(policy, tool.get_name(), arguments, identity);
        request.owner_scope = identity.owner_scope.empty() ? "anonymous" : identity.owner_scope;
        const auto keyed = policy.concurrency == ToolConcurrency::KeyedExclusive
                        || policy.concurrency == ToolConcurrency::Exclusive
                        || policy.concurrency == ToolConcurrency::SingleFlight;
        request.capacity = keyed ? 1 : policy.capacity;
        request.max_pending = policy.max_pending;
        request.queue_timeout = policy.queue_timeout;
        request.priority = std::min(
            policy.priority_ceiling,
            std::max(policy.host_priority,
                     std::max(context.requested_priority, context.inherited_priority)));
        request.fairness_weight = policy.fairness_weight;

        auto lease = co_await impl_->arbiter->acquire_async(std::move(request),
                                                             context.cancel_token);
        if (context.cancel_token) {
            context.cancel_token->throw_if_cancelled("after tool resource admission");
        }

        std::optional<HostResourceLease> host_lease;
        if (host_admission) host_lease.emplace(co_await reserve_host());
        if (context.cancel_token) {
            context.cancel_token->throw_if_cancelled("after host resource admission");
        }
        ToolExecutionResult result{
            ToolTerminalStatus::Succeeded,
            check_output(co_await invoke_with_retry()), {}, false, false};
        if (policy.implementation == ToolExecutionImplementation::BlockingProcess) {
            result.exit_code = 0;
        }
        co_return publish_singleflight(std::move(result));
    } catch (const graph::CancelledException& error) {
        co_return publish_singleflight(ToolExecutionResult{
            started ? ToolTerminalStatus::CancellationRequested
                    : ToolTerminalStatus::CancelledBeforeStart,
            {}, error.what(), false, started && policy.effect != ToolEffectClass::ReadOnly});
    } catch (const asio::system_error& error) {
        const auto code = error.code();
        const auto queue_failure = code == asio::error::timed_out
                                || code == asio::error::no_buffer_space;
        const auto status = code == asio::error::timed_out
                          ? (started ? ToolTerminalStatus::TimedOut
                                     : ToolTerminalStatus::Expired)
                          : (code == asio::error::operation_aborted
                                 ? (started ? ToolTerminalStatus::Killed
                                            : ToolTerminalStatus::CancelledBeforeStart)
                                 : ToolTerminalStatus::Rejected);
        const auto uncertain = started
            && policy.effect != ToolEffectClass::ReadOnly
            && policy.effect != ToolEffectClass::ExternalRead
            && policy.idempotency != ToolIdempotency::Idempotent;
        co_return publish_singleflight(ToolExecutionResult{
            status, {}, error.what(),
            queue_failure && policy.idempotency == ToolIdempotency::Idempotent, uncertain});
    } catch (const detail::ProcessBridgeError& error) {
        co_return publish_singleflight(ToolExecutionResult{
            error.status, std::move(error.output), error.what(), false,
            error.effect_uncertain, error.exit_code, error.signal_number,
            error.output_truncated});
    } catch (const std::length_error& error) {
        co_return publish_singleflight(ToolExecutionResult{
            ToolTerminalStatus::Failed, {}, error.what(), false, false});
    } catch (const std::exception& error) {
        const auto uncertain = started
            && policy.effect != ToolEffectClass::ReadOnly
            && policy.effect != ToolEffectClass::ExternalRead
            && policy.idempotency != ToolIdempotency::Idempotent;
        co_return publish_singleflight(ToolExecutionResult{
            uncertain ? ToolTerminalStatus::ReconciliationRequired
                      : ToolTerminalStatus::Failed,
            {}, error.what(), false, uncertain});
    } catch (...) {
        co_return publish_singleflight(ToolExecutionResult{
            started ? ToolTerminalStatus::ReconciliationRequired
                    : ToolTerminalStatus::Failed,
            {}, "tool execution failed with an unknown exception", false, started});
    }
}

asio::awaitable<std::string> ToolExecutionController::execute_async(
    Tool& tool, json arguments, ToolExecutionContext context) {
    const auto result = co_await execute_result_async(tool, std::move(arguments),
                                                       std::move(context));
    if (result.succeeded()) co_return result.output;
    switch (result.status) {
        case ToolTerminalStatus::CancelledBeforeStart:
        case ToolTerminalStatus::CancellationRequested:
            throw graph::CancelledException(result.error);
        case ToolTerminalStatus::Expired:
        case ToolTerminalStatus::TimedOut:
            throw asio::system_error(asio::error::timed_out);
        case ToolTerminalStatus::Rejected:
            throw asio::system_error(asio::error::no_buffer_space);
        default:
            throw std::runtime_error(result.error.empty()
                                         ? "tool execution failed"
                                         : result.error);
    }
}


std::shared_ptr<ToolExecutionPolicyRegistry> ToolExecutionController::policies() const {
    return impl_->policies;
}

std::shared_ptr<ResourceArbiter> ToolExecutionController::arbiter() const {
    return impl_->arbiter;
}

std::shared_ptr<HostAdmissionController> ToolExecutionController::host_admission() const {
    return impl_->host_admission;
}

std::shared_ptr<ToolExecutionController> default_tool_execution_controller() {
    static const auto controller = std::make_shared<ToolExecutionController>();
    return controller;
}

} // namespace neograph
