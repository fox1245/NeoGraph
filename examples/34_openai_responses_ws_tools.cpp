// NeoGraph Example 34: OpenRouter Responses-compatible built-in tools tour
// over WebSocket.
//
// Walks every tool type the OpenRouter Responses API exposes — custom function,
// web_search, image_generation, file_search, tool_search, skills,
// shell — by sending one `response.create` per section over a fresh
// `wss://openrouter.ai/api/v1/responses` connection and printing what
// comes back.
//
// Talks to neograph::async::ws_connect directly (no SchemaProvider)
// because the SchemaProvider tool serialization is custom-function
// only — it doesn't know about `{"type":"web_search"}` etc. Going
// raw also keeps each request body verbatim in the source so you can
// see exactly what each tool's wire shape looks like.
//
// Sections that need extra setup are gated on env vars and skip
// gracefully when missing:
//   - file_search:     OPENROUTER_VECTOR_STORE_ID (a pre-built store)
//   - skills (custom): OPENROUTER_SKILL_ID         (override the curated default)
//
// Usage:
//   echo 'OPENROUTER_API_KEY=sk-or-...' > .env
//   ./example_openai_responses_ws_tools
//
// Each section:
//   - opens a fresh WS connection (clean event stream)
//   - sends one response.create with the tool slot populated
//   - drains events until response.completed (or error / close)
//   - prints a one-line summary + any tool-specific output

#include <neograph/async/http_client.h>
#include <neograph/json.h>

#include <neograph/async/run_sync.h>

#include <asio/this_coro.hpp>

#include <cppdotenv/dotenv.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ws = neograph::async;
using neograph::json;

namespace tools_demo {

// One section's per-event accounting — populated by the recv loop,
// printed at the end. Generic enough to absorb any of the seven
// tool flavours.
struct Summary {
    int            events            = 0;
    int            text_delta_count  = 0;
    std::string    assembled_text;

    // Custom function call (type="function_call" output items).
    std::string    fn_call_name;
    std::string    fn_call_args;

    // image_generation.partial_image / response.image_generation.* —
    // populated when an image gets generated. We only count and log
    // the size; rendering bytes is out of scope.
    int            image_chunks      = 0;
    std::size_t    image_total_bytes = 0;

    // web_search and tool_search emit dedicated *_call output items;
    // we track that they showed up and their counts.
    int            web_search_calls  = 0;
    int            tool_search_calls = 0;
    int            file_search_calls = 0;
    int            shell_calls       = 0;

    // server-side error event payload, if any.
    std::string    server_error;

    // close-frame metadata if the server killed the socket early.
    bool           closed_early      = false;
    int            close_code        = 0;
    std::string    close_reason;
};

void render(const std::string& section, const Summary& s) {
    std::cout << "  events=" << s.events;
    if (!s.assembled_text.empty()) {
        // Trim long assistant text to keep the demo output readable.
        std::string preview = s.assembled_text.size() > 240
            ? s.assembled_text.substr(0, 240) + "..."
            : s.assembled_text;
        std::cout << "  text=\"" << preview << "\"";
    }
    if (!s.fn_call_name.empty()) {
        std::cout << "  fn=" << s.fn_call_name << "(" << s.fn_call_args << ")";
    }
    if (s.image_chunks > 0) {
        std::cout << "  image_chunks=" << s.image_chunks
                  << " bytes=" << s.image_total_bytes;
    }
    if (s.web_search_calls)  std::cout << "  web_search_calls="  << s.web_search_calls;
    if (s.tool_search_calls) std::cout << "  tool_search_calls=" << s.tool_search_calls;
    if (s.file_search_calls) std::cout << "  file_search_calls=" << s.file_search_calls;
    if (s.shell_calls)       std::cout << "  shell_calls="       << s.shell_calls;
    if (s.closed_early) {
        std::cout << "  CLOSED close_code=" << s.close_code
                  << " reason=\"" << s.close_reason << "\"";
    }
    if (!s.server_error.empty()) {
        std::cout << "  ERROR=\"" << s.server_error << "\"";
    }
    std::cout << "\n";
    (void)section;
}

// Apply one parsed event to the running summary. Returns true when
// the stream should terminate (response.completed or error).
//
// noinline keeps the coroutine in run_section() small — GCC 13
// otherwise inlines this 60-line body into the coroutine frame
// codegen and trips an internal compiler error
// (build_special_member_call at cp/call.cc:11096).
[[gnu::noinline]]
bool consume_event(std::string_view payload, Summary& out) {
    out.events++;

    json j;
    try { j = json::parse(payload); }
    catch (const std::exception&) { return false; }

    std::string type = j.value("type", std::string{});
    if (type.empty()) return false;

    if (type == "response.output_text.delta") {
        out.text_delta_count++;
        out.assembled_text += j.value("delta", std::string{});
        return false;
    }
    if (type == "response.output_item.added") {
        if (j.contains("item") && j["item"].is_object()) {
            auto item = j["item"];
            std::string item_type = item.value("type", std::string{});
            if (item_type == "function_call") {
                out.fn_call_name = item.value("name", std::string{});
            }
            else if (item_type == "web_search_call")  out.web_search_calls++;
            else if (item_type == "tool_search_call") out.tool_search_calls++;
            else if (item_type == "file_search_call") out.file_search_calls++;
            else if (item_type == "shell_call")       out.shell_calls++;
        }
        return false;
    }
    if (type == "response.function_call_arguments.delta") {
        out.fn_call_args += j.value("delta", std::string{});
        return false;
    }
    if (type.rfind("response.image_generation", 0) == 0) {
        out.image_chunks++;
        std::string b64 = j.value("partial_image_b64", std::string{});
        if (b64.empty()) b64 = j.value("image_b64", std::string{});
        out.image_total_bytes += b64.size();
        return false;
    }
    if (type == "error") {
        std::string m = j.value("message", std::string{});
        if (m.empty() && j.contains("error") && j["error"].is_object())
            m = j["error"].value("message", std::string{});
        out.server_error = m;
        return true;
    }
    return type == "response.completed";
}

// GCC 13 has two coroutine codegen bugs that have to be dodged here
// (both trip an internal compiler error at cp/call.cc:11096,
// build_special_member_call):
//   (1) More than one non-trivially-destructible coroutine parameter
//       (e.g. two std::string by value) triggers it. std::string_view
//       is trivial, so passing refs via view sidesteps the bug.
//   (2) Naming the result of a co_await inside a loop body (`auto
//       msg = co_await wsc->recv(); ...`) triggers the destructor
//       codegen for the temp. Feeding the await straight into a call
//       expression avoids the named local.
// Bisected against GCC 13.3.0; the same patterns compile fine on
// clang 18 + libc++ but we follow the project's default toolchain.
asio::awaitable<void> run_section(
    asio::any_io_executor ex,
    std::string_view api_key,
    std::string_view body_json,
    Summary* out) {
    json request = json::parse(std::string{body_json});
    request["model"] = "~deepseek/deepseek-v4-flash-latest";
    request["stream"] = true;
    request["provider"] = {{"zdr", true}};

    std::string pending;
    bool done = false;
    auto process_event = [&](std::string_view event) {
        const auto data_pos = event.find("data:");
        if (data_pos == std::string_view::npos || done) return;
        std::string payload(event.substr(data_pos + 5));
        while (!payload.empty() &&
               (payload.front() == ' ' || payload.front() == '\t')) {
            payload.erase(payload.begin());
        }
        while (!payload.empty() &&
               (payload.back() == '\r' || payload.back() == '\n' ||
                payload.back() == ' ')) {
            payload.pop_back();
        }
        if (payload == "[DONE]") {
            done = true;
            return;
        }
        if (consume_event(payload, *out)) done = true;
    };

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", "Bearer " + std::string{api_key}},
        {"Content-Type", "application/json"},
    };
    ws::RequestOptions opts;
    opts.timeout = std::chrono::seconds(120);
    auto response = co_await ws::async_post_stream(
        ex, "openrouter.ai", "443", "/api/v1/responses",
        request.dump(), std::move(headers), /*tls=*/true,
        [&](std::string_view chunk) {
            pending.append(chunk.data(), chunk.size());
            std::size_t separator = 0;
            while ((separator = pending.find("\n\n")) != std::string::npos) {
                const std::string event = pending.substr(0, separator);
                pending.erase(0, separator + 2);
                process_event(event);
            }
        }, opts);
    if (!pending.empty()) process_event(pending);
    if (response.status != 200 && out->server_error.empty()) {
        out->server_error = "HTTP " + std::to_string(response.status);
    }
    co_return;
}

// Build a plain user-text input array — used by every section.
json user_input(const std::string& text) {
    return json::array({
        json{
            {"role", "user"},
            {"content", text},
        }
    });
}

}  // namespace tools_demo
using namespace tools_demo;

int main() {
    cppdotenv::auto_load_dotenv();
    const char* api_key = std::getenv("OPENROUTER_API_KEY");
    if (!api_key) {
        std::cerr << "Set OPENROUTER_API_KEY (or put it in .env)\n";
        return 1;
    }

    // Section table. Each entry produces a request body; sections that
    // need extra setup come with a skip predicate that prints the
    // reason rather than failing.
    struct Section {
        std::string name;
        std::function<json()> build;
        std::function<std::string()> skip_reason;  // empty string = run
    };

    std::vector<Section> sections;

    const std::string MODEL = "~deepseek/deepseek-v4-flash-latest";

    sections.push_back({
        "function (custom calculator)",
        [&]{
            json body;
            body["model"] = MODEL;
            body["input"] = user_input(
                "Use the calculator tool to compute 1234567 * 89. "
                "Do not compute it yourself.");
            body["instructions"] =
                "You MUST use the calculator tool for any arithmetic.";
            body["tools"] = json::array({
                json{
                    {"type", "function"},
                    {"name", "calculator"},
                    {"description",
                     "Evaluate a mathematical expression. "
                     "Required for any arithmetic."},
                    {"parameters", json{
                        {"type", "object"},
                        {"properties", json{
                            {"expression", json{{"type","string"}}}
                        }},
                        {"required", json::array({"expression"})},
                        {"additionalProperties", false},
                    }},
                }
            });
            return body;
        },
        []{ return std::string{}; },
    });

    sections.push_back({
        "web_search",
        [&]{
            json body;
            body["model"] = MODEL;
            body["input"] = user_input(
                "Search the web for the official release date of "
                "Linux kernel 6.0 and reply with just the date.");
            body["tools"] = json::array({ json{{"type", "web_search"}} });
            return body;
        },
        []{ return std::string{}; },
    });

    sections.push_back({
        "image_generation",
        [&]{
            json body;
            body["model"] = MODEL;
            body["input"] = user_input(
                "Generate a 256x256 image of a small cube. "
                "Reply 'done' once generated.");
            body["tools"] = json::array({
                json{
                    {"type", "image_generation"},
                    {"size",  "1024x1024"},
                    {"quality", "low"},
                }
            });
            return body;
        },
        []{ return std::string{}; },
    });

    sections.push_back({
        "file_search",
        [&]{
            const char* vs = std::getenv("OPENROUTER_VECTOR_STORE_ID");
            json body;
            body["model"] = MODEL;
            body["input"] = user_input(
                "Search the indexed files for the term 'NeoGraph' and "
                "summarize what it is in one sentence.");
            body["tools"] = json::array({
                json{
                    {"type", "file_search"},
                    {"vector_store_ids",
                     json::array({ std::string{vs ? vs : ""} })},
                }
            });
            return body;
        },
        []{
            return std::getenv("OPENROUTER_VECTOR_STORE_ID")
                ? std::string{}
                : std::string{"set OPENROUTER_VECTOR_STORE_ID to a pre-built store"};
        },
    });

    sections.push_back({
        "tool_search (OpenRouter DeepSeek)",
        [&]{
            json body;
            body["model"] = MODEL;
            body["input"] = user_input(
                "I need to know the current Unix epoch time. Use any "
                "available tool to get it.");
            // Two deferred function tools + one tool_search entry.
            // The model decides which to load. Per the docs,
            // defer_loading=true keeps tools out of the system prompt
            // until tool_search resolves them.
            body["tools"] = json::array({
                json{{"type", "tool_search"}},
                json{
                    {"type", "function"},
                    {"name", "get_unix_time"},
                    {"description", "Returns the current Unix epoch time."},
                    {"parameters", json{
                        {"type","object"},
                        {"properties", json::object()},
                        {"required", json::array()},
                        {"additionalProperties", false},
                    }},
                    {"defer_loading", true},
                },
                json{
                    {"type", "function"},
                    {"name", "get_weather"},
                    {"description", "Returns the current weather for a city."},
                    {"parameters", json{
                        {"type","object"},
                        {"properties", json{
                            {"city", json{{"type","string"}}}
                        }},
                        {"required", json::array({"city"})},
                        {"additionalProperties", false},
                    }},
                    {"defer_loading", true},
                },
            });
            return body;
        },
        []{ return std::string{}; },
    });

    sections.push_back({
        "skills (mounted in shell.environment)",
        [&]{
            const char* skill_id = std::getenv("OPENROUTER_SKILL_ID");
            json body;
            body["model"] = MODEL;
            body["input"] = user_input(
                "Use any available skill to summarize what skills are "
                "available to you. Reply in one sentence.");
            // Skills are not a top-level tool — they ride inside the
            // shell tool's environment. Default to OpenAI's curated
            // openai-spreadsheets skill_id; override via env.
            body["tools"] = json::array({
                json{
                    {"type", "shell"},
                    {"environment", json{
                        {"type", "container_auto"},
                        {"skills", json::array({
                            json{
                                {"type", "skill_reference"},
                                {"skill_id",
                                 std::string{skill_id ? skill_id
                                                      : "openai-spreadsheets"}},
                            }
                        })},
                    }},
                }
            });
            return body;
        },
        []{ return std::string{}; },
    });

    sections.push_back({
        "shell (container_auto)",
        [&]{
            json body;
            body["model"] = MODEL;
            body["input"] = user_input(
                "Use the shell tool to print the current working "
                "directory and the contents of /etc/os-release. "
                "Then reply with one sentence summarizing the OS.");
            body["tools"] = json::array({
                json{
                    {"type", "shell"},
                    {"environment", json{{"type", "container_auto"}}},
                }
            });
            return body;
        },
        []{ return std::string{}; },
    });

    // Drive each section through a fresh run_sync() rather than
    // chaining them inside one coroutine. GCC 13 trips internal
    // compiler errors on the co_await-of-a-coroutine pattern we'd
    // otherwise need; per-section run_sync sidesteps it entirely and
    // also gives each section a clean io_context (mirrors how users
    // would call this from non-coroutine code).
    int failed = 0;
    for (const auto& s : sections) {
        std::cout << "\n── " << s.name << " ──\n";
        if (auto reason = s.skip_reason(); !reason.empty()) {
            std::cout << "  SKIP — " << reason << "\n";
            continue;
        }
        try {
            auto t0 = std::chrono::steady_clock::now();
            Summary summary;
            std::string key_str = api_key;
            std::string body_str = s.build().dump();
            if (body_str.size() >= 2 && body_str.back() == '}') {
                body_str.pop_back();
                body_str += R"(,"type":"response.create"})";
            }
            // Pass string views into the coroutine; the backing
            // std::string locals (key_str, body_str) outlive run_sync
            // since it blocks.
            auto make_awaitable = [](std::string_view k,
                                     std::string_view b,
                                     Summary* out)
                -> asio::awaitable<void> {
                auto ex = co_await asio::this_coro::executor;
                co_await run_section(ex, k, b, out);
            };
            neograph::async::run_sync(
                make_awaitable(key_str, body_str, &summary));
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            std::cout << "  (" << ms << " ms)";
            render(s.name, summary);
            if (summary.closed_early || !summary.server_error.empty())
                failed++;
        } catch (const std::exception& e) {
            std::cout << "  EXCEPTION: " << e.what() << "\n";
            failed++;
        }
    }
    std::cout << "\n── " << (sections.size() - failed)
              << "/" << sections.size() << " sections succeeded ──\n";
    return failed == 0 ? 0 : 2;
}
