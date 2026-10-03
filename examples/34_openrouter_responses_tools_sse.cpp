// NeoGraph Example 34: all seven OpenRouter Responses tool sections over SSE.
// Requests use only declared SDK controls, never raw HTTP/SSE or JSON overrides.
// Each section retains its full terminal Outcome and all ordered wire events,
// including unknown types. Function tools are advertised here, not executed.
// OPENROUTER_VECTOR_STORE_ID gates file_search; OPENROUTER_SKILL_ID overrides
// the curated openai-spreadsheets skill mounted in shell.environment.

#include "provider_example_support.h"

#include <cppdotenv/dotenv.hpp>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
sp::responses::ToolDefinition function_tool(
    std::string name, std::string description, std::string_view schema,
    bool deferred = false) {
    auto parsed = sp::json::parse(schema);
    if (const auto* error = std::get_if<sp::json::ParseError>(&parsed))
        throw std::invalid_argument(error->message);
    sp::responses::ToolDefinition tool;
    tool.name = std::move(name);
    tool.description = std::move(description);
    tool.parameters = std::make_shared<const sp::json::Document>(
        std::get<sp::json::Document>(std::move(parsed)));
    if (deferred) tool.defer_loading = true;
    return tool;
}

sp::responses::Request user_request(std::string prompt) {
    sp::responses::Request request;
    request.model = examples::openrouter_model;
    request.messages.push_back(examples::message(sp::Role::User, std::move(prompt)));
    return request;
}

void print_document(const std::shared_ptr<const sp::json::Document>& document) {
    std::cout << (document ? document->root().dump() : "null");
}

// Borrowed delta/snapshot views are displayed within the callback. RawWire's
// immutable document is separately retained; no semantic event is flattened
// into a fabricated text-only completion or used as native replay authority.
void print_event(const sp::Event& event) {
    std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, sp::RawWire>) {
            std::cout << "  RawWire type=" << value.type << " payload=";
            print_document(value.payload);
        } else if constexpr (std::is_same_v<T, sp::ResponseEnvelope>) {
            std::cout << "  ResponseEnvelope payload=";
            print_document(value.payload);
        } else if constexpr (std::is_same_v<T, sp::Begin>) {
            std::cout << "  Begin generation=" << value.generation;
        } else if constexpr (std::is_same_v<T, sp::MessageBegin>) {
            std::cout << "  MessageBegin id=" << value.message.value
                      << " vendor_id=" << value.vendor_id.value_or("")
                      << " role=" << static_cast<int>(value.role)
                      << " native_context=" << static_cast<bool>(value.native_context);
        } else if constexpr (std::is_same_v<T, sp::PartBegin>) {
            std::cout << "  PartBegin message=" << value.message.value
                      << " part=" << value.part.value << " order=" << value.order
                      << " kind=" << static_cast<int>(value.kind)
                      << " wire_type=" << value.header.wire_type
                      << " wire_id=" << value.header.wire_id
                      << " name=" << value.header.name
                      << " tool_kind=" << static_cast<int>(value.header.tool_kind)
                      << " metadata=";
            print_document(value.header.wire_metadata);
        } else if constexpr (std::is_same_v<T, sp::PartDelta>) {
            std::cout << "  PartDelta part=" << value.part.value
                      << " kind=" << static_cast<int>(value.payload.kind)
                      << " channel=" << static_cast<int>(value.payload.channel)
                      << " bytes=" << sp::json::quote(value.payload.bytes);
        } else if constexpr (std::is_same_v<T, sp::PartSeal>) {
            std::cout << "  PartSeal part=" << value.part.value
                      << " snapshot=" << (value.snapshot ? sp::json::quote(*value.snapshot) : "null")
                      << " metadata=";
            print_document(value.wire_metadata);
        } else if constexpr (std::is_same_v<T, sp::MessageSeal>) {
            std::cout << "  MessageSeal message=" << value.message.value << " output=";
            print_document(value.wire_output);
        } else if constexpr (std::is_same_v<T, sp::UsageUpdate>) {
            std::cout << "  UsageUpdate " << neograph::usage_to_json(value.snapshot).dump();
        } else if constexpr (std::is_same_v<T, sp::Stop>) {
            std::cout << "  Stop kind=" << static_cast<int>(value.reason.kind)
                      << " raw=" << value.reason.raw
                      << " sequence=" << value.reason.sequence.value_or("") << " details=";
            print_document(value.reason.details);
        } else if constexpr (std::is_same_v<T, sp::Commit>) {
            std::cout << "  Commit evidence=" << value.evidence;
        } else if constexpr (std::is_same_v<T, sp::Fail>) {
            std::cout << "  Fail kind=" << static_cast<int>(value.error.kind)
                      << " message=" << value.error.safe_message
                      << " HTTP=" << value.error.http_status
                      << " vendor_code=" << value.error.vendor_code
                      << " retry_class=" << static_cast<int>(value.error.retry_class)
                      << " retry_safety=" << static_cast<int>(value.error.retry_safety);
        }
        std::cout << "\n";
    }, event);
}

struct Section {
    std::string name;
    std::function<sp::responses::Request()> build;
    std::function<std::string()> skip_reason;
};
struct SectionResult {
    std::string name;
    std::vector<sp::RawWire> wire_events;
    std::shared_ptr<const sp::Outcome> outcome;
};
} // namespace

int main() {
    cppdotenv::auto_load_dotenv();
    try {
        const char* api_key = std::getenv("OPENROUTER_API_KEY");
        if (!api_key) {
            std::cerr << "Set OPENROUTER_API_KEY (env or .env)\n";
            return 1;
        }
        auto provider = examples::make_openrouter_provider(api_key, "responses");
        const auto no_skip = [] { return std::string{}; };
        const std::vector<Section> sections = {
            {"function (custom calculator)", [] {
                auto request = user_request(
                    "Use the calculator tool to compute 1234567 * 89. Do not compute it yourself.");
                request.instructions = "You MUST use the calculator tool for any arithmetic.";
                request.tools.push_back(function_tool("calculator",
                    "Evaluate a mathematical expression. Required for any arithmetic.",
                    R"({"type":"object","properties":{"expression":{"type":"string"}},"required":["expression"],"additionalProperties":false})"));
                return request;
            }, no_skip},
            {"web_search", [] {
                auto request = user_request(
                    "Search the web for the official release date of Linux kernel 6.0 and reply with just the date.");
                request.hosted_tools.emplace_back(sp::responses::WebSearchTool{});
                return request;
            }, no_skip},
            {"image_generation", [] {
                auto request = user_request(
                    "Generate a 256x256 image of a small cube. Reply 'done' once generated.");
                sp::responses::ImageGenerationTool tool;
                tool.size = "1024x1024";
                tool.quality = "low";
                request.hosted_tools.emplace_back(std::move(tool));
                return request;
            }, no_skip},
            {"file_search", [] {
                auto request = user_request(
                    "Search the indexed files for the term 'NeoGraph' and summarize what it is in one sentence.");
                sp::responses::FileSearchTool tool;
                const char* store = std::getenv("OPENROUTER_VECTOR_STORE_ID");
                tool.vector_store_ids.emplace_back(store ? store : "");
                request.hosted_tools.emplace_back(std::move(tool));
                return request;
            }, [] {
                return std::getenv("OPENROUTER_VECTOR_STORE_ID") ? std::string{}
                    : std::string{"set OPENROUTER_VECTOR_STORE_ID to a pre-built store"};
            }},
            {"tool_search (OpenRouter DeepSeek)", [] {
                auto request = user_request(
                    "I need to know the current Unix epoch time. Use any available tool to get it.");
                request.hosted_tools.emplace_back(sp::responses::ToolSearchTool{});
                request.tools.push_back(function_tool("get_unix_time",
                    "Returns the current Unix epoch time.",
                    R"({"type":"object","properties":{},"required":[],"additionalProperties":false})", true));
                request.tools.push_back(function_tool("get_weather",
                    "Returns the current weather for a city.",
                    R"({"type":"object","properties":{"city":{"type":"string"}},"required":["city"],"additionalProperties":false})", true));
                return request;
            }, no_skip},
            {"skills (mounted in shell.environment)", [] {
                auto request = user_request(
                    "Use any available skill to summarize what skills are available to you. Reply in one sentence.");
                const char* skill = std::getenv("OPENROUTER_SKILL_ID");
                sp::responses::ShellTool shell;
                shell.environment.skills.push_back(sp::responses::SkillReference{
                    skill ? skill : "openai-spreadsheets"});
                request.hosted_tools.emplace_back(std::move(shell));
                return request;
            }, no_skip},
            {"shell (container_auto)", [] {
                auto request = user_request(
                    "Use the shell tool to print the current working directory and the contents of /etc/os-release. "
                    "Then reply with one sentence summarizing the OS.");
                request.hosted_tools.emplace_back(sp::responses::ShellTool{});
                return request;
            }, no_skip}
        };
        std::vector<SectionResult> results;
        results.reserve(sections.size());
        std::size_t succeeded = 0, skipped = 0, failed = 0;
        for (const auto& section : sections) {
            std::cout << "\n-- " << section.name << " --\n";
            if (const auto reason = section.skip_reason(); !reason.empty()) {
                std::cout << "  SKIP: " << reason << "\n";
                ++skipped;
                continue;
            }
            results.push_back({section.name, {}, {}});
            auto& result = results.back();
            try {
                neograph::ProviderRequest request;
                request.payload = section.build();
                request.mode = neograph::ProviderMode::Stream;
                request.options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
                request.on_event = [&](const sp::Event& event) {
                    if (const auto* wire = std::get_if<sp::RawWire>(&event))
                        result.wire_events.push_back(*wire);
                    print_event(event);
                };
                const auto start = std::chrono::steady_clock::now();
                result.outcome = provider->invoke(std::move(request));
                if (!result.outcome) throw std::runtime_error("Provider returned no owned Outcome");
                const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);
                std::cout << "  wall=" << wall.count() << " ms wire_events=" << result.wire_events.size() << "\n"
                          << "--- Full terminal Outcome: ordered outputs/args/citations/artifacts ---\n"
                          << neograph::outcome_projection_json(*result.outcome).dump(2) << "\n";
                if (const auto* failure = std::get_if<sp::Failure>(result.outcome.get())) {
                    std::cout << "  ERROR: " << failure->error.safe_message << "\n";
                    ++failed;
                } else ++succeeded;
            } catch (const std::exception& error) {
                std::cout << "  EXCEPTION: " << error.what() << "\n";
                ++failed;
            }
        }
        std::cout << "\n-- " << succeeded << " succeeded, " << skipped << " skipped, "
                  << failed << " failed / " << sections.size() << " sections --\n";
        return failed == 0 ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
