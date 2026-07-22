#include <neograph/mcp/harness.h>
#include <neograph/mcp/json_schema.h>
#include <neograph/provider.h>

#include "harness_journal_internal.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
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
        std::map<std::string, json> catalog;
        for (const auto& tool : call.tool_catalog) {
            catalog[tool["id"].get<std::string>()] = tool;
        }

        CompletionParams params;
        params.model = config.model;
        params.temperature = 0.0f;
        params.cancel_token = cancel;
        params.tools = chat_tools(call.tool_catalog);
        ChatMessage initial;
        initial.role = "user";
        initial.content = worker_prompt(call);
        params.messages.push_back(std::move(initial));

        for (std::size_t round = 0; round < config.max_tool_rounds; ++round) {
            cancel->throw_if_cancelled("before provider Harness completion");
            ChatCompletion completion;
            const auto provider_correlation = detail::journal_correlation_id("provider");
            const auto provider_started = std::chrono::steady_clock::now();
            detail::append_current_harness_journal_event(
                "provider.call.started",
                {{"message_count", params.messages.size()},
                 {"model", params.model},
                 {"round", round + 1},
                 {"tool_count", params.tools.size()}},
                provider_correlation);
            try {
                completion = config.provider->complete(params);
            } catch (const graph::CancelledException&) {
                detail::append_current_harness_journal_event(
                    "provider.call.completed",
                    {{"duration_ms",
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - provider_started)
                          .count()},
                     {"outcome", "cancelled"}},
                    provider_correlation);
                return HarnessWorkerResponse::cancelled();
            } catch (const std::exception& error) {
                detail::append_current_harness_journal_event(
                    "provider.call.completed",
                    {{"duration_ms",
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - provider_started)
                          .count()},
                     {"error", error.what()},
                     {"outcome", "error"}},
                    provider_correlation);
                return HarnessWorkerResponse::tool_error(error.what());
            }
            detail::append_current_harness_journal_event(
                "provider.call.completed",
                {{"content", completion.message.content},
                 {"duration_ms",
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - provider_started)
                      .count()},
                 {"outcome", completion.message.tool_calls.empty() ? "content" : "tool_calls"},
                 {"tool_call_count", completion.message.tool_calls.size()},
                 {"usage",
                  {{"completion_tokens", completion.usage.completion_tokens},
                   {"prompt_tokens", completion.usage.prompt_tokens},
                   {"total_tokens", completion.usage.total_tokens}}}},
                provider_correlation);

            if (completion.message.tool_calls.empty()) {
                if (completion.message.content.empty()) {
                    return HarnessWorkerResponse::empty("provider returned empty content");
                }
                try {
                    return HarnessWorkerResponse::success(json::parse(completion.message.content));
                } catch (const std::exception& error) {
                    return HarnessWorkerResponse::parse_error(error.what());
                }
            }

            params.messages.push_back(completion.message);
            for (const auto& tool_call : completion.message.tool_calls) {
                auto tool_it = catalog.find(tool_call.name);
                if (tool_it == catalog.end()) {
                    return HarnessWorkerResponse::tool_error(
                        "provider requested undeclared tool: " + tool_call.name);
                }
                try {
                    auto arguments = json::parse(tool_call.arguments);
                    validate_json_value(arguments, tool_it->second["input_schema"],
                                        "Harness tool arguments", "$");
                    arguments           = enforce_path_policy(call, tool_it->second, arguments);
                    const auto executor = tool_it->second.value("executor", json::object());
                    if (executor.value("kind", "") == "host_brokered") {
                        const auto call_id = host_call_id();
                        json pending = {
                            {"call_id", call_id},
                            {"provider_call_id", tool_call.id},
                            {"tool_id", tool_call.name},
                            {"arguments", std::move(arguments)},
                            {"result_schema",
                             tool_it->second.value("output_schema", json::object())},
                        };
                        detail::append_current_harness_journal_event(
                            "host_brokered.call.requested",
                            {{"arguments", pending["arguments"]},
                             {"interaction", executor.value("interaction", "tool_result")},
                             {"provider_call_id", tool_call.id},
                             {"tool_id", tool_call.name}},
                            call_id);
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
                             {"outcome", "error"},
                             {"tool_id", tool_call.name}},
                            capability_correlation);
                        throw;
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
                    ChatMessage message;
                    message.role = "tool";
                    message.tool_call_id = tool_call.id;
                    message.tool_name = tool_call.name;
                    message.content = result.dump();
                    params.messages.push_back(std::move(message));
                } catch (const std::exception& error) {
                    return HarnessWorkerResponse::tool_error(error.what());
                }
            }
        }
        return HarnessWorkerResponse::tool_error(
            "provider exhausted max_tool_rounds without a final result");
    };
}

} // namespace neograph::mcp
