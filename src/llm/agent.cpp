#include <neograph/llm/agent.h>
#include <neograph/async/run_sync.h>
#include <neograph/runtime_interposition_controller.h>
#include <neograph/tool_dispatch.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace neograph::llm {

void Agent::set_runtime_interposition(std::shared_ptr<RuntimeInterpositionController> controller) {
    runtime_interposition_ = std::move(controller);
}

Agent::Agent(std::shared_ptr<Provider> provider, std::vector<std::unique_ptr<Tool>> tools,
             const std::string& instructions, const std::string& model)
    : provider_(std::move(provider)), tools_(std::move(tools)),
      instructions_(instructions), model_(model) {}

std::vector<Tool*> Agent::tool_ptrs() const {
    std::vector<Tool*> result;
    result.reserve(tools_.size());
    for (const auto& tool : tools_) result.push_back(tool.get());
    return result;
}

std::vector<ChatTool> Agent::get_tool_definitions() const {
    std::vector<ChatTool> result;
    result.reserve(tools_.size());
    for (const auto& tool : tools_) result.push_back(tool->get_definition());
    return result;
}

void Agent::ensure_system_message(std::vector<sp::Message>& messages) {
    if (instructions_.empty()) return;
    const auto instruction = portable_message(ChatMessage{"system", instructions_});
    if (!messages.empty() && messages.front().role == sp::Role::System) messages.front() = instruction;
    else messages.insert(messages.begin(), instruction);
}

sp::runtime::Result Agent::complete(const std::vector<sp::Message>& messages) {
    auto request = make_provider_request(*provider_, model_, messages);
    sp::runtime::Result result;
    try {
        result = runtime_interposition_ ? runtime_interposition_->invoke(std::move(request))
                                      : provider_->invoke(std::move(request));
    } catch (const ProviderObserverError& error) {
        outcomes_.push_back(error.outcome());
        usage_->add(outcome_usage(*error.outcome()));
        throw;
    }
    if (result) outcomes_.push_back(result);
    if (result) usage_->add(outcome_usage(*result));
    return result;
}

namespace {
void append_tool_results(std::vector<sp::Message>& history, std::vector<ToolCall> calls,
                         std::vector<Tool*> tools, ToolGate gate, ToolExecutionContext execution) {
    if (execution.effect_broker) {
        const auto turns = std::count_if(history.begin(), history.end(), [](const sp::Message& message) {
            return message.role == sp::Role::Assistant && !client_tool_calls(message).empty();
        });
        execution.effect_task_id = "agent:turn:" + std::to_string(turns - 1);
    }
    auto results = neograph::async::run_sync(dispatch_tool_calls(
        std::move(calls), std::move(tools), std::move(gate), {}, std::move(execution)));
    for (const auto& result : results) history.push_back(portable_message(result));
}
}

sp::runtime::Result Agent::run(std::vector<sp::Message>& messages, int max_iterations) {
    return run(messages, max_iterations, {});
}
sp::runtime::Result Agent::run(std::vector<sp::Message>& messages, int max_iterations,
                             ToolExecutionContext execution) {
    return run_loop(messages, max_iterations, std::move(execution), ProviderMode::Collect, {});
}
sp::runtime::Result Agent::run_stream(std::vector<sp::Message>& messages,
    const std::function<void(const sp::Event&)>& observer, int max_iterations) {
    return run_stream(messages, observer, max_iterations, {});
}
sp::runtime::Result Agent::run_stream(std::vector<sp::Message>& messages,
    const std::function<void(const sp::Event&)>& observer, int max_iterations,
    ToolExecutionContext execution) {
    return run_loop(messages, max_iterations, std::move(execution), ProviderMode::Stream, observer);
}

sp::runtime::Result Agent::run_loop(std::vector<sp::Message>& messages, int max_iterations,
    ToolExecutionContext execution, ProviderMode mode,
    std::function<void(const sp::Event&)> observer) {
    if (max_iterations <= 0) throw std::runtime_error("Agent exceeded max iterations (" +
                                                     std::to_string(max_iterations) + ")");
    if (execution.effect_broker && (execution.identity.owner_scope.empty() ||
        execution.identity.root_run_id.empty() || execution.identity.thread_id.empty() ||
        execution.effect_grant.grant_id.empty() || execution.effect_grant.operation_id.empty()))
        throw std::invalid_argument("Agent Tool broker requires exact host run and grant identity");
    ensure_system_message(messages);
    if (!execution.controller) execution.controller = tool_execution_controller_;
    if (!execution.hook_runtime) execution.hook_runtime = hook_runtime_;
    bool dispatched_tools = false;
    {
        auto calls = pending_client_tool_calls(messages);
        if (!calls.empty()) {
            append_tool_results(messages, std::move(calls), tool_ptrs(), tool_gate_, execution);
            dispatched_tools = true;
        }
    }
    const auto tools = get_tool_definitions();
    for (int iteration = 0; iteration < max_iterations; ++iteration) {
        auto request = make_provider_request(*provider_, model_, messages, tools, {}, mode);
        request.on_event = observer;
        request.cancel_token = execution.cancel_token;
        request.options.deadline = execution.deadline;
        if (mode == ProviderMode::Stream && !dispatched_tools && tool_detection_timeout_seconds_ > 0) {
            const auto detection_deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(tool_detection_timeout_seconds_);
            if (!request.options.deadline || detection_deadline < *request.options.deadline)
                request.options.deadline = detection_deadline;
        }
        sp::runtime::Result result;
        try {
            result = runtime_interposition_ ? runtime_interposition_->invoke(std::move(request))
                                          : provider_->invoke(std::move(request));
        } catch (const ProviderObserverError& error) {
            usage_->add(outcome_usage(*error.outcome()));
            outcomes_.push_back(error.outcome());
            const auto& returned = outcome_messages(*error.outcome());
            messages.insert(messages.end(), returned.begin(), returned.end());
            throw;
        }
        if (!result) throw std::runtime_error("Provider returned no outcome");
        usage_->add(outcome_usage(*result));
        const auto& returned = outcome_messages(*result);
        messages.insert(messages.end(), returned.begin(), returned.end());
        outcomes_.push_back(result);
        outcome_or_throw(result); // Failure retains full partial messages and typed error.
        auto calls = pending_client_tool_calls(returned);
        if (calls.empty()) return result;
        append_tool_results(messages, std::move(calls), tool_ptrs(), tool_gate_, execution);
        dispatched_tools = true;
    }
    throw std::runtime_error("Agent exceeded max iterations (" + std::to_string(max_iterations) + ")");
}

} // namespace neograph::llm
