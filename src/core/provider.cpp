#include <neograph/provider.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/provider_call_broker.h>
#include "canonical_json.h"
#include "provider_wake.h"
#include <json/json.h>
#include <core/native.h>

#include <asio/cancellation_state.hpp>
#include <type_traits>
#include <utility>
#include <asio/system_error.hpp>
#include <asio/this_coro.hpp>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <stop_token>
#include <atomic>
#include <memory_resource>
#include <new>

namespace neograph {
namespace {
sp::runtime::Result failure(sp::Error error) {
    return std::make_shared<const sp::Outcome>(sp::Failure{std::move(error), {}});
}
sp::Error local_error(sp::ErrorKind kind, std::string message) {
    sp::Error error;
    error.kind = kind;
    error.safe_message = std::move(message);
    error.retry_safety = sp::RetrySafety::NotSent;
    return error;
}
std::size_t size_sum(std::size_t a, std::size_t b) noexcept {
    return b > std::numeric_limits<std::size_t>::max() - a
        ? std::numeric_limits<std::size_t>::max() : a + b;
}
std::size_t size_product(std::size_t a, std::size_t b) noexcept {
    return a && b > std::numeric_limits<std::size_t>::max() / a
        ? std::numeric_limits<std::size_t>::max() : a * b;
}
struct ForwardStop {
    std::stop_source source;
    void operator()() noexcept { source.request_stop(); }
};
struct StopChannel {
    std::stop_source source;
    std::optional<std::stop_callback<ForwardStop>> external;
};
const sp::json::Document* event_document(const sp::Event& event) noexcept {
    return std::visit([](const auto& value) -> const sp::json::Document* {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, sp::RawWire> || std::is_same_v<T, sp::ResponseEnvelope>)
            return value.payload.get();
        else if constexpr (std::is_same_v<T, sp::PartBegin>) return value.header.wire_metadata.get();
        else if constexpr (std::is_same_v<T, sp::PartSeal>) return value.wire_metadata.get();
        else if constexpr (std::is_same_v<T, sp::MessageSeal>) return value.wire_output.get();
        else if constexpr (std::is_same_v<T, sp::Stop>) return value.reason.details.get();
        else return nullptr;
    }, event);
}
std::size_t event_member_bytes(const sp::Event& event) noexcept {
    return std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        std::size_t bytes = 0;
        auto text = [&](const std::string& string) { bytes = size_sum(bytes, size_sum(string.capacity(), 1)); };
        auto optional_text = [&](const auto& string) { if (string) text(*string); };
        if constexpr (std::is_same_v<T, sp::Begin>) text(value.generation);
        else if constexpr (std::is_same_v<T, sp::MessageBegin>) optional_text(value.vendor_id);
        else if constexpr (std::is_same_v<T, sp::PartBegin>) {
            text(value.header.wire_id); text(value.header.name); text(value.header.wire_type);
        } else if constexpr (std::is_same_v<T, sp::UsageUpdate>) {
            bytes = size_product(value.snapshot.conflicts.capacity(), sizeof(sp::UsageConflict));
            for (const auto& conflict : value.snapshot.conflicts) { text(conflict.counter); text(conflict.detail); }
            for (const auto& entry : value.snapshot.extra) {
                bytes = size_sum(bytes, sizeof(entry) + 4 * sizeof(void*));
                text(entry.first);
            }
        } else if constexpr (std::is_same_v<T, sp::Stop>) {
            text(value.reason.raw); optional_text(value.reason.sequence);
        } else if constexpr (std::is_same_v<T, sp::Commit>) text(value.evidence);
        else if constexpr (std::is_same_v<T, sp::Fail>) { text(value.error.safe_message); text(value.error.vendor_code); }
        else if constexpr (std::is_same_v<T, sp::RawWire>) text(value.type);
        return bytes;
    }, event);
}
std::string_view event_borrowed_payload(const sp::Event& event) noexcept {
    if (const auto* delta = std::get_if<sp::PartDelta>(&event)) return delta->payload.bytes;
    if (const auto* seal = std::get_if<sp::PartSeal>(&event); seal && seal->snapshot) return *seal->snapshot;
    return {};
}
// SDK delta/snapshot bytes are borrowed only during its callback. Allocate the
// final owner before installing views, so even small-string storage stays put.
struct OwnedEvent {
    sp::Event event;
    std::string bytes;
    std::size_t charge = 0;
    explicit OwnedEvent(const sp::Event& source) : event(source), bytes(event_borrowed_payload(source)) {
        if (auto* delta = std::get_if<sp::PartDelta>(&event)) delta->payload.bytes = bytes;
        else if (auto* seal = std::get_if<sp::PartSeal>(&event); seal && seal->snapshot)
            seal->snapshot = std::string_view(bytes);
    }
};
// Container capacity is ownership too, including buckets and vectors currently
// draining on the outer executor. Allocation happens under Bridge::mutex; a
// draining vector may free outside it, so only this independent counter is atomic.
class QueueMemoryResource final : public std::pmr::memory_resource {
public:
    QueueMemoryResource(const std::size_t& maximum, const std::size_t& retained)
        : maximum_(maximum), retained_(retained) {}
    std::size_t allocated_bytes() const noexcept { return allocated_.load(std::memory_order_relaxed); }
private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        const auto used = size_sum(retained_, allocated_bytes());
        if (used > maximum_ || bytes > maximum_ - used) throw std::bad_alloc{};
        auto* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        allocated_.fetch_add(bytes, std::memory_order_relaxed);
        return result;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
        allocated_.fetch_sub(bytes, std::memory_order_relaxed);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
    const std::size_t& maximum_;
    const std::size_t& retained_;
    std::atomic<std::size_t> allocated_{0};
};

struct Bridge {
    struct RetainedDocument { std::size_t references = 0, bytes = 0; };
    std::mutex mutex;
    bool abandoned = false, resource_failure = false;
    std::exception_ptr resource_exception;
    std::size_t max_events = 0, max_bytes = 0, retained_events = 0, retained_bytes = 0;
    QueueMemoryResource memory{max_bytes, retained_bytes};
    std::pmr::vector<std::unique_ptr<OwnedEvent>> events{&memory};
    std::pmr::unordered_map<const sp::json::Document*, RetainedDocument> documents{&memory};
    std::shared_ptr<sp::runtime::Client> client;
    std::shared_ptr<StopChannel> stop;
    std::shared_ptr<detail::ProviderWakeSignal> wake;
    sp::runtime::Result result;

    void retain(const sp::Event& source) {
        bool exhausted = false;
        {
            std::lock_guard lock(mutex);
            if (abandoned || resource_failure) return;
            const auto* document = event_document(source);
            auto entry = documents.find(document);
            const bool new_document = document && entry == documents.end();
            const auto document_bytes = new_document ? size_sum(document->retained_bytes(),
                sizeof(std::shared_ptr<const sp::json::Document>)) : 0;
            const auto fixed = sizeof(OwnedEvent);
            const auto estimate = size_sum(size_sum(fixed, event_member_bytes(source)),
                size_sum(event_borrowed_payload(source).size(), sizeof(std::string)));
            const auto used = size_sum(retained_bytes, memory.allocated_bytes());
            const auto available = used <= max_bytes ? max_bytes - used : 0;
            if (retained_events >= max_events || used > max_bytes || document_bytes > available ||
                estimate > available - document_bytes) exhausted = true;
            else {
                bool inserted = false;
                auto charge = estimate + document_bytes;
                // Reserve owned data before allocation so container growth sees
                // the complete simultaneous queued+draining ownership window.
                retained_bytes += charge;
                try {
                    auto owned = std::make_unique<OwnedEvent>(source);
                    owned->charge = size_sum(size_sum(fixed, event_member_bytes(owned->event)),
                        size_sum(owned->bytes.capacity(), 1));
                    if (owned->charge > estimate) throw std::bad_alloc{};
                    retained_bytes -= estimate - owned->charge;
                    charge = owned->charge + document_bytes;
                    if (new_document) {
                        entry = documents.emplace(document, RetainedDocument{0, document_bytes}).first;
                        inserted = true;
                    }
                    events.push_back(std::move(owned));
                    if (document) ++entry->second.references;
                    ++retained_events;
                } catch (...) {
                    if (inserted) documents.erase(document);
                    retained_bytes -= charge;
                    resource_exception = std::current_exception();
                    exhausted = true;
                }
            }
            if (exhausted) resource_failure = true;
        }
        // Cancellation callbacks run outside the queue lock. No SDK callback
        // waits for the outer executor or invokes its consumer.
        if (exhausted) stop->source.request_stop();
    }
    void release_locked(std::unique_ptr<OwnedEvent>& event) noexcept {
        if (!event) return;
        retained_bytes -= event->charge;
        --retained_events;
        if (const auto* document = event_document(event->event)) {
            const auto entry = documents.find(document);
            if (--entry->second.references == 0) {
                retained_bytes -= entry->second.bytes;
                documents.erase(entry);
            }
        }
        event.reset();
    }
    void release(std::unique_ptr<OwnedEvent>& event) noexcept {
        std::lock_guard lock(mutex);
        release_locked(event);
    }
    void release(std::pmr::vector<std::unique_ptr<OwnedEvent>>& batch) noexcept {
        std::lock_guard lock(mutex);
        for (auto& event : batch) release_locked(event);
    }
    void abandon() noexcept {
        std::lock_guard lock(mutex);
        abandoned = true;
        if (wake) wake->stop();
        for (auto& event : events) release_locked(event);
        events.clear();
    }
};
struct DispatchHooks {
    std::function<void(const PreparedProviderRequest&)> before;
    std::function<void(sp::runtime::Result)> after;
    std::function<void(const sp::Event&)> event;
    std::function<void(std::exception_ptr)> on_error;
    std::shared_ptr<DispatchHooks> next;
};
template<class Function>
void observe_hooks(const std::shared_ptr<DispatchHooks>& hooks, Function&& invoke) noexcept {
    for (auto current = hooks; current; current = current->next) {
        try { invoke(*current); } catch (...) {}
    }
}
}

struct PreparedProviderRequest::Impl {
    std::shared_ptr<sp::runtime::Client> client;
    std::function<asio::awaitable<sp::runtime::Result>(
        const PreparedProviderRequest&, const std::function<void(const sp::Event&)>&)> local_dispatch;
    sp::runtime::PreparedRequest prepared;
    ProviderMode mode;
    std::shared_ptr<graph::CancelToken> cancel_token;
    std::function<void(const sp::Event&)> on_event;
    ProviderObserverLimits observer_limits;
    std::optional<sp::Error> error;
    std::string digest;
    std::shared_ptr<DispatchHooks> hooks;
    std::shared_ptr<StopChannel> stop;
    std::size_t event_count_limit = 0, event_byte_limit = 0;
};
PreparedProviderRequest::PreparedProviderRequest() = default;
PreparedProviderRequest::PreparedProviderRequest(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PreparedProviderRequest::~PreparedProviderRequest() = default;
PreparedProviderRequest::PreparedProviderRequest(PreparedProviderRequest&&) noexcept = default;
PreparedProviderRequest& PreparedProviderRequest::operator=(PreparedProviderRequest&&) noexcept = default;
bool PreparedProviderRequest::valid() const noexcept { return impl_ && impl_->prepared.valid() && !error(); }
const sp::Error* PreparedProviderRequest::error() const noexcept {
    if (!impl_) return nullptr;
    return impl_->error ? &*impl_->error : impl_->prepared.error();
}
std::string_view PreparedProviderRequest::family() const noexcept { return impl_ ? impl_->prepared.family() : std::string_view{}; }
std::string_view PreparedProviderRequest::model() const noexcept { return impl_ ? impl_->prepared.model() : std::string_view{}; }
std::string_view PreparedProviderRequest::encoded_body() const noexcept { return impl_ ? impl_->prepared.encoded_body() : std::string_view{}; }
sp::runtime::SteadyTime PreparedProviderRequest::deadline() const noexcept { return impl_ ? impl_->prepared.deadline() : sp::runtime::SteadyTime{}; }
ProviderMode PreparedProviderRequest::mode() const noexcept { return impl_ ? impl_->mode : ProviderMode::Collect; }
bool PreparedProviderRequest::is_cancelled() const noexcept {
    return impl_ && impl_->cancel_token && impl_->cancel_token->is_cancelled();
}
std::optional<std::uint64_t> PreparedProviderRequest::max_output_tokens() const noexcept {
    return impl_ ? impl_->prepared.max_output_tokens() : std::nullopt;
}
bool PreparedProviderRequest::requires_native_custody() const noexcept {
    const auto* context = impl_ ? impl_->prepared.native_context() : nullptr;
    return context && context->replay_eligible() && !impl_->local_dispatch;
}
const sp::descriptor::ValidatedDescriptor* PreparedProviderRequest::admitted_descriptor() const noexcept {
    return impl_ ? impl_->prepared.descriptor() : nullptr;
}

PreparedProviderRequest Provider::prepare_runtime(std::shared_ptr<sp::runtime::Client> client, ProviderRequest request) {
    sp::runtime::require_interface_contract(sp::EXPECTED_INTERFACE_REVISION, sp::capability::RequiredProvider);
    if (!client) throw std::invalid_argument("Provider runtime is unavailable");
    auto impl = std::make_unique<PreparedProviderRequest::Impl>();
    impl->client = std::move(client);
    impl->mode = request.mode;
    impl->cancel_token = std::move(request.cancel_token);
    impl->on_event = std::move(request.on_event);
    impl->observer_limits = request.observer_limits;
    request.options.streaming = request.mode == ProviderMode::Stream;
    if (request.mode != ProviderMode::Collect && request.mode != ProviderMode::Stream)
        impl->error = local_error(sp::ErrorKind::InvalidRequest, "Invalid provider mode");
    else if ((request.observer_limits.max_events && !*request.observer_limits.max_events) ||
             (request.observer_limits.max_bytes && !*request.observer_limits.max_bytes))
        impl->error = local_error(sp::ErrorKind::InvalidRequest, "Observer limits must be positive");
    else if (impl->cancel_token && impl->cancel_token->is_cancelled())
        impl->error = local_error(sp::ErrorKind::Cancelled, "Provider request cancelled before admission");
    else {
        try {
            impl->stop = std::make_shared<StopChannel>();
            if (request.options.stop_token.stop_possible())
                impl->stop->external.emplace(request.options.stop_token, ForwardStop{impl->stop->source});
            request.options.stop_token = impl->stop->source.get_token();
            impl->prepared = impl->client->prepare(std::move(request.payload), std::move(request.options));
        }
        catch (const sp::runtime::AdmissionError& error) {
            impl->error = std::get<sp::Failure>(*error.outcome()).error;
        }
    }
    PreparedProviderRequest result(std::move(impl));
    if (result.valid()) {
        const auto* limits = result.impl_->prepared.limits();
        const auto* descriptor = result.impl_->prepared.descriptor();
        if (!limits || !descriptor) throw std::invalid_argument("Provider preparation has no admitted resource policy");
        const auto event_floor = sizeof(OwnedEvent) + sizeof(std::unique_ptr<OwnedEvent>);
        result.impl_->event_count_limit = std::max<std::size_t>(1, limits->queued_body_bytes / event_floor);
        // Queue bytes count ownership, not transport chunks. Native history's
        // global archive ceiling cannot become a per-operation observer grant.
        auto bytes = sp::json::retained_size_bound(limits->max_response_bytes);
        const auto native_source = std::min(descriptor->policy()->resources().native_bytes,
            size_sum(limits->max_response_bytes, result.encoded_body().size()));
        bytes = size_sum(bytes, sp::json::retained_size_bound(native_source));
        bytes = size_sum(bytes, limits->semantic.max_content_bytes);
        bytes = size_sum(bytes, limits->semantic.max_tool_bytes);
        bytes = size_sum(bytes, limits->max_response_bytes);
        bytes = size_sum(bytes, size_product(result.impl_->event_count_limit,
            event_floor + sizeof(Bridge::RetainedDocument) + 4 * sizeof(void*)));
        result.impl_->event_byte_limit = bytes;
        const auto& selected = result.impl_->observer_limits;
        if ((selected.max_events && *selected.max_events > result.impl_->event_count_limit) ||
            (selected.max_bytes && *selected.max_bytes > result.impl_->event_byte_limit)) {
            result.impl_->error = local_error(sp::ErrorKind::InvalidRequest,
                "Observer limit exceeds admitted resources");
            return result;
        }
        if (selected.max_events) result.impl_->event_count_limit = *selected.max_events;
        if (selected.max_bytes) result.impl_->event_byte_limit = *selected.max_bytes;
        result.impl_->digest = request_digest(result);
    }
    return result;
}
PreparedProviderRequest Provider::prepare_local(
    std::shared_ptr<sp::runtime::Client> client, ProviderRequest request, LocalDispatch dispatch) {
    if (!dispatch) throw std::invalid_argument("Local provider dispatch must be present");
    auto prepared = prepare_runtime(std::move(client), std::move(request));
    prepared.impl_->local_dispatch = std::move(dispatch);
    return prepared;
}
PreparedProviderRequest Provider::reject_preparation(sp::Error error) {
    auto impl = std::make_unique<PreparedProviderRequest::Impl>();
    impl->mode = ProviderMode::Collect;
    error.retry_safety = sp::RetrySafety::NotSent;
    impl->error = std::move(error);
    return PreparedProviderRequest(std::move(impl));
}
PreparedProviderRequest Provider::observe_prepared(
    PreparedProviderRequest request, std::function<void(const PreparedProviderRequest&)> before,
    std::function<void(sp::runtime::Result)> after, std::function<void(const sp::Event&)> event,
    std::function<void(std::exception_ptr)> on_error) noexcept {
    if (!request.impl_) return request;
    try {
        auto hooks = std::make_shared<DispatchHooks>();
        hooks->before = std::move(before);
        hooks->after = std::move(after);
        hooks->event = std::move(event);
        hooks->on_error = std::move(on_error);
        hooks->next = std::move(request.impl_->hooks);
        request.impl_->hooks = std::move(hooks);
    } catch (...) {}
    return request;
}
std::string Provider::request_digest(const PreparedProviderRequest& request) {
    if (!request.valid()) throw std::invalid_argument("Cannot digest an invalid provider preparation");
    if (!request.impl_->digest.empty()) return request.impl_->digest;
    const auto* descriptor = request.impl_->prepared.descriptor();
    const auto* retry = request.impl_->prepared.retry_policy();
    if (!descriptor || !retry) throw std::invalid_argument("Provider preparation has no admitted binding");
    json headers = json::array();
    for (const auto& [name, value] : descriptor->headers()) headers.push_back({name, value});
    const auto policy_identity = descriptor->policy()->identity();
    char policy_hex[64];
    if (policy_identity.size() != sizeof(policy_hex) / 2)
        throw std::invalid_argument("Provider preparation has no valid policy identity");
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < policy_identity.size(); ++i) {
        const auto byte = static_cast<unsigned char>(policy_identity[i]);
        policy_hex[2 * i] = digits[byte >> 4];
        policy_hex[2 * i + 1] = digits[byte & 15];
    }
    json stop_mappings = json::array();
    for (const auto& [name, kind] : descriptor->stop_mappings())
        stop_mappings.push_back({name, static_cast<unsigned>(kind)});
    return detail::sha256_identity("NeoGraph Provider dispatch identity v1", "provider-request/v3",
        detail::canonical_json_bytes(json{
            {"family", request.family()}, {"model", request.model()},
            {"mode", request.mode() == ProviderMode::Stream ? "stream" : "collect"},
            {"descriptor", {{"id", descriptor->id()}, {"revision", descriptor->revision()},
                {"origin", descriptor->base_url()}, {"buffered_path", descriptor->path(false)},
                {"streaming_path", descriptor->path(true)}, {"headers", std::move(headers)},
                {"policy", std::string_view(policy_hex, sizeof(policy_hex))},
                {"model_member", descriptor->request_model_member()},
                {"messages_member", descriptor->request_messages_member()},
                {"stream_member", descriptor->request_stream_member()},
                {"output_cap_member", descriptor->max_output_tokens_member()},
                {"usage_path", descriptor->usage_path()}, {"stop_mappings", std::move(stop_mappings)}}},
            {"retry", {{"enabled", retry->enabled},
                {"allow_duplicate_billing_risk", retry->allow_duplicate_billing_risk},
                {"max_attempts", retry->max_attempts}, {"base_delay_ms", retry->base_delay.count()},
                {"max_delay_ms", retry->max_delay.count()}}},
            {"observer_limits", {{"max_events", request.impl_->event_count_limit},
                {"max_bytes", request.impl_->event_byte_limit}}},
            {"encoded_body", request.encoded_body()}}));
}
asio::awaitable<sp::runtime::Result> Provider::dispatch_async(PreparedProviderRequest request) {
    return dispatch_impl(std::move(request));
}
asio::awaitable<sp::runtime::Result> Provider::invoke_async(ProviderRequest request) {
    return dispatch_impl(prepare(std::move(request)));
}
sp::runtime::Result Provider::dispatch(PreparedProviderRequest request) {
    return async::run_sync(dispatch_async(std::move(request)));
}
sp::runtime::Result Provider::invoke(ProviderRequest request) { return dispatch(prepare(std::move(request))); }

asio::awaitable<sp::runtime::Result> Provider::dispatch_impl(PreparedProviderRequest request) {
    auto hooks = request.valid() && request.impl_ ? request.impl_->hooks : nullptr;
    observe_hooks(hooks, [&](DispatchHooks& hook) { if (hook.before) hook.before(request); });
    const bool throw_on_cancel = co_await asio::this_coro::throw_if_cancelled();
    co_await asio::this_coro::throw_if_cancelled(false);
    sp::runtime::Result result;
    std::exception_ptr error;
    try { result = co_await dispatch_operation(std::move(request)); }
    catch (...) { error = std::current_exception(); }
    co_await asio::this_coro::throw_if_cancelled(throw_on_cancel);
    if (error) {
        observe_hooks(hooks, [&](DispatchHooks& hook) { if (hook.on_error) hook.on_error(error); });
        std::rethrow_exception(error);
    }
    observe_hooks(hooks, [&](DispatchHooks& hook) { if (hook.after) hook.after(result); });
    co_return result;
}
asio::awaitable<sp::runtime::Result> Provider::dispatch_operation(PreparedProviderRequest request) {
    if (const auto* error = request.error()) co_return failure(*error);
    if (!request.valid()) co_return failure(local_error(sp::ErrorKind::Misuse, "Invalid provider preparation"));
    auto impl = std::move(request.impl_);
    if (impl->cancel_token && impl->cancel_token->is_cancelled())
        co_return failure(local_error(sp::ErrorKind::Cancelled, "Provider request cancelled before dispatch"));
    auto executor = co_await asio::this_coro::executor;
    const auto initial_cancellation = co_await asio::this_coro::cancellation_state;
    if (initial_cancellation.cancelled() != asio::cancellation_type::none)
        co_return failure(local_error(sp::ErrorKind::Cancelled, "Provider request cancelled before dispatch"));
    if (std::chrono::steady_clock::now() >= impl->prepared.deadline())
        co_return failure(local_error(sp::ErrorKind::DeadlineExceeded, "Provider preparation deadline expired"));
    if (impl->local_dispatch) {
        auto dispatch = std::move(impl->local_dispatch);
        auto observer = [primary = std::move(impl->on_event), hooks = impl->hooks](const sp::Event& event) {
            observe_hooks(hooks, [&](DispatchHooks& hook) { if (hook.event) hook.event(event); });
            if (primary) primary(event);
        };
        request.impl_ = std::move(impl);
        auto result = co_await dispatch(request, observer);
        if (!result) throw std::invalid_argument("Local provider returned no owned outcome");
        co_return result;
    }
    detail::ProviderWakeReader wake(executor);
    auto bridge = std::make_shared<Bridge>();
    // Late SDK callbacks retain only SDK/client data, never the outer executor.
    // Abandonment can release the awaiting frame without a synchronous shutdown.
    bridge->client = impl->client;
    bridge->stop = impl->stop;
    bridge->wake = wake.signal();
    bridge->max_events = impl->event_count_limit;
    bridge->max_bytes = impl->event_byte_limit;
    struct Abandon {
        std::shared_ptr<Bridge> bridge;
        ~Abandon() { bridge->abandon(); }
    } abandon{bridge};
    sp::runtime::Callbacks callbacks;
    bool observes_events = bool(impl->on_event);
    for (const auto* hook = impl->hooks.get(); !observes_events && hook; hook = hook->next.get())
        observes_events = bool(hook->event);
    if (observes_events)
        callbacks.on_event = [bridge](const sp::Event& event) {
            bridge->retain(event);
            bridge->wake->notify();
        };
    callbacks.on_outcome = [bridge](sp::runtime::Result result) {
        {
            std::lock_guard lock(bridge->mutex);
            bridge->result = std::move(result);
        }
        bridge->wake->notify();
    };
    sp::runtime::Operation operation;
    std::optional<std::stop_callback<ForwardStop>> graph_stop;
    if (impl->cancel_token)
        graph_stop.emplace(impl->cancel_token->stop_token(), ForwardStop{impl->stop->source});
    try { operation = impl->client->start(std::move(impl->prepared), std::move(callbacks)); }
    catch (const sp::runtime::AdmissionError& error) { co_return error.outcome(); }
    std::exception_ptr callback_error;
    for (;;) {
        auto cancellation = co_await asio::this_coro::cancellation_state;
        const bool cancelled = cancellation.cancelled() != asio::cancellation_type::none;
        if (cancelled) operation.cancel();
        std::pmr::vector<std::unique_ptr<OwnedEvent>> events{&bridge->memory};
        sp::runtime::Result result;
        {
            std::lock_guard lock(bridge->mutex);
            wake.consume();
            events.swap(bridge->events);
            result = bridge->result;
        }
        {
            struct ReleaseBatch {
                std::shared_ptr<Bridge> bridge;
                std::pmr::vector<std::unique_ptr<OwnedEvent>>& events;
                ~ReleaseBatch() { bridge->release(events); }
            } release{bridge, events};
            if (!callback_error) {
                try {
                    for (auto& event : events) {
                        observe_hooks(impl->hooks, [&](DispatchHooks& hook) { if (hook.event) hook.event(event->event); });
                        if (impl->on_event) impl->on_event(event->event);
                        bridge->release(event);
                    }
                } catch (...) { callback_error = std::current_exception(); operation.cancel(); }
            }
        }
        if (result) {
            // Fence callbacks/slot release before releasing our runtime owner.
            operation.join();
            bool resource_failure;
            std::exception_ptr resource_exception;
            {
                std::lock_guard lock(bridge->mutex);
                resource_failure = bridge->resource_failure;
                resource_exception = bridge->resource_exception;
            }
            if (resource_failure)
                throw ProviderObserverError(result, resource_exception, sp::ErrorKind::ResourceLimit);
            if (callback_error) throw ProviderObserverError(result, callback_error);
            co_return result;
        }
        // A cancelled wait requests SDK stop; keep awaiting its authoritative
        // outcome without resetting cancellation or spinning on an aborted wait.
        const auto error = co_await wake.wait(cancelled ? asio::cancellation_slot{} : cancellation.slot());
        if (error && error != asio::error::operation_aborted) throw asio::system_error(error);
    }
}
ProviderRequest make_provider_request(
    const Provider& provider, std::string model, std::vector<sp::Message> messages,
    std::vector<ChatTool> tools, ProviderControls controls, ProviderMode mode) {
    auto reject = [](bool unsupported) {
        if (unsupported) throw std::invalid_argument("Requested provider controls are unsupported by this family");
    };
    auto parameters = [](const ChatTool& tool) {
        auto parsed = sp::json::parse(tool.parameters.dump());
        auto* document = std::get_if<sp::json::Document>(&parsed);
        if (!document || !document->root().is_object())
            throw std::invalid_argument("Tool parameters must be an admitted JSON object");
        return std::make_shared<const sp::json::Document>(std::move(*document));
    };
    const auto family = provider.family();
    const bool chat_controls = controls.chat_reasoning || controls.include_reasoning ||
        controls.usage_include || !controls.models.empty();
    const bool responses_controls = controls.previous_response_id ||
        !controls.previous_response_history.empty() || controls.parallel_tool_calls ||
        controls.verbosity || controls.truncation || controls.responses_include;
    const bool messages_controls = controls.thinking_mode || controls.output_effort ||
        controls.cache_control || controls.messages_tool_choice;
    const bool generate_controls = controls.gemini_history_mode || controls.gemini_thinking_level ||
        !controls.safety_settings.empty() || controls.gemini_tool_choice;
    reject((family != "openai.chat" && chat_controls) ||
           (family != "openai.responses" && responses_controls) ||
           (family != "anthropic.messages" && messages_controls) ||
           (family != "google.generate" && generate_controls));
    ProviderRequest result;
    result.mode = mode;
    if (family == "openai.chat") {
        reject(controls.reasoning_summary || controls.thinking_budget || controls.include_thoughts ||
               controls.thinking_level || controls.thinking_summaries || controls.required_tool ||
               !controls.account_scope.empty() || !controls.system.empty() || controls.store || controls.max_tool_calls);
        sp::chat::Request request;
        request.model = std::move(model);
        request.canonical_messages = std::move(messages);
        request.max_output_tokens = controls.max_output_tokens;
        request.temperature = controls.temperature;
        request.top_p = controls.top_p;
        request.reasoning_effort = std::move(controls.reasoning_effort);
        request.service_tier = std::move(controls.service_tier);
        request.provider = std::move(controls.provider);
        request.response_format = std::move(controls.response_format);
        request.reasoning = std::move(controls.chat_reasoning);
        request.include_reasoning = controls.include_reasoning;
        request.usage_include = controls.usage_include;
        request.models = std::move(controls.models);
        request.tools.reserve(tools.size());
        for (const auto& tool : tools) request.tools.push_back({tool.name, tool.description, parameters(tool)});
        result.payload = std::move(request);
    } else if (family == "anthropic.messages") {
        reject(controls.reasoning_effort || controls.reasoning_summary || controls.include_thoughts ||
               controls.thinking_level || controls.thinking_summaries || controls.service_tier || controls.required_tool ||
               controls.response_format || controls.store || controls.max_tool_calls);
        sp::messages::Request request;
        request.model = std::move(model);
        request.messages = std::move(messages);
        for (auto& message : request.messages) if (message.role == sp::Role::Tool) message.role = sp::Role::User;
        request.system = std::move(controls.system);
        request.account_scope = std::move(controls.account_scope);
        request.max_tokens = controls.max_output_tokens;
        request.temperature = controls.temperature;
        request.top_p = controls.top_p;
        request.thinking_budget = controls.thinking_budget;
        request.thinking_mode = controls.thinking_mode;
        request.output_effort = controls.output_effort;
        request.cache_control = std::move(controls.cache_control);
        request.tool_choice = std::move(controls.messages_tool_choice);
        request.provider = std::move(controls.provider);
        request.tools.reserve(tools.size());
        for (const auto& tool : tools) request.tools.push_back({tool.name, tool.description, parameters(tool), {}, {}});
        result.payload = std::move(request);
    } else if (family == "openai.responses") {
        reject(controls.thinking_budget || controls.include_thoughts ||
               controls.thinking_level || controls.thinking_summaries);
        sp::responses::Request request;
        request.model = std::move(model);
        request.messages = std::move(messages);
        request.instructions = std::move(controls.system);
        request.account_scope = std::move(controls.account_scope);
        request.max_output_tokens = controls.max_output_tokens;
        request.temperature = controls.temperature;
        request.top_p = controls.top_p;
        request.provider = std::move(controls.provider);
        request.response_format = std::move(controls.response_format);
        request.store = controls.store;
        request.max_tool_calls = controls.max_tool_calls;
        request.previous_response_id = std::move(controls.previous_response_id);
        request.previous_response_history = std::move(controls.previous_response_history);
        request.parallel_tool_calls = controls.parallel_tool_calls;
        request.verbosity = controls.verbosity;
        request.truncation = controls.truncation;
        request.include = std::move(controls.responses_include);
        if (controls.reasoning_effort || controls.reasoning_summary)
            request.reasoning = sp::responses::ReasoningOptions{std::move(controls.reasoning_effort), std::move(controls.reasoning_summary)};
        request.service_tier = std::move(controls.service_tier);
        request.required_tool = std::move(controls.required_tool);
        request.tools.reserve(tools.size());
        for (const auto& tool : tools) request.tools.push_back({tool.name, tool.description, parameters(tool), {}});
        result.payload = std::move(request);
    } else if (family == "google.generate") {
        reject(controls.top_p || controls.reasoning_effort || controls.reasoning_summary ||
               controls.thinking_level || controls.thinking_summaries || controls.service_tier ||
               controls.provider || controls.response_format || controls.store || controls.max_tool_calls);
        sp::gemini::Request request;
        request.model = std::move(model);
        request.messages = std::move(messages);
        request.system = std::move(controls.system);
        request.account_scope = std::move(controls.account_scope);
        request.max_output_tokens = controls.max_output_tokens;
        request.thinking_budget = controls.thinking_budget;
        request.include_thoughts = controls.include_thoughts;
        request.temperature = controls.temperature;
        request.thinking_level = controls.gemini_thinking_level;
        request.safety_settings = std::move(controls.safety_settings);
        request.tool_choice = std::move(controls.gemini_tool_choice);
        if (controls.gemini_history_mode) request.history_mode = *controls.gemini_history_mode;
        request.required_tool = std::move(controls.required_tool);
        request.tools.reserve(tools.size());
        for (const auto& tool : tools) request.tools.push_back({tool.name, tool.description, parameters(tool)});
        result.payload = std::move(request);
    } else if (family == "google.interactions") {
        reject(controls.temperature || controls.top_p || controls.reasoning_effort || controls.reasoning_summary ||
               controls.thinking_budget || controls.include_thoughts ||
               controls.provider || controls.response_format || controls.store || controls.max_tool_calls);
        sp::interactions::Request request;
        request.model = std::move(model);
        request.messages = std::move(messages);
        request.system = std::move(controls.system);
        request.account_scope = std::move(controls.account_scope);
        request.max_output_tokens = controls.max_output_tokens;
        request.thinking_level = std::move(controls.thinking_level);
        request.thinking_summaries = controls.thinking_summaries;
        request.service_tier = std::move(controls.service_tier);
        request.required_tool = std::move(controls.required_tool);
        request.tools.reserve(tools.size());
        for (const auto& tool : tools) request.tools.push_back({tool.name, tool.description, parameters(tool)});
        result.payload = std::move(request);
    } else throw std::invalid_argument("Unsupported provider family");
    auto history = std::visit([](auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, sp::chat::Request>) return std::move(payload.canonical_messages);
        else return std::move(payload.messages);
    }, result.payload);
    set_provider_request_messages(result, std::move(history));
    return result;
}
void set_provider_request_messages(ProviderRequest& request, std::vector<sp::Message> messages) {
    std::visit([&](auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, sp::chat::Request>) {
            payload.messages.clear();
            payload.canonical_messages = std::move(messages);
        } else if constexpr (std::is_same_v<T, sp::responses::Request>) {
            payload.messages = std::move(messages);
        } else {
            std::vector<sp::Message> conversation;
            conversation.reserve(messages.size());
            for (auto& message : messages) {
                if (message.role == sp::Role::System) {
                    if (message.native || message.wire_output)
                        throw std::invalid_argument("System instruction is not a portable text projection");
                    for (const auto& part : message.parts) {
                        const auto* text = std::get_if<sp::Text>(&part);
                        if (!text) throw std::invalid_argument("Nontext system instruction cannot be represented by this family");
                        if (!payload.system.empty()) payload.system += "\n\n";
                        payload.system += text->value;
                    }
                } else {
                    if constexpr (std::is_same_v<T, sp::messages::Request>)
                        if (message.role == sp::Role::Tool) message.role = sp::Role::User;
                    conversation.push_back(std::move(message));
                }
            }
            payload.messages = std::move(conversation);
        }
    }, request.payload);
}
void clear_provider_request_messages(ProviderRequest& request) {
    std::visit([](auto& payload) {
        payload.messages.clear();
        if constexpr (std::is_same_v<std::decay_t<decltype(payload)>, sp::chat::Request>)
            payload.canonical_messages.clear();
    }, request.payload);
}
std::string message_digest(const sp::Message& message) {
    return detail::sha256_identity("NeoGraph provider message identity v2", "full-message/v2",
        detail::canonical_json_bytes(message_projection_json(message)));
}
const std::vector<sp::Message>& provider_request_messages(const ProviderRequest& request) {
    return std::visit([](const auto& payload) -> const std::vector<sp::Message>& {
        if constexpr (std::is_same_v<std::decay_t<decltype(payload)>, sp::chat::Request>) {
            if (!payload.messages.empty())
                throw std::invalid_argument("Canonical history view requires full typed Chat messages");
            return payload.canonical_messages;
        } else return payload.messages;
    }, request.payload);
}
std::optional<std::uint64_t> Provider::conservative_token_upper_bound(const PreparedProviderRequest& request) {
    if (!request.valid()) return {};
    const auto* descriptor = request.impl_->prepared.descriptor();
    const auto* retry = request.impl_->prepared.retry_policy();
    const auto invocations = request.impl_->prepared.model_invocation_limit();
    if (!descriptor || !retry || !invocations) return {};
    const auto input = descriptor->policy()->input_limit(request.family(), request.model());
    const auto output = request.max_output_tokens() ? request.max_output_tokens()
        : descriptor->policy()->output_limit(request.family(), request.model());
    if (!input || !output || *output > std::numeric_limits<std::uint64_t>::max() - *input) return {};
    const auto per_invocation = *input + *output;
    const auto attempts = retry->enabled ? retry->max_attempts : 1;
    if (!attempts || *invocations > std::numeric_limits<std::uint64_t>::max() / attempts) return {};
    const auto possible = *invocations * attempts;
    if (!possible || per_invocation > std::numeric_limits<std::uint64_t>::max() / possible) return {};
    return per_invocation * possible;
}
ProviderBudgetClaim::ProviderBudgetClaim() = default;
ProviderBudgetClaim::ProviderBudgetClaim(ProviderDispatchBudget budget, std::uint64_t amount)
    : budget_(std::move(budget)), amount_(amount) {}
ProviderBudgetClaim::~ProviderBudgetClaim() { release_undispatched(); }
ProviderBudgetClaim::ProviderBudgetClaim(ProviderBudgetClaim&& other) noexcept
    : budget_(std::move(other.budget_)), amount_(std::exchange(other.amount_, 0)),
      managed_effect_(std::move(other.managed_effect_)),
      dispatched_(other.dispatched_), settled_(other.settled_),
      writeahead_committed_(other.writeahead_committed_) {}
ProviderBudgetClaim& ProviderBudgetClaim::operator=(ProviderBudgetClaim&& other) noexcept {
    if (this != &other) {
        release_undispatched();
        budget_ = std::move(other.budget_);
        amount_ = std::exchange(other.amount_, 0);
        managed_effect_ = std::move(other.managed_effect_);
        dispatched_ = other.dispatched_;
        settled_ = other.settled_;
        writeahead_committed_ = other.writeahead_committed_;
    }
    return *this;
}
bool ProviderBudgetClaim::active() const noexcept { return budget_.usage && amount_ != 0; }
std::uint64_t ProviderBudgetClaim::amount() const noexcept { return amount_; }
asio::awaitable<void> ProviderBudgetClaim::begin_managed_effect(
    const PreparedProviderRequest& request, std::string effect_id) {
    if (!budget_.managed_budget_lease) co_return;
    if (!active() || dispatched_ || settled_ || writeahead_committed_)
        throw std::logic_error("Managed provider effect requires an undispatched live claim");
    if (!budget_.managed_budget_store)
        throw std::logic_error("Managed provider effect requires its original durable store");
    if (effect_id.empty()) effect_id = budget_.managed_effect_id;
    if (effect_id.empty())
        throw std::invalid_argument("Managed provider effect requires a host call identity");
    auto effect = co_await budget_.managed_budget_store->begin_managed_budget_effect_async(
        budget_.managed_budget_lease, std::move(effect_id), amount_, Provider::request_digest(request));
    // From here even a host allocation/cancellation failure must retain the
    // already durable pending window; absence of SDK evidence is not a refund.
    writeahead_committed_ = true;
    managed_effect_ = std::make_unique<graph::ManagedBudgetEffectReceipt>(std::move(effect));
}
asio::awaitable<void> ProviderBudgetClaim::settle_managed(
    sp::runtime::Result result, std::exception_ptr dispatch_error) {
    if (!active()) co_return;
    if (!result) throw std::runtime_error("Provider dispatch returned no owned outcome");
    try {
        if (budget_.managed_budget_lease) {
            if (!managed_effect_ || !writeahead_committed_ || !dispatched_ || settled_)
                throw std::logic_error("Managed provider settlement requires its pending dispatched claim");
            if (!budget_.usage->remember_provider_effect(managed_effect_->effect_id()))
                throw std::logic_error("Managed provider effect was already accounted");
        }
        (void)settle_accounting(result);
        if (managed_effect_) {
            co_await budget_.managed_budget_store->settle_managed_budget_effect_async(
                budget_.managed_budget_lease, *managed_effect_, result,
                budget_.usage->authority_snapshot());
        }
    } catch (...) {
        throw ProviderBudgetSettlementError(std::move(result), std::current_exception(),
                                            std::move(dispatch_error));
    }
}
void ProviderBudgetClaim::mark_dispatched() {
    if (budget_.managed_budget_lease && (!writeahead_committed_ || !managed_effect_))
        throw std::logic_error("Managed provider dispatch requires its durable pending effect");
    dispatched_ = true;
}
void ProviderBudgetClaim::release_undispatched() noexcept {
    if (active() && !dispatched_ && !settled_ && !writeahead_committed_)
        budget_.usage->release_reservation(amount_);
    amount_ = 0;
}
std::uint64_t ProviderBudgetClaim::settle(sp::runtime::Result result) {
    if (budget_.managed_budget_lease)
        throw std::logic_error("Managed provider settlement requires the durable coroutine barrier");
    return settle_accounting(std::move(result));
}
std::uint64_t ProviderBudgetClaim::settle_accounting(sp::runtime::Result result) {
    if (!active()) return 0;
    if (settled_) throw std::logic_error("Provider budget claim is already settled");
    if (!dispatched_) throw std::logic_error("Provider budget claim has not crossed dispatch authority");
    if (!result) throw std::invalid_argument("Provider budget settlement requires an owned outcome");
    std::uint64_t committed = amount_;
    if (const auto* completion = std::get_if<sp::Completion>(result.get())) {
        if (completion->attempt.prior_usage_unknown || completion->attempt.transport_internal_resends != 0)
            budget_.usage->observe(completion->usage);
        else {
            const auto charge = UsageAccumulator::conservative_final_charge(completion->usage);
            budget_.usage->settle_reservation(amount_, completion->usage);
            if (charge) committed = *charge;
        }
    } else {
        const auto& failed = std::get<sp::Failure>(*result);
        budget_.usage->observe(failed.partial.usage);
        if (provider_failure_proves_not_sent(failed)) {
            budget_.usage->release_reservation(amount_);
            committed = 0;
        }
    }
    settled_ = true;
    if (budget_.usage->total_tokens_wide() > budget_.model_token_budget) {
        if (budget_.budget_exhausted) budget_.budget_exhausted->store(true, std::memory_order_release);
        if (budget_.budget_cancel_token) budget_.budget_cancel_token->cancel();
    }
    return committed;
}
ProviderBudgetClaim reserve_provider_dispatch(const PreparedProviderRequest& request, ProviderDispatchBudget budget) {
    if (!request.valid()) {
        if (const auto* error = request.error()) throw ProviderFailure(failure(*error));
        throw ProviderFailure(failure(local_error(sp::ErrorKind::Misuse, "Invalid provider preparation")));
    }
    if (budget.managed_budget_lease) {
        const auto& scope = budget.managed_budget_lease->scope();
        if (!budget.managed_budget_store || budget.model_token_budget == 0 ||
            scope.original_ceiling == 0 || budget.model_token_budget > scope.original_ceiling)
            throw ProviderFailure(failure(local_error(sp::ErrorKind::InvalidConfig,
                "Standalone provider currency requires its original bounded durable lease")));
    }
    if (budget.model_token_budget == 0) return {};
    if (!budget.usage)
        throw ProviderFailure(failure(local_error(sp::ErrorKind::InvalidConfig, "Bounded provider dispatch requires a usage authority")));
    const auto amount = Provider::conservative_token_upper_bound(request);
    if (!amount || *amount == 0)
        throw ProviderFailure(failure(local_error(sp::ErrorKind::LimitUnknown,
            "Bounded provider dispatch requires admitted model input and output ceilings")));
    if ((budget.budget_exhausted && budget.budget_exhausted->load(std::memory_order_acquire)) ||
        !budget.usage->try_reserve(*amount, budget.model_token_budget)) {
        if (budget.budget_exhausted) budget.budget_exhausted->store(true, std::memory_order_release);
        if (budget.budget_cancel_token) budget.budget_cancel_token->cancel();
        throw ProviderFailure(failure(local_error(sp::ErrorKind::QuotaExhausted, "Provider token budget exhausted")));
    }
    return ProviderBudgetClaim(std::move(budget), *amount);
}

std::string graph::managed_provider_effect_id(const ProviderCallIdentity& identity) {
    if (!identity.managed_budget_lease) return {};
    const auto& lease = *identity.managed_budget_lease;
    const auto& scope = lease.scope();
    if (identity.thread_id != lease.execution_thread_id() || identity.owner_scope != scope.owner_scope ||
        identity.task_id.empty() || identity.node_name.empty())
        throw std::invalid_argument("Managed provider effect does not match its host task scope");
    const json identity_bytes{
        {"bank_generation", lease.bank_generation()},
        {"owner_scope", scope.owner_scope}, {"thread_id", lease.execution_thread_id()},
        {"graph_identity", scope.graph_identity}, {"task_id", identity.task_id},
        {"node_name", identity.node_name}, {"call_ordinal", identity.call_ordinal}};
    return ::neograph::detail::sha256_identity("NeoGraph managed provider effect v1", "host-lease-call/v1",
        ::neograph::detail::canonical_json_bytes(identity_bytes));
}
} // namespace neograph
