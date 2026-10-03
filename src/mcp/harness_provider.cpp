#include <neograph/mcp/harness.h>
#include <neograph/mcp/json_schema.h>
#include <neograph/provider.h>
#include <neograph/provider_outcome_codec.h>
#include <json/json.h>

#include "harness_journal_internal.h"

#include <asio/error.hpp>
#include <asio/system_error.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace neograph::mcp {
namespace {

bool is_within(const std::filesystem::path& child, const std::filesystem::path& root) {
    auto child_it = child.begin();
    auto root_it = root.begin();
    for (; root_it != root.end(); ++root_it, ++child_it) {
        if (child_it == child.end() || *child_it != *root_it) return false;
    }
    return true;
}

std::filesystem::path resolved_path(const std::filesystem::path& value) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(value, error);
    if (error) {
        throw std::runtime_error("cannot resolve Harness path: " + value.string());
    }
    auto resolved = std::filesystem::weakly_canonical(absolute, error);
    if (error) {
        throw std::runtime_error("cannot canonicalize Harness path: " + absolute.string());
    }
    return resolved;
}

json enforce_path_policy(const HarnessWorkerCall& call, const json& tool, const json& arguments) {
    const auto path_arguments = tool.value("path_arguments", json::array());
    if (path_arguments.empty()) return arguments;

    const auto roots = call.policy.value("workspace_roots", json::array());
    if (roots.empty()) {
        throw std::runtime_error("path-bearing tool requires policy.workspace_roots");
    }

    std::vector<std::filesystem::path> normalized_roots;
    for (const auto& root_json : roots) {
        const auto root = root_json.get<std::string>();
        if (root.empty()) {
            throw std::runtime_error("policy.workspace_roots must not contain empty paths");
        }
        normalized_roots.push_back(resolved_path(root));
    }
    auto normalized_arguments = arguments;
    for (const auto& key_json : path_arguments) {
        const auto key = key_json.get<std::string>();
        if (!arguments.contains(key) || !arguments[key].is_string()) continue;
        auto candidate = std::filesystem::path(arguments[key].get<std::string>());
        if (candidate.is_relative()) candidate = normalized_roots.front() / candidate;
        candidate = resolved_path(candidate);
        bool allowed = false;
        for (const auto& root : normalized_roots) {
            if (is_within(candidate, root)) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            throw std::runtime_error("tool path escapes configured workspace roots: " +
                                     candidate.string());
        }
        normalized_arguments[key] = candidate.string();
    }
    return normalized_arguments;
}

std::vector<ChatTool> chat_tools(const json& catalog) {
    std::vector<ChatTool> tools;
    for (const auto& entry : catalog) {
        ChatTool tool;
        tool.name = entry["id"].get<std::string>();
        tool.description = entry.value("description", "");
        tool.parameters = entry["input_schema"];
        tools.push_back(std::move(tool));
    }
    return tools;
}

std::string worker_prompt(const HarnessWorkerCall& call) {
    json contract = {
        {"objective", call.task.value("objective", "")},
        {"acceptance", call.task.value("acceptance", json::array())},
        {"instructions", call.worker.value("instructions", "")},
        {"output_schema", call.worker["output_schema"]},
    };
    if (call.task.contains("contract_manifest"))
        contract["frozen_contract"] = call.task.at("contract_manifest");
    if (!call.repair_feedback.empty()) {
        contract["repair_feedback"] = call.repair_feedback;
    }
    if (call.resume_value) {
        contract["host_resume"] = *call.resume_value;
    }
    return "Execute this worker contract. Use only the supplied tools. Return only "
           "one JSON value conforming to output_schema, without Markdown fences.\n" +
           contract.dump(2);
}

std::string host_call_id() {
    static const std::uint64_t        nonce = std::random_device{}();
    static std::atomic<std::uint64_t> sequence{1};
    std::ostringstream                out;
    out << "hcall_" << std::hex << nonce << '_' << sequence.fetch_add(1, std::memory_order_relaxed);
    return out.str();
}
json effect_descriptor(const json& executor, const std::string& call_id,
                       std::string_view run_id) {
    if (!executor.contains("effect")) return json();
    const auto effect_namespace =
        run_id.empty() ? detail::current_harness_run_id() : std::string(run_id);
    if (effect_namespace.empty()) {
        throw std::runtime_error("Harness host-brokered effect is missing its run namespace");
    }
    const auto effect = executor["effect"];
    const auto effect_id = effect_namespace + ":" + call_id;
    return {
        {"effect_id", effect_id},
        {"idempotency_key", effect_id},
        {"idempotency", effect.at("idempotency")},
        {"status_query", effect.value("status_query", false)},
        {"fencing", effect.value("fencing", false)},
    };
}

class ProviderDeadline {
public:
    ProviderDeadline(std::shared_ptr<graph::CancelToken> parent,
                     std::shared_ptr<graph::CancelToken> child,
                     std::chrono::steady_clock::time_point deadline)
        : parent_(std::move(parent)), child_(std::move(child)), deadline_(deadline) {
        timer_ = std::thread([this, deadline] {
            std::unique_lock lock(mutex_);
            if (!cv_.wait_until(lock, deadline,
                              [this] { return finished_; })) {
                // This is the deadline's linearization point. If the parent
                // was already cancelled, preserve that earlier cause even if
                // the provider delays unwinding until after the timer fires.
                const auto cause = parent_->is_cancelled()
                                       ? ProviderCancellationCause::Parent
                                       : ProviderCancellationCause::Timeout;
                cause_.store(cause, std::memory_order_release);
                child_->cancel();
            }
        });
    }

    void finish() {
        {
            std::lock_guard lock(mutex_);
            // A completed dispatch may win the mutex before a delayed timer
            // thread; do not turn that scheduling delay into renewed time.
            if (!finished_ && std::chrono::steady_clock::now() >= deadline_ &&
                cause_.load(std::memory_order_acquire) == ProviderCancellationCause::None) {
                cause_.store(parent_->is_cancelled() ? ProviderCancellationCause::Parent
                                                    : ProviderCancellationCause::Timeout,
                             std::memory_order_release);
            }
            finished_ = true;
        }
        cv_.notify_one();
        if (timer_.joinable()) timer_.join();
    }

    ~ProviderDeadline() { finish(); }

    bool timeout_won() const noexcept {
        return cause_.load(std::memory_order_acquire) ==
               ProviderCancellationCause::Timeout;
    }

private:
    enum class ProviderCancellationCause : std::uint8_t {
        None,
        Parent,
        Timeout,
    };

    std::shared_ptr<graph::CancelToken> parent_;
    std::shared_ptr<graph::CancelToken> child_;
    std::chrono::steady_clock::time_point deadline_;
    std::atomic<ProviderCancellationCause> cause_{ProviderCancellationCause::None};
    std::mutex                          mutex_;
    std::condition_variable             cv_;
    bool                                finished_ = false;
    std::thread                         timer_;
};

class TokenReservation {
public:
    explicit TokenReservation(std::shared_ptr<UsageAccumulator> usage)
        : usage_(std::move(usage)) {}

    ~TokenReservation() { if (!dispatched_) release(); }

    void hold(std::uint64_t tokens) noexcept { held_ = tokens; }

    void dispatched() noexcept { dispatched_ = true; }

    void release() noexcept {
        if (held_ == 0) return;
        if (usage_) usage_->release_reservation(held_);
        held_ = 0;
    }

    void settle(const sp::Usage& usage) noexcept {
        if (!usage_) return;
        if (held_ != 0) {
            usage_->settle_reservation(held_, usage);
            held_ = 0;
        } else {
            usage_->add(usage);
        }
    }

private:
    std::shared_ptr<UsageAccumulator> usage_;
    std::uint64_t                     held_ = 0;
    bool                              dispatched_ = false;
};

} // namespace

HarnessWorkerExecutor make_provider_harness_executor(HarnessProviderExecutorConfig config) {
    if (!config.provider) {
        throw std::invalid_argument("Harness provider executor requires a provider");
    }
    if (config.max_tool_rounds == 0 || config.max_tool_rounds > 64) {
        throw std::invalid_argument("max_tool_rounds must be between 1 and 64");
    }

    return [config = std::move(config)](const HarnessWorkerCall&                   call,
                                        const std::shared_ptr<graph::CancelToken>& cancel) {
        bool provider_effect_uncertain = false;
        const auto execute = [&]() -> HarnessWorkerResponse {
        if (call.model_token_budget != 0 && !call.usage) {
            return HarnessWorkerResponse::tool_error(
                "bounded Harness provider execution requires a UsageAccumulator");
        }
        std::map<std::string, json> catalog;
        for (const auto& tool : call.tool_catalog) {
            catalog[tool["id"].get<std::string>()] = tool;
        }

        std::size_t max_tool_rounds = config.max_tool_rounds;
        if (call.worker.contains("max_provider_tool_rounds")) {
            const auto& declared = call.worker.at("max_provider_tool_rounds");
            if (!declared.is_number_unsigned() || declared.get<std::uint64_t>() == 0) {
                return HarnessWorkerResponse::tool_error(
                    "worker max_provider_tool_rounds must be a positive unsigned integer");
            }
            max_tool_rounds =
                std::min(config.max_tool_rounds, static_cast<std::size_t>(
                                                declared.get<std::uint64_t>()));
        }

        ProviderControls controls;
        controls.temperature = 0.0;
        const auto budget = call.worker.value("_harness_provider_budget", json::object());
        const int provider_timeout = budget.value("provider_timeout_seconds", 0);
        const auto max_output_value = budget.value("max_output_tokens", json(-1));
        if (max_output_value != json(-1)) {
            if ((!max_output_value.is_number_unsigned() &&
                 !max_output_value.is_number_integer()) ||
                (max_output_value.is_number_integer() &&
                 !max_output_value.is_number_unsigned() &&
                 max_output_value.get<std::int64_t>() <= 0) ||
                (max_output_value.is_number_unsigned() &&
                 max_output_value.get<std::uint64_t>() == 0)) {
                return HarnessWorkerResponse::tool_error(
                    "Harness max_output_tokens must be positive or absent");
            }
            controls.max_output_tokens = max_output_value.get<std::uint64_t>();
        }
        const auto input_token_ceiling =
            budget.value("input_token_ceiling", std::uint64_t{0});
        const auto maximum_reservation = std::numeric_limits<std::uint64_t>::max();
        if (call.model_token_budget != 0 &&
            (!controls.max_output_tokens || input_token_ceiling == 0 ||
             *controls.max_output_tokens > maximum_reservation - input_token_ceiling)) {
            return HarnessWorkerResponse::tool_error(
                "bounded Harness provider request requires representable input/output ceilings");
        }
        const auto request_token_reservation =
            call.model_token_budget == 0
                ? std::uint64_t{0}
                : input_token_ceiling + *controls.max_output_tokens;
        const auto tools = chat_tools(call.tool_catalog);
        std::vector<sp::Message> history;
        sp::Message initial;
        initial.role = sp::Role::User;
        initial.parts.emplace_back(sp::Text{worker_prompt(call)});
        history.push_back(std::move(initial));

        std::size_t provider_round = 0, tool_rounds = 0;
        const auto model_budget_reached = [&] {
            if (!call.usage || call.model_token_budget == 0) return false;
            const auto total_tokens = call.usage->total_tokens_wide();
            return total_tokens >= call.model_token_budget;
        };
        while (true) {
            const bool budget_reached = model_budget_reached();
            if ((call.budget_exhausted &&
                 call.budget_exhausted->load(std::memory_order_acquire)) ||
                budget_reached) {
                if (budget_reached && call.budget_exhausted)
                    call.budget_exhausted->store(true, std::memory_order_release);
                cancel->cancel();
                return HarnessWorkerResponse::cancelled(
                    "Program model-token budget exhausted before provider call");
            }
            ++provider_round;
            cancel->throw_if_cancelled("before provider Harness completion");
            // Each provider completion gets a fresh child scope. A previous
            // round's deadline must not cancel a later round after a slow
            // capability call or host-brokered pause.
            const auto provider_cancel = cancel->fork();
            const auto provider_correlation = detail::journal_correlation_id("provider");
            const auto provider_started = std::chrono::steady_clock::now();
            PreparedProviderRequest prepared;
            try {
                auto request = make_provider_request(
                    *config.provider, config.model, history, tools, controls, ProviderMode::Collect);
                request.cancel_token = provider_cancel;
                if (provider_timeout > 0)
                    request.options.deadline =
                        provider_started + std::chrono::seconds(provider_timeout);
                prepared = config.provider->prepare(std::move(request));
            } catch (const std::exception& error) {
                return HarnessWorkerResponse::tool_error(error.what());
            }
            if (!prepared.valid()) {
                const auto* error = prepared.error();
                if (error && error->kind == sp::ErrorKind::Cancelled)
                    return HarnessWorkerResponse::cancelled(error->safe_message);
                if (error && error->kind == sp::ErrorKind::DeadlineExceeded)
                    return HarnessWorkerResponse::timeout(error->safe_message);
                return HarnessWorkerResponse::tool_error(
                    error ? error->safe_message : "provider preparation is invalid");
            }
            if (call.model_token_budget != 0) {
                const auto admitted_bound = Provider::conservative_token_upper_bound(prepared);
                if (!admitted_bound || request_token_reservation < *admitted_bound) {
                    return HarnessWorkerResponse::tool_error(
                        "Harness token grant does not cover admitted model input/output limits");
                }
            }
            const auto absolute_deadline = prepared.deadline();
            const auto request_digest = Provider::request_digest(prepared);
            if (cancel->is_cancelled())
                return HarnessWorkerResponse::cancelled();
            if (std::chrono::steady_clock::now() >= absolute_deadline)
                return HarnessWorkerResponse::timeout("provider request exceeded its timeout");
            TokenReservation reservation(call.usage);
            if (call.model_token_budget != 0) {
                const auto reserved_tokens = request_token_reservation;
                if (!call.usage->try_reserve(reserved_tokens, call.model_token_budget)) {
                    if (call.budget_exhausted)
                        call.budget_exhausted->store(true, std::memory_order_release);
                    cancel->cancel();
                    detail::append_current_harness_journal_event(
                        "provider.call.budget_exhausted",
                        {{"model_token_budget", call.model_token_budget},
                         {"model_tokens_reserved", reserved_tokens}},
                        provider_correlation);
                    return HarnessWorkerResponse::cancelled(
                        "Program model-token budget exhausted before provider dispatch");
                }
                reservation.hold(reserved_tokens);
            }
            detail::append_current_harness_journal_event(
                "provider.call.started",
                {{"message_count", history.size()},
                 {"max_output_tokens",
                  controls.max_output_tokens ? json(*controls.max_output_tokens) : json(nullptr)},
                 {"model", config.model},
                 {"provider", config.provider->get_name()},
                 {"request_digest", request_digest},
                 {"provider_timeout_seconds",
                  provider_timeout == 0 ? json(nullptr) : json(provider_timeout)},
                 {"round", provider_round},
                 {"tool_count", tools.size()}},
                provider_correlation);
            if (cancel->is_cancelled() ||
                std::chrono::steady_clock::now() >= absolute_deadline) {
                const bool cancelled_before_dispatch = cancel->is_cancelled();
                detail::append_current_harness_journal_event(
                    "provider.call.completed",
                    {{"duration_ms",
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - provider_started).count()},
                     {"outcome", cancelled_before_dispatch ? "cancelled" : "timeout"},
                     {"dispatched", false}},
                    provider_correlation);
                if (cancelled_before_dispatch)
                    return HarnessWorkerResponse::cancelled();
                return HarnessWorkerResponse::timeout("provider request exceeded its timeout");
            }
            auto deadline = std::make_unique<ProviderDeadline>(
                cancel, provider_cancel, absolute_deadline);
            const auto deadline_expired = [&deadline] {
                return deadline && deadline->timeout_won();
            };
            const auto parent_cancelled = [&] {
                return cancel->is_cancelled() && !deadline_expired();
            };
            const auto duration_ms = [&] {
                return std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - provider_started).count();
            };
            sp::runtime::Result result;
            const bool prior_uncertainty = provider_effect_uncertain;
            try {
                // Once dispatch is entered, exceptions/unknown delivery retain
                // the reservation. Only accepted, fully-known usage can settle it.
                provider_effect_uncertain = true;
                if (call.usage)
                    call.usage->remember_provider_effect(
                        call.run_id + ":" + provider_correlation + ":" + request_digest);
                reservation.dispatched();
                result = config.provider->dispatch(std::move(prepared));
            } catch (const graph::CancelledException& error) {
                detail::append_current_harness_journal_event(
                    "provider.call.completed",
                    {{"duration_ms", duration_ms()}, {"error", error.what()},
                     {"outcome", deadline_expired() ? "timeout" : "cancelled"}},
                    provider_correlation);
                if (parent_cancelled()) return HarnessWorkerResponse::cancelled(error.what());
                if (deadline_expired())
                    return HarnessWorkerResponse::timeout("provider request exceeded its timeout");
                return HarnessWorkerResponse::cancelled(error.what());
            } catch (const asio::system_error& error) {
                detail::append_current_harness_journal_event(
                    "provider.call.completed",
                    {{"duration_ms", duration_ms()}, {"error", error.what()},
                     {"outcome", parent_cancelled() ? "cancelled"
                                 : deadline_expired() || error.code() == asio::error::timed_out
                                     ? "timeout" : "error"}},
                    provider_correlation);
                if (parent_cancelled()) return HarnessWorkerResponse::cancelled(error.what());
                if (deadline_expired() || error.code() == asio::error::timed_out)
                    return HarnessWorkerResponse::timeout(error.what());
                return HarnessWorkerResponse::tool_error(error.what());
            } catch (const std::exception& error) {
                detail::append_current_harness_journal_event(
                    "provider.call.completed",
                    {{"duration_ms", duration_ms()}, {"error", error.what()},
                     {"outcome", parent_cancelled() ? "cancelled"
                                 : deadline_expired() ? "timeout" : "error"}},
                    provider_correlation);
                if (parent_cancelled()) return HarnessWorkerResponse::cancelled(error.what());
                if (deadline_expired())
                    return HarnessWorkerResponse::timeout("provider request exceeded its timeout");
                return HarnessWorkerResponse::tool_error(error.what());
            }
            deadline->finish();
            const bool timed_out = deadline_expired();
            const bool cancelled = parent_cancelled();
            // The timer owns the prepared absolute deadline, not tool execution.
            deadline.reset();
            if (!result) {
                detail::append_current_harness_journal_event(
                    "provider.call.completed",
                    {{"duration_ms", duration_ms()}, {"outcome", "error"},
                     {"error", "provider returned no outcome"}},
                    provider_correlation);
                return HarnessWorkerResponse::tool_error("provider returned no outcome");
            }
            const auto* completion = std::get_if<sp::Completion>(result.get());
            provider_effect_uncertain = prior_uncertainty || timed_out || cancelled;
            if (completion) {
                const auto& usage = completion->usage;
                provider_effect_uncertain =
                    provider_effect_uncertain || completion->attempt.prior_usage_unknown ||
                    completion->attempt.transport_internal_resends != 0 ||
                    usage.stage != sp::UsageStage::Final ||
                    usage.quality != sp::UsageQuality::Consistent ||
                    !usage.input_total || !usage.output_total;
            } else {
                provider_effect_uncertain = provider_effect_uncertain ||
                    !provider_failure_proves_not_sent(std::get<sp::Failure>(*result));
            }
            if (completion && !completion->attempt.prior_usage_unknown &&
                completion->attempt.transport_internal_resends == 0 && !timed_out && !cancelled)
                reservation.settle(completion->usage);
            else if (call.usage)
                call.usage->observe(outcome_usage(*result));
            const auto archive_binding = call.run_id + ":" + provider_correlation + ":" + request_digest;
            std::size_t client_tool_call_count = 0;
            if (completion) {
                for (const auto& message : completion->messages) {
                    for (const auto& part : message.parts) {
                        const auto* tool = std::get_if<sp::ToolCall>(&part);
                        if (tool && tool->kind == sp::ToolCallKind::ClientExecuted)
                            ++client_tool_call_count;
                    }
                }
            }
            detail::append_current_harness_journal_event(
                "provider.call.completed",
                {{"duration_ms", duration_ms()},
                 {"outcome", timed_out ? "timeout" : cancelled ? "cancelled"
                             : !completion ? "error"
                             : client_tool_call_count == 0 ? "content" : "tool_calls"},
                 {"tool_call_count", client_tool_call_count},
                 {"usage", provider_codec::encode_usage(outcome_usage(*result))},
                 {"provider_outcome",
                  config.native_archive
                      ? provider_codec::encode_outcome(*result, config.native_archive, archive_binding)
                      : provider_codec::observe_outcome(*result)}},
                provider_correlation);
            if (cancelled || (cancel->is_cancelled() && !timed_out)) {
                provider_effect_uncertain = true;
                return HarnessWorkerResponse::cancelled(
                    "provider request cancelled before completion was accepted");
            }
            if (timed_out)
                return HarnessWorkerResponse::timeout("provider request exceeded its timeout");
            if (const auto* failure = std::get_if<sp::Failure>(result.get())) {
                if (failure->error.kind == sp::ErrorKind::Cancelled)
                    return HarnessWorkerResponse::cancelled(failure->error.safe_message);
                if (failure->error.kind == sp::ErrorKind::DeadlineExceeded)
                    return HarnessWorkerResponse::timeout(failure->error.safe_message);
                return HarnessWorkerResponse::tool_error(failure->error.safe_message);
            }
            std::vector<const sp::ToolCall*> tool_calls;
            tool_calls.reserve(client_tool_call_count);
            for (const auto& message : completion->messages) {
                for (const auto& part : message.parts) {
                    if (const auto* invalid = std::get_if<sp::InvalidToolCall>(&part)) {
                        return HarnessWorkerResponse::tool_error(
                            "provider returned an invalid tool call: " + invalid->name);
                    }
                    if (const auto* tool = std::get_if<sp::ToolCall>(&part);
                        tool && tool->kind == sp::ToolCallKind::ClientExecuted) {
                        if (message.role != sp::Role::Assistant || tool->id.empty() ||
                            tool->name.empty() || !tool->input || !tool->input->root().is_object()) {
                            return HarnessWorkerResponse::tool_error(
                                "provider returned an invalid client tool call");
                        }
                        tool_calls.push_back(tool);
                    }
                }
            }
            const auto model_tokens_committed =
                call.usage ? call.usage->total_tokens_wide() : 0;
            const auto reported_usage = call.usage ? call.usage->snapshot() : sp::Usage{};
            const auto exceeds_reported_budget = [&](const sp::Usage& usage) {
                return call.model_token_budget != 0 &&
                       usage.stage == sp::UsageStage::Final &&
                       usage.quality == sp::UsageQuality::Consistent &&
                       ((usage.total && usage.total->value > call.model_token_budget) ||
                        (usage.input_total && usage.output_total &&
                         (usage.input_total->value > call.model_token_budget ||
                          usage.output_total->value >
                              call.model_token_budget - usage.input_total->value)));
            };
            const bool reported_budget_overrun =
                exceeds_reported_budget(reported_usage) ||
                exceeds_reported_budget(completion->usage);
            if (call.model_token_budget != 0 &&
                (reported_budget_overrun ||
                 model_tokens_committed > call.model_token_budget ||
                 (!tool_calls.empty() && model_budget_reached()) ||
                 (call.budget_exhausted &&
                  call.budget_exhausted->load(std::memory_order_acquire)))) {
                if (call.budget_exhausted)
                    call.budget_exhausted->store(true, std::memory_order_release);
                cancel->cancel();
                detail::append_current_harness_journal_event(
                    "provider.call.budget_exhausted",
                    {{"model_token_budget", call.model_token_budget},
                     {"model_tokens_committed", model_tokens_committed},
                     {"reported_usage", provider_codec::encode_usage(reported_usage)}},
                    provider_correlation);
                return HarnessWorkerResponse::cancelled(
                    "Program model-token budget exhausted during provider call");
            }
            if (tool_calls.empty()) {
                // Only the final Harness JSON result uses a Text projection.
                // Provider history and journal outcomes remain fully ordered.
                const auto text = outcome_text(*result);
                if (text.empty())
                    return HarnessWorkerResponse::empty("provider returned empty content");
                try {
                    return HarnessWorkerResponse::success(json::parse(text));
                } catch (const std::exception& error) {
                    return HarnessWorkerResponse::parse_error(error.what());
                }
            }

            if (tool_rounds >= max_tool_rounds) {
                if (call.budget_exhausted) {
                    call.budget_exhausted->store(true, std::memory_order_release);
                    provider_cancel->cancel();
                    cancel->cancel();
                    detail::append_current_harness_journal_event(
                        "provider.tool_round_budget_exhausted",
                        {{"max_provider_tool_rounds", max_tool_rounds},
                         {"provider_round", provider_round},
                         {"tool_rounds", tool_rounds}},
                        provider_correlation);
                    return HarnessWorkerResponse::cancelled(
                        "Program provider/tool-round budget exhausted");
                }
                return HarnessWorkerResponse::tool_error(
                    "provider exhausted max_tool_rounds without a final result");
            }
            ++tool_rounds;

            history.insert(history.end(), completion->messages.begin(), completion->messages.end());
            for (const auto* typed_tool_call : tool_calls) {
                const auto& tool_call = *typed_tool_call;
                if (cancel->is_cancelled()) {
                    return HarnessWorkerResponse::cancelled(
                        "provider request cancelled before capability dispatch");
                }
                auto tool_it = catalog.find(tool_call.name);
                if (tool_it == catalog.end()) {
                    return HarnessWorkerResponse::tool_error(
                        "provider requested undeclared tool: " + tool_call.name);
                }
                try {
                    auto arguments = json::parse(tool_call.input->root().dump());
                    validate_json_value(arguments, tool_it->second["input_schema"],
                                        "Harness tool arguments", "$");
                    arguments           = enforce_path_policy(call, tool_it->second, arguments);
                    const auto executor = tool_it->second.value("executor", json::object());
                    if (executor.value("kind", "") == "host_brokered") {
                        if (cancel->is_cancelled()) {
                            return HarnessWorkerResponse::cancelled(
                                "provider request cancelled before host dispatch");
                        }
                        const auto call_id = host_call_id();
                        json pending = {
                            {"call_id", call_id},
                            {"provider_call_id", tool_call.id},
                            {"tool_id", tool_call.name},
                            {"arguments", std::move(arguments)},
                            {"result_schema",
                             tool_it->second.value("output_schema", json::object())},
                        };
                        if (const auto effect = effect_descriptor(executor, call_id, call.run_id);
                            !effect.is_null()) {
                            pending["effect"] = effect;
                        }
                        detail::append_current_harness_journal_event(
                            "host_brokered.call.requested",
                            {{"arguments", pending["arguments"]},
                             {"effect", pending.value("effect", json())},
                             {"interaction", executor.value("interaction", "tool_result")},
                             {"provider_call_id", tool_call.id},
                             {"tool_id", tool_call.name}},
                            call_id);
                        if (cancel->is_cancelled()) {
                            return HarnessWorkerResponse::cancelled(
                                "provider request cancelled before host result wait");
                        }
                        if (executor.value("interaction", "tool_result") == "input") {
                            return HarnessWorkerResponse::input_required(std::move(pending));
                        }
                        return HarnessWorkerResponse::awaiting_tool_results(std::move(pending));
                    }
                    if (!config.capability_executor) {
                        return HarnessWorkerResponse::tool_error(
                            "provider requested a tool but no capability executor is configured");
                    }
                    const auto capability_correlation =
                        tool_call.id.empty() ? detail::journal_correlation_id("capability")
                                             : tool_call.id;
                    const auto capability_started = std::chrono::steady_clock::now();
                    detail::append_current_harness_journal_event(
                        "capability.call.started",
                        {{"arguments", arguments},
                         {"executor_kind", executor.value("kind", "builtin")},
                         {"tool_id", tool_call.name}},
                        capability_correlation);
                    json result;
                    try {
                        result = config.capability_executor(tool_it->second, arguments, cancel);
                        if (tool_it->second.contains("output_schema")) {
                            validate_json_value(result, tool_it->second["output_schema"],
                                                "Harness tool result", "$");
                        }
                    } catch (const std::exception& error) {
                        detail::append_current_harness_journal_event(
                            "capability.call.completed",
                            {{"duration_ms",
                              std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - capability_started)
                                  .count()},
                             {"error", error.what()},
                             {"executor_kind", executor.value("kind", "builtin")},
                             {"outcome", cancel->is_cancelled() ? "cancelled" : "error"},
                             {"tool_id", tool_call.name}},
                            capability_correlation);
                        if (cancel->is_cancelled()) {
                            return HarnessWorkerResponse::cancelled(error.what());
                        }
                        throw;
                    }
                    if (cancel->is_cancelled()) {
                        detail::append_current_harness_journal_event(
                            "capability.call.completed",
                            {{"duration_ms",
                              std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - capability_started)
                                  .count()},
                             {"executor_kind", executor.value("kind", "builtin")},
                             {"outcome", "cancelled"},
                             {"tool_id", tool_call.name}},
                            capability_correlation);
                        return HarnessWorkerResponse::cancelled(
                            "provider request cancelled during capability dispatch");
                    }
                    detail::append_current_harness_journal_event(
                        "capability.call.completed",
                        {{"duration_ms",
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - capability_started)
                              .count()},
                         {"executor_kind", executor.value("kind", "builtin")},
                         {"outcome", "success"},
                         {"result", result},
                         {"tool_id", tool_call.name}},
                        capability_correlation);
                    sp::Message message;
                    message.role = sp::Role::Tool;
                    message.parts.emplace_back(sp::ToolResult{
                        tool_call.id, result.dump(), false,
                        sp::ToolResultHostMetadata{tool_call.name, "", false, false}});
                    history.push_back(std::move(message));
                } catch (const std::exception& error) {
                    if (cancel->is_cancelled())
                        return HarnessWorkerResponse::cancelled(error.what());
                    return HarnessWorkerResponse::tool_error(error.what());
                }
            }
            continue;


        }
        };
        auto response = execute();
        response.provider_effect_uncertain = provider_effect_uncertain;
        return response;
    };
}

} // namespace neograph::mcp
