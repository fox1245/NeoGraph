#pragma once

#include <neograph/api.h>
#include <neograph/types.h>
#include <runtime/client.h>
#include <core/request_controls.h>
#include <asio/awaitable.hpp>
#include <cstdint>
#include <atomic>
#include <functional>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <utility>

namespace neograph::graph {
class CancelToken;
class CheckpointStore;
class OwnedManagedBudgetLease;
class ManagedBudgetEffectReceipt;
}

namespace neograph {

struct ProviderDispatchBudget {
    std::shared_ptr<UsageAccumulator> usage;
    std::uint64_t model_token_budget = 0;
    std::shared_ptr<std::atomic<bool>> budget_exhausted;
    std::shared_ptr<graph::CancelToken> budget_cancel_token;
    std::shared_ptr<graph::OwnedManagedBudgetLease> managed_budget_lease;
    std::shared_ptr<graph::CheckpointStore> managed_budget_store;
    std::string managed_effect_id;
};

enum class ProviderMode : std::uint8_t { Collect, Stream };

/// Controls of one provider family, in that family's own SDK types. `make_provider_request` applies the nest
/// of the provider's family and ignores the others, so one `ProviderControls` can serve every node of a graph
/// whose nodes call different families. Unset members are not sent; the SDK checks every set one at prepare.
/// `extensions` are top-level request members the SDK policy admits for the origin, sent as given.
struct ChatControls {
    std::optional<sp::chat::ReasoningOptions> reasoning;
    std::optional<bool> include_reasoning, usage_include;
    std::vector<std::string> models;
    std::vector<sp::chat::OutputModality> modalities;
    std::optional<sp::chat::AudioOutput> audio;
    std::optional<sp::OpenRouterImageConfig> image_config;
    std::vector<sp::OpenRouterPlugin> plugins;
    std::optional<json> extensions;
};
struct ResponsesControls {
    /// Server-side cursor; send only new input with this identifier.
    std::optional<std::string> previous_response_id;
    /// Prior cursor turns retained as local validation evidence, not resent input.
    std::vector<sp::Message> previous_response_history;
    std::optional<bool> parallel_tool_calls;
    std::optional<sp::responses::Verbosity> verbosity;
    std::optional<sp::responses::Truncation> truncation;
    std::optional<std::vector<sp::responses::Include>> include;
    std::vector<std::string> include_names;
    /// Tools the service runs (web search, image generation, file search, code interpreter, MCP, ...).
    std::vector<sp::responses::HostedTool> hosted_tools;
    std::vector<sp::responses::OutputModality> modalities;
    std::optional<sp::OpenRouterImageConfig> image_config;
    std::vector<sp::OpenRouterPlugin> plugins;
    std::vector<sp::responses::McpApproval> mcp_approvals;
    std::optional<json> extensions;
};
struct MessagesControls {
    std::optional<sp::messages::ThinkingMode> thinking_mode;
    std::optional<sp::messages::OutputEffort> output_effort;
    std::optional<sp::messages::CacheControl> cache_control;
    std::optional<sp::messages::ToolChoice> tool_choice;
    /// Tools the family defines (server tools such as web search, typed client tools such as memory), sent
    /// after the portable function tools.
    std::vector<sp::messages::ToolDefinition> tools;
    std::optional<std::string> container;
    std::vector<sp::messages::ContainerSkill> skills;
    std::vector<std::string> betas;
    std::vector<sp::messages::McpServer> mcp_servers;
    std::optional<json> extensions;
};
struct GenerateControls {
    /// Explicit admission mode for authentic or portable foreign Gemini history.
    std::optional<sp::gemini::HistoryMode> history_mode;
    std::optional<sp::gemini::ThinkingLevel> thinking_level;
    std::vector<sp::gemini::SafetySetting> safety_settings;
    std::optional<sp::gemini::ToolChoice> tool_choice;
    std::vector<sp::gemini::ResponseModality> response_modalities;
    std::optional<sp::gemini::SpeechConfig> speech_config;
    std::optional<sp::gemini::ImageConfig> image_config;
    std::optional<sp::gemini::AudioTranscriptionConfig> audio_transcription_config;
    std::optional<sp::gemini::AudioFormat> audio_format;
    std::vector<sp::gemini::BuiltinTool> builtin_tools;
    std::vector<sp::gemini::McpServer> mcp_servers;
    std::optional<bool> server_tool_invocations;
    std::optional<sp::gemini::RetrievalConfig> retrieval_config;
    std::optional<json> extensions;
};
struct InteractionsControls {
    std::vector<sp::interactions::ResponseFormat> response_format;
    std::optional<sp::interactions::SpeechConfiguration> speech_config;
    std::optional<sp::interactions::TranscriptionConfig> transcription_config;
    std::vector<sp::interactions::BuiltinTool> builtin_tools;
    /// Continues a stored interaction; `background` runs it as a job (it needs the portable `store`).
    std::optional<std::string> previous_interaction_id, video_task, agent;
    std::optional<bool> background;
    std::optional<json> extensions;
};

/// Typed per-call controls translated into the selected SDK provider family.
/// The flat members are portable; a family's own controls are in its nest. A portable control the family
/// cannot express throws in `make_provider_request`; unsupported family/model combinations fail at prepare.
struct ProviderControls {
    std::optional<std::uint64_t> max_output_tokens;
    std::optional<std::uint64_t> max_tool_calls;
    std::optional<double> temperature, top_p;
    std::optional<std::string> reasoning_effort, reasoning_summary;
    std::optional<std::uint64_t> thinking_budget;
    std::optional<bool> include_thoughts;
    std::optional<std::string> thinking_level;
    std::optional<bool> thinking_summaries;
    std::optional<std::string> service_tier, required_tool;
    std::optional<sp::OpenRouterRouting> provider;
    std::optional<sp::ResponseFormat> response_format;
    std::optional<bool> store;
    std::string account_scope, system;

    ChatControls chat;
    ResponsesControls responses;
    MessagesControls messages;
    GenerateControls generate;
    InteractionsControls interactions;

    /// Deprecated in 0.16.0 and removed in 0.17.0: use the family nests (`chat.reasoning`,
    /// `responses.include`, `messages.tool_choice`, `generate.thinking_level`, ...). Setting one of these and
    /// its nest member too throws.
    std::optional<sp::chat::ReasoningOptions> chat_reasoning;
    std::optional<bool> include_reasoning, usage_include;
    std::vector<std::string> models;
    std::optional<std::string> previous_response_id;
    std::vector<sp::Message> previous_response_history;
    std::optional<bool> parallel_tool_calls;
    std::optional<sp::responses::Verbosity> verbosity;
    std::optional<sp::responses::Truncation> truncation;
    std::optional<std::vector<sp::responses::Include>> responses_include;
    std::optional<sp::messages::ThinkingMode> thinking_mode;
    std::optional<sp::messages::OutputEffort> output_effort;
    std::optional<sp::messages::CacheControl> cache_control;
    std::optional<sp::messages::ToolChoice> messages_tool_choice;
    std::optional<sp::gemini::HistoryMode> gemini_history_mode;
    std::optional<sp::gemini::ThinkingLevel> gemini_thinking_level;
    std::vector<sp::gemini::SafetySetting> safety_settings;
    std::optional<sp::gemini::ToolChoice> gemini_tool_choice;
};

/// Host-only delivery caps; explicit values may tighten admitted SDK resources.
struct ProviderObserverLimits {
    std::optional<std::size_t> max_events;
    std::optional<std::size_t> max_bytes;
};

namespace detail {
/// What an endpoint payload prepared. `error` stops the call before the SDK is asked (an upload whose file cannot
/// be read). `upload` names what an upload sends that its encoded body cannot (#541): the SDK reads a file slice
/// while it sends it, and puts a random boundary in the body of an upload from memory.
struct EndpointPreparation {
    sp::runtime::PreparedRequest prepared;
    std::optional<sp::Error> error;
    std::optional<json> upload;
};
/// An endpoint request a ProviderRequest carries instead of a chat payload
/// (#531): it prepares itself on the provider's SDK client.
class EndpointPayload {
public:
    virtual ~EndpointPayload() = default;
    virtual EndpointPreparation prepare(sp::runtime::Client& client, sp::runtime::RunOptions options) const = 0;
};
} // namespace detail

/// Portable request controls remain the SDK's declared, family-specific fields.
/// No raw JSON field override or callback-selected streaming mode is admitted.
struct ProviderRequest {
    sp::runtime::Request payload;
    /// An endpoint call instead of a chat call (#531, make_endpoint_request).
    /// `payload` is then unused, and the call is buffered.
    std::shared_ptr<const detail::EndpointPayload> endpoint;
    ProviderMode mode = ProviderMode::Collect;
    sp::runtime::RunOptions options;
    std::shared_ptr<graph::CancelToken> cancel_token;
    std::function<void(const sp::Event&)> on_event;
    ProviderObserverLimits observer_limits;
};

/// Post-dispatch host failure retaining the exact drained provider outcome.
/// The owned outcome is evidence, not success or permission to redispatch.
class NEOGRAPH_API ProviderOutcomeError : public std::runtime_error {
public:
    ProviderOutcomeError(const char* message, sp::runtime::Result outcome,
                         std::exception_ptr cause)
        : std::runtime_error(message), outcome_(std::move(outcome)), cause_(std::move(cause)) {}
    const sp::runtime::Result& outcome() const noexcept { return outcome_; }
    const std::exception_ptr& cause() const noexcept { return cause_; }
private:
    sp::runtime::Result outcome_;
    std::exception_ptr cause_;
};

class NEOGRAPH_API ProviderBudgetSettlementError final : public ProviderOutcomeError {
public:
    ProviderBudgetSettlementError(sp::runtime::Result outcome, std::exception_ptr cause,
                                  std::exception_ptr dispatch_error = {})
        : ProviderOutcomeError("Provider budget settlement could not be persisted",
                               std::move(outcome), std::move(cause)),
          dispatch_error_(std::move(dispatch_error)) {}
    const std::exception_ptr& dispatch_error() const noexcept { return dispatch_error_; }
private:
    std::exception_ptr dispatch_error_;
};

class NEOGRAPH_API ProviderObserverError final : public ProviderOutcomeError {
public:
    ProviderObserverError(sp::runtime::Result result, std::exception_ptr cause,
                          sp::ErrorKind kind = sp::ErrorKind::Misuse)
        : ProviderOutcomeError(kind == sp::ErrorKind::ResourceLimit
              ? "Provider event retention resource limit exhausted" : "Provider event observer failed",
              std::move(result), std::move(cause)), kind_(kind) {}
    sp::ErrorKind error_kind() const noexcept { return kind_; }
private:
    sp::ErrorKind kind_;
};

class Provider;
class NEOGRAPH_API PreparedProviderRequest final {
public:
    PreparedProviderRequest();
    ~PreparedProviderRequest();
    PreparedProviderRequest(PreparedProviderRequest&&) noexcept;
    PreparedProviderRequest& operator=(PreparedProviderRequest&&) noexcept;
    PreparedProviderRequest(const PreparedProviderRequest&) = delete;
    PreparedProviderRequest& operator=(const PreparedProviderRequest&) = delete;
    bool valid() const noexcept;
    const sp::Error* error() const noexcept;
    std::string_view family() const noexcept;
    std::string_view model() const noexcept;
    std::string_view encoded_body() const noexcept;
    sp::runtime::SteadyTime deadline() const noexcept;
    ProviderMode mode() const noexcept;
    bool is_cancelled() const noexcept;
    std::optional<std::uint64_t> max_output_tokens() const noexcept;
    bool requires_native_custody() const noexcept;
    const sp::descriptor::ValidatedDescriptor* admitted_descriptor() const noexcept;
    /// An endpoint read (#531): a GET, such as a job poll or download or a file
    /// or batch lookup. The provider never charges one, and it can be sent again,
    /// so a journaling broker sends it each time instead of replaying it.
    bool is_endpoint_read() const noexcept;
private:
    struct Impl;
    explicit PreparedProviderRequest(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
    friend class Provider;
};

class NEOGRAPH_API ProviderBudgetClaim;
NEOGRAPH_API ProviderBudgetClaim reserve_provider_dispatch(
    const PreparedProviderRequest& request, ProviderDispatchBudget budget);

class NEOGRAPH_API ProviderBudgetClaim final {
public:
    ProviderBudgetClaim();
    ~ProviderBudgetClaim();
    ProviderBudgetClaim(ProviderBudgetClaim&&) noexcept;
    ProviderBudgetClaim& operator=(ProviderBudgetClaim&&) noexcept;
    ProviderBudgetClaim(const ProviderBudgetClaim&) = delete;
    ProviderBudgetClaim& operator=(const ProviderBudgetClaim&) = delete;
    bool active() const noexcept;
    std::uint64_t amount() const noexcept;
    asio::awaitable<void> begin_managed_effect(
        const PreparedProviderRequest& request, std::string effect_id = {});
    asio::awaitable<void> settle_managed(
        sp::runtime::Result result, std::exception_ptr dispatch_error = {});
    void mark_dispatched();
    std::uint64_t settle(sp::runtime::Result result);
private:
    ProviderBudgetClaim(ProviderDispatchBudget budget, std::uint64_t amount);
    void release_undispatched() noexcept;
    std::uint64_t settle_accounting(sp::runtime::Result result);
    ProviderDispatchBudget budget_;
    std::uint64_t amount_ = 0;
    std::unique_ptr<graph::ManagedBudgetEffectReceipt> managed_effect_;
    bool dispatched_ = false, settled_ = false, writeahead_committed_ = false;
    friend ProviderBudgetClaim reserve_provider_dispatch(
        const PreparedProviderRequest&, ProviderDispatchBudget);
};

/// The typed outcome of each SchemaProvider endpoint request that
/// Provider::prepare_endpoint takes (#531): images, decisions, embeddings,
/// rerank, moderation, speech, transcription and translation, video jobs,
/// files, batches, token counts and OpenRouter generation statistics. A chat
/// request is not one; it goes through Provider::prepare.
template <class Request> struct EndpointOutcome {};
template <> struct EndpointOutcome<sp::images::Request> { using type = sp::images::Outcome; };
template <> struct EndpointOutcome<sp::decisions::Request> { using type = sp::decisions::Outcome; };
template <> struct EndpointOutcome<sp::embeddings::Request> { using type = sp::embeddings::Outcome; };
template <> struct EndpointOutcome<sp::rerank::Request> { using type = sp::rerank::Outcome; };
template <> struct EndpointOutcome<sp::moderation::Request> { using type = sp::moderation::Outcome; };
template <> struct EndpointOutcome<sp::speech::Request> { using type = sp::speech::Outcome; };
template <> struct EndpointOutcome<sp::transcription::Request> { using type = sp::transcription::Outcome; };
template <> struct EndpointOutcome<sp::videos::Request> { using type = sp::jobs::Outcome; };
template <> struct EndpointOutcome<sp::jobs::Poll> { using type = sp::jobs::Outcome; };
template <> struct EndpointOutcome<sp::jobs::Download> { using type = sp::jobs::DownloadOutcome; };
template <> struct EndpointOutcome<sp::files::Upload> { using type = sp::files::Outcome; };
template <> struct EndpointOutcome<sp::files::Get> { using type = sp::files::Outcome; };
template <> struct EndpointOutcome<sp::files::List> { using type = sp::files::PageOutcome; };
template <> struct EndpointOutcome<sp::files::Delete> { using type = sp::files::DeleteOutcome; };
template <> struct EndpointOutcome<sp::files::Download> { using type = sp::jobs::DownloadOutcome; };
template <> struct EndpointOutcome<sp::files::Start> { using type = sp::files::ProgressOutcome; };
template <> struct EndpointOutcome<sp::files::Part> { using type = sp::files::ProgressOutcome; };
template <> struct EndpointOutcome<sp::files::Complete> { using type = sp::files::ProgressOutcome; };
template <> struct EndpointOutcome<sp::files::Query> { using type = sp::files::ProgressOutcome; };
template <> struct EndpointOutcome<sp::files::Cancel> { using type = sp::files::ProgressOutcome; };
template <> struct EndpointOutcome<sp::batches::Submit> { using type = sp::batches::Outcome; };
template <> struct EndpointOutcome<sp::batches::Get> { using type = sp::batches::Outcome; };
template <> struct EndpointOutcome<sp::batches::Cancel> { using type = sp::batches::Outcome; };
template <> struct EndpointOutcome<sp::batches::Delete> { using type = sp::batches::DeleteOutcome; };
template <> struct EndpointOutcome<sp::batches::Results> { using type = sp::jobs::DownloadOutcome; };
template <> struct EndpointOutcome<sp::tokens::Count> { using type = sp::tokens::Outcome; };
template <> struct EndpointOutcome<sp::generations::Get> { using type = sp::generations::Outcome; };
template <class Request>
concept EndpointRequest = requires { typename EndpointOutcome<Request>::type; };
template <EndpointRequest Request>
using EndpointOutcomeOf = typename EndpointOutcome<Request>::type;

namespace detail {
template <EndpointRequest Request>
EndpointPreparation prepare_endpoint_call(sp::runtime::Client& client, sp::runtime::RunOptions options,
                                          const Request& request) {
    return {client.prepare(request, std::move(options)).release(), std::nullopt, std::nullopt};
}
/// An upload (#541) is named by its metadata and by the SHA-256, offset and size of the bytes it sends, whether they
/// are in memory or a slice of a file, and never by the file's path. The slice is read, in bounded pieces, when the
/// call is prepared; a file that is missing or shorter than the slice is the preparation's `error`, and the SDK is
/// told the digest so the file it sends is the one that was hashed.
NEOGRAPH_API EndpointPreparation prepare_endpoint_call(sp::runtime::Client& client, sp::runtime::RunOptions options,
                                                       const sp::files::Upload& request);
NEOGRAPH_API EndpointPreparation prepare_endpoint_call(sp::runtime::Client& client, sp::runtime::RunOptions options,
                                                       const sp::files::Part& request);
template <EndpointRequest Request>
class TypedEndpointPayload final : public EndpointPayload {
public:
    explicit TypedEndpointPayload(Request request) : request_(std::move(request)) {}
    EndpointPreparation prepare(sp::runtime::Client& client, sp::runtime::RunOptions options) const override {
        return prepare_endpoint_call(client, std::move(options), request_);
    }
private:
    Request request_;
};
/// Starts one SDK endpoint call. It receives the function the call's outcome
/// callback runs, and returns the function that cancels the call.
using EndpointStart = std::function<std::function<void()>(std::function<void()> notify)>;
/// Runs `start` and resumes once the call's outcome callback has run. A
/// cancelled awaiting coroutine cancels the call and still waits for it.
NEOGRAPH_API asio::awaitable<void> await_endpoint(EndpointStart start);
} // namespace detail

/// Capability identity plus a single prepared, owned operation contract.
/// Returned awaitables own all operation state, including the runtime client;
/// neither request nor Provider object must survive coroutine scheduling.
class NEOGRAPH_API Provider {
public:
    virtual ~Provider() = default;
    virtual std::string get_name() const = 0;
    virtual std::string_view family() const noexcept = 0;
    virtual PreparedProviderRequest prepare(ProviderRequest request) = 0;
    sp::runtime::Result dispatch(PreparedProviderRequest request);
    asio::awaitable<sp::runtime::Result> dispatch_async(PreparedProviderRequest request);
    sp::runtime::Result invoke(ProviderRequest request);
    asio::awaitable<sp::runtime::Result> invoke_async(ProviderRequest request);
    static std::string request_digest(const PreparedProviderRequest& request);
    static std::optional<std::uint64_t> conservative_token_upper_bound(const PreparedProviderRequest& request);
    /// Most model invocations one dispatch of this request can bill: the
    /// request's model invocations times the transport attempts its retry
    /// policy allows. Empty when the prepared request does not state them.
    static std::optional<std::uint64_t> invocation_upper_bound(const PreparedProviderRequest& request);

    /// Whether this provider forwards SchemaProvider endpoint calls.
    bool has_endpoints() const { return endpoint_runtime() != nullptr; }

    /// SchemaProvider's non-chat endpoints at this provider's origin (#531). They
    /// run on the client its chat calls use, with the same credential, admission
    /// slots, retry budget, deadlines and error classification. The descriptor
    /// policy admits each endpoint when the call is prepared; a refused call
    /// carries its error and completes as a Failure without reaching the network.
    /// Calls are buffered and sent as given: SchemaProvider::Defaults apply to
    /// chat calls only, and nothing here journals, reserves or charges a budget.
    /// A provider without SchemaProvider endpoints throws std::logic_error.
    /// Preparation throws sp::runtime::AdmissionError when no slot is free.
    template <EndpointRequest Request>
    sp::runtime::PreparedEndpoint<EndpointOutcomeOf<Request>> prepare_endpoint(
        Request request, sp::runtime::RunOptions options = {}) {
        return require_endpoint_runtime()->prepare(std::move(request), std::move(options));
    }
    /// Runs a prepared call on this provider's client and blocks for its outcome.
    template <class Outcome>
    Outcome complete_endpoint(sp::runtime::PreparedEndpoint<Outcome> prepared) {
        return require_endpoint_runtime()->complete(std::move(prepared));
    }
    template <EndpointRequest Request>
    EndpointOutcomeOf<Request> complete_endpoint(Request request, sp::runtime::RunOptions options = {}) {
        return require_endpoint_runtime()->complete(std::move(request), std::move(options));
    }
    /// The same call without blocking a thread. The awaitable owns the client and
    /// the call; cancelling the awaiting coroutine cancels the call, and its
    /// outcome is then the SDK's Cancelled failure.
    template <class Outcome>
    asio::awaitable<Outcome> complete_endpoint_async(sp::runtime::PreparedEndpoint<Outcome> prepared) {
        return run_endpoint(require_endpoint_runtime(), std::move(prepared));
    }
    template <EndpointRequest Request>
    asio::awaitable<EndpointOutcomeOf<Request>> complete_endpoint_async(
        Request request, sp::runtime::RunOptions options = {}) {
        auto client = require_endpoint_runtime();
        try { return run_endpoint(client, client->prepare(std::move(request), std::move(options))); }
        catch (const sp::runtime::AdmissionError& error) { return refused_endpoint<EndpointOutcomeOf<Request>>(error); }
    }
protected:
    /// The SchemaProvider client whose endpoints this provider forwards, the same
    /// one for the provider's lifetime. Null, the default, means none.
    virtual std::shared_ptr<sp::runtime::Client> endpoint_runtime() const { return nullptr; }
    /// For a provider that wraps another: forward the inner provider's endpoints
    /// by returning this from endpoint_runtime().
    static std::shared_ptr<sp::runtime::Client> endpoint_runtime_of(const Provider& inner) {
        return inner.endpoint_runtime();
    }
    using LocalDispatch = std::function<asio::awaitable<sp::runtime::Result>(
        const PreparedProviderRequest&, const std::function<void(const sp::Event&)>&)>;
    static PreparedProviderRequest prepare_local(
        std::shared_ptr<sp::runtime::Client> client, ProviderRequest request, LocalDispatch dispatch);
    static PreparedProviderRequest prepare_runtime(
        std::shared_ptr<sp::runtime::Client> client, ProviderRequest request);
    static PreparedProviderRequest reject_preparation(sp::Error error);
    static PreparedProviderRequest observe_prepared(
        PreparedProviderRequest request,
        std::function<void(const PreparedProviderRequest&)> before,
        std::function<void(sp::runtime::Result)> after,
        std::function<void(const sp::Event&)> event = {},
        std::function<void(std::exception_ptr)> on_error = {}) noexcept;
private:
    static asio::awaitable<sp::runtime::Result> dispatch_impl(PreparedProviderRequest request);
    static asio::awaitable<sp::runtime::Result> dispatch_operation(PreparedProviderRequest request);
    std::shared_ptr<sp::runtime::Client> require_endpoint_runtime() const;
    template <class Outcome>
    static asio::awaitable<Outcome> run_endpoint(
        std::shared_ptr<sp::runtime::Client> client, sp::runtime::PreparedEndpoint<Outcome> prepared) {
        sp::runtime::EndpointCall<Outcome> call;
        try {
            co_await detail::await_endpoint([&](std::function<void()> notify) {
                // The callback keeps the client alive until the SDK has delivered.
                call = client->start(std::move(prepared), [client, notify = std::move(notify)](Outcome) { notify(); });
                return std::function<void()>([&call] { call.cancel(); });
            });
        } catch (const sp::runtime::AdmissionError& error) {
            co_return sp::runtime::detail::typed_outcome(*error.outcome(), static_cast<const Outcome*>(nullptr));
        }
        // The callback has run; join fences the slot release and returns the outcome it was given.
        co_return call.join();
    }
    template <class Outcome>
    static asio::awaitable<Outcome> refused_endpoint(sp::runtime::AdmissionError error) {
        co_return sp::runtime::detail::typed_outcome(*error.outcome(), static_cast<const Outcome*>(nullptr));
    }
};

/// An endpoint call as a ProviderRequest (#531), so it takes every path a chat
/// call takes: dispatch hooks, observers, the provider call broker and its
/// journal, and token budgets, which refuse it because it has no output cap.
/// Its outcome is untyped; the endpoint's typed() (sp::images::typed, ...) or
/// sp::runtime::detail::typed_outcome types it. A provider without SchemaProvider
/// endpoints refuses it when it is prepared.
template <EndpointRequest Request>
ProviderRequest make_endpoint_request(Request request) {
    ProviderRequest result;
    result.endpoint = std::make_shared<const detail::TypedEndpointPayload<Request>>(std::move(request));
    return result;
}

NEOGRAPH_API ProviderRequest make_provider_request(
    const Provider& provider, std::string model, std::vector<sp::Message> messages,
    std::vector<ChatTool> tools = {}, ProviderControls controls = {},
    ProviderMode mode = ProviderMode::Collect);
NEOGRAPH_API void set_provider_request_messages(ProviderRequest& request, std::vector<sp::Message> messages);
NEOGRAPH_API void clear_provider_request_messages(ProviderRequest& request);
NEOGRAPH_API const std::vector<sp::Message>& provider_request_messages(const ProviderRequest& request);

} // namespace neograph
