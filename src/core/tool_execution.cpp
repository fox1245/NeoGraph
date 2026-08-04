#include <neograph/tool_execution.h>

#include <neograph/graph/cancel.h>
#include <neograph/tool.h>

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

namespace neograph {
namespace {

constexpr std::size_t kMaximumResourceKeyBytes = 512;
constexpr std::size_t kMaximumResourceComponentBytes = 256;
constexpr std::size_t kMaximumPolicyQueueDepth = 65'536;
constexpr std::size_t kMaximumOutputBytes = 64U * 1024U * 1024U;

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
    if (output_limit_bytes == 0 || output_limit_bytes > kMaximumOutputBytes) {
        throw std::invalid_argument("tool execution output_limit_bytes must be 1..64MiB");
    }
    if (concurrency == ToolConcurrency::KeyedExclusive && capacity != 1) {
        throw std::invalid_argument("keyed-exclusive tool policy must have capacity 1");
    }
    if (concurrency == ToolConcurrency::Capacity && capacity == 0) {
        throw std::invalid_argument("capacity tool policy must have positive capacity");
    }
    if (concurrency != ToolConcurrency::Reentrant) {
        validate_resource_template(resource_key_template);
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

class ResourceArbiterImpl final : public std::enable_shared_from_this<ResourceArbiterImpl> {
public:
    struct Waiter {
        enum class State : std::uint8_t { Queued, Granted, Claimed, Cancelled };

        std::string resource_key;
        State state = State::Queued;
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

    void release_one_locked(const std::string& resource_key) noexcept {
        const auto found = resources_.find(resource_key);
        if (found == resources_.end() || found->second.active == 0) return;
        auto& resource = found->second;
        --resource.active;
        if (stopping_) return;
        while (resource.active < resource.capacity && !resource.waiters.empty()) {
            auto waiter = std::move(resource.waiters.front());
            resource.waiters.pop_front();
            if (waiter->state != Waiter::State::Queued) continue;
            waiter->state = Waiter::State::Granted;
            ++resource.active;
        }
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, Resource> resources_;
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
};

ToolExecutionController::ToolExecutionController(ToolExecutionControllerConfig config)
    : impl_(std::make_unique<Impl>()) {
    impl_->policies = config.policies ? std::move(config.policies)
                                      : std::make_shared<ToolExecutionPolicyRegistry>();
    impl_->arbiter = config.arbiter ? std::move(config.arbiter)
                                    : std::make_shared<ResourceArbiter>();
}

ToolExecutionController::~ToolExecutionController() = default;

asio::awaitable<std::string> ToolExecutionController::execute_async(
    Tool& tool, json arguments, ToolExecutionContext context) {
    const auto policy = impl_->policies->resolve(tool.get_name());
    policy.validate();

    auto invoke = [&tool, &policy, &arguments, &context]() -> asio::awaitable<std::string> {
        // ContextualAsyncTool has no safe synchronous fallback by design: its
        // context-bearing entry point is the contract it opted into.
        if (auto* contextual = dynamic_cast<ContextualAsyncTool*>(&tool)) {
            co_return co_await contextual->execute_async(arguments, std::move(context));
        }
        if (policy.implementation == ToolExecutionImplementation::BlockingThread) {
            co_return co_await detail::execute_blocking_tool_async(tool, std::move(arguments));
        }
        co_return co_await tool.execute_async(arguments);
    };

    auto check_output = [&policy](std::string value) -> std::string {
        if (value.size() > policy.output_limit_bytes) {
            throw std::length_error("tool result exceeds policy output_limit_bytes");
        }
        return value;
    };

    if (policy.concurrency == ToolConcurrency::Reentrant) {
        co_return check_output(co_await invoke());
    }

    ResourceRequest request;
    request.resource_key = derive_resource_key(policy, tool.get_name(), arguments, context.identity);
    request.owner_scope = context.identity.owner_scope.empty() ? "anonymous" : context.identity.owner_scope;
    request.capacity = policy.concurrency == ToolConcurrency::KeyedExclusive ? 1 : policy.capacity;
    request.max_pending = policy.max_pending;
    request.queue_timeout = policy.queue_timeout;

    auto lease = co_await impl_->arbiter->acquire_async(std::move(request), context.cancel_token);
    if (context.cancel_token) {
        context.cancel_token->throw_if_cancelled("after tool resource admission");
    }
    co_return check_output(co_await invoke());
}

std::shared_ptr<ToolExecutionPolicyRegistry> ToolExecutionController::policies() const {
    return impl_->policies;
}

std::shared_ptr<ResourceArbiter> ToolExecutionController::arbiter() const {
    return impl_->arbiter;
}

std::shared_ptr<ToolExecutionController> default_tool_execution_controller() {
    static const auto controller = std::make_shared<ToolExecutionController>();
    return controller;
}

} // namespace neograph
