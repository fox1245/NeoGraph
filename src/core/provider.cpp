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
#include <algorithm>
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

    // The byte budget must exist before the pmr containers are constructed: MSVC's
    // unordered_map allocates its sentinel and buckets in its constructor, libstdc++ lazily.
    // A zero budget would reject that first allocation as bad_alloc.
    Bridge(std::size_t event_limit, std::size_t byte_limit) : max_events(event_limit), max_bytes(byte_limit) {}

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
bool PreparedProviderRequest::is_endpoint_read() const noexcept {
    return impl_ && !impl_->prepared.endpoint().empty() && impl_->prepared.method() == "GET";
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
    else if (request.endpoint && request.mode != ProviderMode::Collect)
        impl->error = local_error(sp::ErrorKind::InvalidRequest, "Endpoint calls are buffered");
    else {
        try {
            impl->stop = std::make_shared<StopChannel>();
            if (request.options.stop_token.stop_possible())
                impl->stop->external.emplace(request.options.stop_token, ForwardStop{impl->stop->source});
            request.options.stop_token = impl->stop->source.get_token();
            impl->prepared = request.endpoint
                ? request.endpoint->prepare(*impl->client, std::move(request.options))
                : impl->client->prepare(std::move(request.payload), std::move(request.options));
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
    // A local dispatch answers chat requests; an endpoint call would get a chat outcome.
    if (request.endpoint)
        return reject_preparation(local_error(sp::ErrorKind::Unsupported, "A local provider has no SchemaProvider endpoints"));
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
    json identity{
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
            {"encoded_body", request.encoded_body()}};
    // An endpoint call is also named by what it sends: several operations share an endpoint, and a read or a
    // file upload has no body to tell them apart. A chat call's identity is unchanged.
    if (const auto endpoint = request.impl_->prepared.endpoint(); !endpoint.empty())
        identity["endpoint"] = {{"name", endpoint}, {"method", request.impl_->prepared.method()},
                                {"url", request.impl_->prepared.url()}};
    return detail::sha256_identity("NeoGraph Provider dispatch identity v1", "provider-request/v3",
        detail::canonical_json_bytes(identity));
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
    auto bridge = std::make_shared<Bridge>(impl->event_count_limit, impl->event_byte_limit);
    // Late SDK callbacks retain only SDK/client data, never the outer executor.
    // Abandonment can release the awaiting frame without a synchronous shutdown.
    bridge->client = impl->client;
    bridge->stop = impl->stop;
    bridge->wake = wake.signal();
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
std::shared_ptr<sp::runtime::Client> Provider::require_endpoint_runtime() const {
    auto client = endpoint_runtime();
    if (!client) throw std::logic_error("Provider has no SchemaProvider endpoints");
    return client;
}
asio::awaitable<void> detail::await_endpoint(EndpointStart start) {
    ProviderWakeReader wake(co_await asio::this_coro::executor);
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto cancel = start([done, signal = wake.signal()] {
        done->store(true, std::memory_order_release);
        signal->notify();
    });
    // As for a chat call, a cancelled wait cancels the call and keeps awaiting
    // its authoritative outcome instead of spinning on an aborted wait.
    const bool throw_on_cancel = co_await asio::this_coro::throw_if_cancelled();
    co_await asio::this_coro::throw_if_cancelled(false);
    bool cancelled = false;
    for (;;) {
        auto cancellation = co_await asio::this_coro::cancellation_state;
        if (!cancelled && cancellation.cancelled() != asio::cancellation_type::none) {
            cancelled = true;
            cancel();
        }
        wake.consume();
        if (done->load(std::memory_order_acquire)) break;
        const auto error = co_await wake.wait(cancelled ? asio::cancellation_slot{} : cancellation.slot());
        if (error && error != asio::error::operation_aborted) throw asio::system_error(error);
    }
    co_await asio::this_coro::throw_if_cancelled(throw_on_cancel);
}
// Normalize only portable client results; native replay and mixed messages are
// indivisible, and a system message remains a boundary even when lifted out.
static void group_tool_results(std::vector<sp::Message>& messages) {
    const auto eligible = [](const sp::Message& message) {
        if ((message.role != sp::Role::Tool && message.role != sp::Role::User) ||
            message.native || message.wire_output || message.parts.empty())
            return false;
        for (const auto& part : message.parts)
            if (!std::holds_alternative<sp::ToolResult>(part)) return false;
        return true;
    };
    std::size_t write = 0;
    for (std::size_t read = 0; read < messages.size();) {
        auto end = read + 1;
        auto parts = messages[read].parts.size();
        if (eligible(messages[read])) {
            while (end < messages.size() && eligible(messages[end])) {
                parts += messages[end].parts.size();
                ++end;
            }
        }
        if (end > read + 1) {
            auto& destination = messages[read].parts;
            destination.reserve(parts);
            for (auto next = read + 1; next < end; ++next)
                for (auto& part : messages[next].parts)
                    destination.push_back(std::move(part));
        }
        if (write != read) messages[write] = std::move(messages[read]);
        ++write;
        read = end;
    }
    messages.resize(write);
}

// A Responses function_call_output has no error field, and the SDK refuses a
// result marked as an error. NeoGraph writes a failed call's error, status and
// retryability into the result's own text (tool_failure.h), so that text is what
// the request carries (#544). Native replay is indivisible.
static void send_tool_errors_as_text(std::vector<sp::Message>& messages) {
    for (auto& message : messages) {
        if (message.native || message.wire_output) continue;
        for (auto& part : message.parts)
            if (auto* result = std::get_if<sp::ToolResult>(&part)) result->is_error = false;
    }
}

namespace {
// A deprecated flat control moves into its family nest; setting both is ambiguous.
template <class T> void adopt(std::optional<T>& nest, std::optional<T>& alias, const char* name) {
    if (!alias) return;
    if (nest) throw std::invalid_argument(std::string("Set ProviderControls::") + name + " or its family nest, not both");
    nest = std::move(alias);
    alias.reset();
}
template <class T> void adopt(std::vector<T>& nest, std::vector<T>& alias, const char* name) {
    if (alias.empty()) return;
    if (!nest.empty()) throw std::invalid_argument(std::string("Set ProviderControls::") + name + " or its family nest, not both");
    nest = std::move(alias);
    alias.clear();
}
template <class T> bool present(const std::optional<T>& value) { return value.has_value(); }
template <class T> bool present(const std::vector<T>& value) { return !value.empty(); }
// A deprecated flat control belongs to one family. It moves into that family's
// nest, and any other family refuses it, as it did before the nests.
void adopt_deprecated(ProviderControls& c, std::string_view family) {
    const bool chat = present(c.chat_reasoning) || present(c.include_reasoning) || present(c.usage_include) ||
                      present(c.models);
    const bool responses = present(c.previous_response_id) || present(c.previous_response_history) ||
                           present(c.parallel_tool_calls) || present(c.verbosity) || present(c.truncation) ||
                           present(c.responses_include);
    const bool messages = present(c.thinking_mode) || present(c.output_effort) || present(c.cache_control) ||
                          present(c.messages_tool_choice);
    const bool generate = present(c.gemini_history_mode) || present(c.gemini_thinking_level) ||
                          present(c.safety_settings) || present(c.gemini_tool_choice);
    if ((chat && family != "openai.chat") || (responses && family != "openai.responses") ||
        (messages && family != "anthropic.messages") || (generate && family != "google.generate"))
        throw std::invalid_argument("Requested provider controls are unsupported by this family");
    adopt(c.chat.reasoning, c.chat_reasoning, "chat_reasoning");
    adopt(c.chat.include_reasoning, c.include_reasoning, "include_reasoning");
    adopt(c.chat.usage_include, c.usage_include, "usage_include");
    adopt(c.chat.models, c.models, "models");
    adopt(c.responses.previous_response_id, c.previous_response_id, "previous_response_id");
    adopt(c.responses.previous_response_history, c.previous_response_history, "previous_response_history");
    adopt(c.responses.parallel_tool_calls, c.parallel_tool_calls, "parallel_tool_calls");
    adopt(c.responses.verbosity, c.verbosity, "verbosity");
    adopt(c.responses.truncation, c.truncation, "truncation");
    adopt(c.responses.include, c.responses_include, "responses_include");
    adopt(c.messages.thinking_mode, c.thinking_mode, "thinking_mode");
    adopt(c.messages.output_effort, c.output_effort, "output_effort");
    adopt(c.messages.cache_control, c.cache_control, "cache_control");
    adopt(c.messages.tool_choice, c.messages_tool_choice, "messages_tool_choice");
    adopt(c.generate.history_mode, c.gemini_history_mode, "gemini_history_mode");
    adopt(c.generate.thinking_level, c.gemini_thinking_level, "gemini_thinking_level");
    adopt(c.generate.safety_settings, c.safety_settings, "safety_settings");
    adopt(c.generate.tool_choice, c.gemini_tool_choice, "gemini_tool_choice");
}
std::shared_ptr<const sp::json::Document> extensions_document(const std::optional<json>& value) {
    if (!value) return {};
    auto parsed = sp::json::parse(value->dump());
    auto* document = std::get_if<sp::json::Document>(&parsed);
    if (!document || !document->root().is_object())
        throw std::invalid_argument("Provider extensions must be a JSON object");
    return std::make_shared<const sp::json::Document>(std::move(*document));
}
} // namespace

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
    if (family != "openai.responses" &&
        std::any_of(tools.begin(), tools.end(), [](const ChatTool& tool) { return tool.strict.value_or(false); }))
        throw std::invalid_argument("Strict tool definitions are supported only by the openai.responses family");
    // Each family takes its own nest; the nests of other families are ignored.
    adopt_deprecated(controls, family);
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
        auto& chat = controls.chat;
        request.reasoning = std::move(chat.reasoning);
        request.include_reasoning = chat.include_reasoning;
        request.usage_include = chat.usage_include;
        request.models = std::move(chat.models);
        request.modalities = std::move(chat.modalities);
        request.audio = std::move(chat.audio);
        request.image_config = std::move(chat.image_config);
        request.plugins = std::move(chat.plugins);
        request.extensions = extensions_document(chat.extensions);
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
        request.system = std::move(controls.system);
        request.account_scope = std::move(controls.account_scope);
        request.max_tokens = controls.max_output_tokens;
        request.temperature = controls.temperature;
        request.top_p = controls.top_p;
        request.thinking_budget = controls.thinking_budget;
        auto& family_controls = controls.messages;
        request.thinking_mode = family_controls.thinking_mode;
        request.output_effort = family_controls.output_effort;
        request.cache_control = std::move(family_controls.cache_control);
        request.tool_choice = std::move(family_controls.tool_choice);
        request.provider = std::move(controls.provider);
        request.container = std::move(family_controls.container);
        request.skills = std::move(family_controls.skills);
        request.betas = std::move(family_controls.betas);
        request.mcp_servers = std::move(family_controls.mcp_servers);
        request.extensions = extensions_document(family_controls.extensions);
        request.tools.reserve(tools.size() + family_controls.tools.size());
        for (const auto& tool : tools) {
            sp::messages::ToolDefinition definition;
            definition.name = tool.name;
            definition.description = tool.description;
            definition.input_schema = parameters(tool);
            request.tools.push_back(std::move(definition));
        }
        for (auto& tool : family_controls.tools) request.tools.push_back(std::move(tool));
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
        auto& responses = controls.responses;
        request.previous_response_id = std::move(responses.previous_response_id);
        request.previous_response_history = std::move(responses.previous_response_history);
        request.parallel_tool_calls = responses.parallel_tool_calls;
        request.verbosity = responses.verbosity;
        request.truncation = responses.truncation;
        request.include = std::move(responses.include);
        request.include_names = std::move(responses.include_names);
        request.hosted_tools = std::move(responses.hosted_tools);
        request.modalities = std::move(responses.modalities);
        request.image_config = std::move(responses.image_config);
        request.plugins = std::move(responses.plugins);
        request.mcp_approvals = std::move(responses.mcp_approvals);
        request.extensions = extensions_document(responses.extensions);
        if (controls.reasoning_effort || controls.reasoning_summary)
            request.reasoning = sp::responses::ReasoningOptions{std::move(controls.reasoning_effort), std::move(controls.reasoning_summary)};
        request.service_tier = std::move(controls.service_tier);
        request.required_tool = std::move(controls.required_tool);
        request.tools.reserve(tools.size());
        for (const auto& tool : tools) request.tools.push_back({tool.name, tool.description, parameters(tool), tool.strict, {}});
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
        auto& generate = controls.generate;
        request.thinking_level = generate.thinking_level;
        request.safety_settings = std::move(generate.safety_settings);
        request.tool_choice = std::move(generate.tool_choice);
        if (generate.history_mode) request.history_mode = *generate.history_mode;
        request.response_modalities = std::move(generate.response_modalities);
        request.speech_config = std::move(generate.speech_config);
        request.image_config = std::move(generate.image_config);
        request.audio_transcription_config = std::move(generate.audio_transcription_config);
        request.audio_format = std::move(generate.audio_format);
        request.builtin_tools = std::move(generate.builtin_tools);
        request.mcp_servers = std::move(generate.mcp_servers);
        request.server_tool_invocations = generate.server_tool_invocations;
        request.retrieval_config = std::move(generate.retrieval_config);
        request.extensions = extensions_document(generate.extensions);
        request.required_tool = std::move(controls.required_tool);
        request.tools.reserve(tools.size());
        for (const auto& tool : tools) request.tools.push_back({tool.name, tool.description, parameters(tool)});
        result.payload = std::move(request);
    } else if (family == "google.interactions") {
        reject(controls.temperature || controls.top_p || controls.reasoning_effort || controls.reasoning_summary ||
               controls.thinking_budget || controls.include_thoughts ||
               controls.provider || controls.response_format || controls.max_tool_calls);
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
        request.store = controls.store;
        auto& interactions = controls.interactions;
        request.response_format = std::move(interactions.response_format);
        request.speech_config = std::move(interactions.speech_config);
        request.transcription_config = std::move(interactions.transcription_config);
        request.builtin_tools = std::move(interactions.builtin_tools);
        request.previous_interaction_id = std::move(interactions.previous_interaction_id);
        request.video_task = std::move(interactions.video_task);
        request.agent = std::move(interactions.agent);
        request.background = interactions.background;
        request.extensions = extensions_document(interactions.extensions);
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
            send_tool_errors_as_text(messages);
            payload.messages = std::move(messages);
        } else {
            if constexpr (std::is_same_v<T, sp::messages::Request> ||
                          std::is_same_v<T, sp::gemini::Request>)
                group_tool_results(messages);
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
std::optional<std::uint64_t> Provider::invocation_upper_bound(const PreparedProviderRequest& request) {
    if (!request.valid()) return {};
    const auto* retry = request.impl_->prepared.retry_policy();
    const auto invocations = request.impl_->prepared.model_invocation_limit();
    if (!retry || !invocations) return {};
    const auto attempts = retry->enabled ? retry->max_attempts : 1;
    if (!attempts || *invocations > std::numeric_limits<std::uint64_t>::max() / attempts) return {};
    const auto possible = *invocations * attempts;
    if (!possible) return {};
    return possible;
}
std::optional<std::uint64_t> Provider::conservative_token_upper_bound(const PreparedProviderRequest& request) {
    if (!request.valid()) return {};
    const auto* descriptor = request.impl_->prepared.descriptor();
    const auto possible = invocation_upper_bound(request);
    if (!descriptor || !possible) return {};
    const auto input = descriptor->policy()->input_limit(request.family(), descriptor->base_url(), request.model());
    const auto output = request.max_output_tokens() ? request.max_output_tokens()
        : descriptor->policy()->output_limit(request.family(), descriptor->base_url(), request.model());
    if (!input || !output || *output > std::numeric_limits<std::uint64_t>::max() - *input) return {};
    const auto per_invocation = *input + *output;
    if (per_invocation > std::numeric_limits<std::uint64_t>::max() / *possible) return {};
    return per_invocation * *possible;
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
        if (provider_failure_proves_not_accepted(failed)) {
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
