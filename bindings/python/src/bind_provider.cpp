// Provider / Tool / completion-types bindings.
//
// Commit 1 surface (this file):
//   - neograph.Provider          — abstract holder, no Python subclass yet.
//   - neograph.Tool              — opaque holder. Trampoline ships in commit 2.
//   - neograph.CompletionParams  — plain data class, sync provider input.
//   - neograph.ChatMessage       — plain data class, conversation message.
//   - neograph.ToolCall          — plain data class, LLM-issued tool call.
//   - neograph.ChatTool          — plain data class, tool definition.
//   - neograph.ChatCompletion    — plain data class, sync provider output.
//   - neograph.OpenAIProvider    — concrete OpenAI-compatible provider.
//   - neograph.SchemaProvider    — concrete schema-driven multi-vendor.
//
// Async (`Provider.complete_async`) is intentionally not exposed here.
// Sync `complete()` is enough for commit 1 and the awaitable surface
// would require an asio<->asyncio bridge; deferred to a later commit.

#include "json_bridge.h"
#include "opaque_types.h"

#include <neograph/provider.h>
#include <neograph/tool.h>
#include <neograph/types.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>
#include <string>

#ifdef NEOGRAPH_PYBIND_HAS_LLM
#include <neograph/llm/openai_provider.h>
#include <neograph/llm/schema_provider.h>
#endif

namespace py = pybind11;

namespace neograph::pybind {

namespace {

// pybind11 trampoline so Python classes can subclass neograph.Provider
// and have their `complete()` / `get_name()` overrides called from
// graph nodes (LLMCallNode, etc.). The C++ engine sees a Provider*;
// its virtual dispatch resolves to PyProvider, which then bounces
// into the Python override under the GIL.
//
// Streaming fallback: if the Python subclass doesn't override
// `complete_stream`, we just call `complete()` and emit the whole
// content as a single chunk. That lets a non-streaming Python wrapper
// still work in graphs that requested stream mode.
class PyProvider : public neograph::Provider {
  public:
    using neograph::Provider::Provider;

    neograph::ChatCompletion
    complete(const neograph::CompletionParams& params) override {
        PYBIND11_OVERRIDE(neograph::ChatCompletion,
                          neograph::Provider, complete, params);
    }

    neograph::ChatCompletion
    complete_stream(const neograph::CompletionParams& params,
                    const neograph::StreamCallback& on_chunk) override {
        // If the Python subclass overrode this, dispatch. Otherwise
        // synthesise a one-chunk stream from `complete()` so callers
        // that asked for streaming still see the content via the
        // callback — no ABCMeta flailing required of the user.
        py::gil_scoped_acquire gil;
        py::function override =
            py::get_override(static_cast<const neograph::Provider*>(this),
                             "complete_stream");
        if (override) {
            // Python on_chunk: pass a callable that re-locks the GIL
            // before forwarding to the C++ callback so the user can
            // call `on_chunk(token)` cleanly.
            auto py_on_chunk = py::cpp_function([&on_chunk](const std::string& tok) {
                if (on_chunk) on_chunk(tok);
            });
            auto r = override(params, py_on_chunk);
            return r.cast<neograph::ChatCompletion>();
        }
        // Fallback: complete() + single chunk.
        auto result = complete(params);
        if (on_chunk && !result.message.content.empty()) {
            on_chunk(result.message.content);
        }
        return result;
    }

    std::string get_name() const override {
        PYBIND11_OVERRIDE_PURE(std::string,
                               neograph::Provider, get_name,);
    }
};

}  // namespace

void init_provider(py::module_& m) {
    // ── ToolCall ─────────────────────────────────────────────────────────
    py::class_<neograph::ToolCall>(m, "ToolCall",
        "A single tool invocation requested by the LLM.")
        .def(py::init<>())
        .def(py::init([](const std::string& id,
                         const std::string& name,
                         const std::string& arguments) {
            return neograph::ToolCall{id, name, arguments};
        }), py::arg("id") = "", py::arg("name") = "", py::arg("arguments") = "")
        .def_readwrite("id",        &neograph::ToolCall::id)
        .def_readwrite("name",      &neograph::ToolCall::name)
        .def_readwrite("arguments", &neograph::ToolCall::arguments)
        .def("__repr__", [](const neograph::ToolCall& tc) {
            return "<ToolCall id=" + tc.id + " name=" + tc.name + ">";
        });

    // ── ChatMessage ──────────────────────────────────────────────────────
    py::class_<neograph::ChatMessage>(m, "ChatMessage",
        "A message in the conversation history.")
        .def(py::init<>())
        .def(py::init([](const std::string& role,
                         const std::string& content,
                         std::vector<neograph::ToolCall> tool_calls,
                         const std::string& tool_call_id,
                         const std::string& tool_name,
                         std::vector<std::string> image_urls) {
            neograph::ChatMessage m;
            m.role = role;
            m.content = content;
            m.tool_calls = std::move(tool_calls);
            m.tool_call_id = tool_call_id;
            m.tool_name = tool_name;
            m.image_urls = std::move(image_urls);
            return m;
        }),
            py::arg("role") = "",
            py::arg("content") = "",
            py::arg("tool_calls") = std::vector<neograph::ToolCall>{},
            py::arg("tool_call_id") = "",
            py::arg("tool_name") = "",
            py::arg("image_urls") = std::vector<std::string>{})
        .def_readwrite("role",         &neograph::ChatMessage::role)
        .def_readwrite("content",      &neograph::ChatMessage::content)
        .def_readwrite("tool_calls",   &neograph::ChatMessage::tool_calls)
        .def_readwrite("tool_call_id", &neograph::ChatMessage::tool_call_id)
        .def_readwrite("tool_name",    &neograph::ChatMessage::tool_name)
        .def_readwrite("image_urls",   &neograph::ChatMessage::image_urls)
        .def("__repr__", [](const neograph::ChatMessage& m) {
            return "<ChatMessage role=" + m.role + " content=" + m.content + ">";
        });

    // ── ChatTool (tool *definition* sent to the LLM) ─────────────────────
    py::class_<neograph::ChatTool>(m, "ChatTool",
        "Tool definition metadata sent to the LLM.")
        .def(py::init<>())
        .def(py::init([](const std::string& name,
                         const std::string& description,
                         py::object parameters) {
            neograph::ChatTool t;
            t.name = name;
            t.description = description;
            t.parameters = py_to_json(parameters);
            return t;
        }),
            py::arg("name") = "",
            py::arg("description") = "",
            py::arg("parameters") = py::dict())
        .def_readwrite("name",        &neograph::ChatTool::name)
        .def_readwrite("description", &neograph::ChatTool::description)
        .def_property("parameters",
            [](const neograph::ChatTool& t) { return json_to_py(t.parameters); },
            [](neograph::ChatTool& t, py::object v) { t.parameters = py_to_json(v); });

    // ── CompletionParams ─────────────────────────────────────────────────
    py::class_<neograph::CompletionParams>(m, "CompletionParams",
        "Parameters for a sync LLM completion request.")
        .def(py::init<>())
        .def(py::init([](const std::string& model,
                         std::vector<neograph::ChatMessage> messages,
                         std::vector<neograph::ChatTool> tools,
                         float temperature, int max_tokens) {
            neograph::CompletionParams p;
            p.model = model;
            p.messages = std::move(messages);
            p.tools = std::move(tools);
            p.temperature = temperature;
            p.max_tokens = max_tokens;
            return p;
        }),
            py::arg("model") = "",
            py::arg("messages") = std::vector<neograph::ChatMessage>{},
            py::arg("tools") = std::vector<neograph::ChatTool>{},
            py::arg("temperature") = 0.7f,
            py::arg("max_tokens") = -1)
        .def_readwrite("model",       &neograph::CompletionParams::model)
        .def_readwrite("messages",    &neograph::CompletionParams::messages)
        .def_readwrite("tools",       &neograph::CompletionParams::tools)
        .def_readwrite("temperature", &neograph::CompletionParams::temperature)
        .def_readwrite("max_tokens",  &neograph::CompletionParams::max_tokens);

    // ── ChatCompletion + Usage ───────────────────────────────────────────
    py::class_<neograph::ChatCompletion> chat_completion(m, "ChatCompletion",
        "LLM completion response: message + token usage.");
    chat_completion
        .def(py::init<>())
        .def_readwrite("message", &neograph::ChatCompletion::message)
        .def_readwrite("usage",   &neograph::ChatCompletion::usage);

    py::class_<neograph::ChatCompletion::Usage>(chat_completion, "Usage",
        "Token usage statistics.")
        .def(py::init<>())
        .def_readwrite("prompt_tokens",     &neograph::ChatCompletion::Usage::prompt_tokens)
        .def_readwrite("completion_tokens", &neograph::ChatCompletion::Usage::completion_tokens)
        .def_readwrite("total_tokens",      &neograph::ChatCompletion::Usage::total_tokens);

    // ── Provider base — Python subclassable via trampoline (v0.2.3+) ─────
    //
    // Lets a Python user bring their own LLM client (the official
    // openai SDK, anthropic SDK, langchain wrapper, etc.) and plug it
    // into NeoGraph nodes by subclassing `Provider` and implementing
    // `complete(params)` + `get_name()`. Streaming defaults to a
    // no-op single-chunk fallback that just calls `complete()` —
    // override `complete_stream` if your underlying client supports
    // token streams.
    py::class_<neograph::Provider, PyProvider,
               std::shared_ptr<neograph::Provider>>(m, "Provider",
        "Abstract LLM provider. Subclass and override "
        "`complete(params: CompletionParams) -> ChatCompletion` and "
        "`get_name() -> str` to plug your own LLM client (openai SDK, "
        "anthropic SDK, langchain, etc.) into NeoGraph graphs. "
        "Construct a concrete subclass directly, e.g. "
        "neograph_engine.llm.OpenAIProvider, when you want NeoGraph's "
        "built-in async HTTP path instead.")
        .def(py::init<>())
        .def("complete", [](neograph::Provider& self,
                            const neograph::CompletionParams& p) {
            // Release the GIL while the provider does network I/O so
            // other Python threads aren't blocked. The Provider impls
            // do no Python callbacks of their own.
            py::gil_scoped_release release;
            return self.complete(p);
        }, py::arg("params"))
        // Convenience overload — accept a bare list of message dicts (or
        // ChatMessage objects). Skips one CompletionParams + N ChatMessage
        // round-trips through pybind on every call. Measured against the
        // typed-CompletionParams path on a 5-parallel burst, this saves
        // ~250-300ms of Python-side marshalling per burst (the gap
        // between NG-via-pybind and the C++-direct probe).
        .def("complete", [](neograph::Provider& self,
                            const py::sequence& messages,
                            const std::string& model,
                            double temperature,
                            int max_tokens,
                            const py::sequence& tools) {
            neograph::CompletionParams p;
            p.model       = model;
            p.temperature = temperature;
            p.max_tokens  = max_tokens;
            p.messages.reserve(py::len(messages));
            for (const auto& item : messages) {
                if (py::isinstance<neograph::ChatMessage>(item)) {
                    p.messages.push_back(item.cast<neograph::ChatMessage>());
                    continue;
                }
                // Treat anything else as a {"role": ..., "content": ...}
                // mapping — same shape OpenAI / Anthropic / LangChain use.
                auto d = item.cast<py::dict>();
                neograph::ChatMessage m;
                if (d.contains("role"))    m.role    = d["role"].cast<std::string>();
                if (d.contains("content")) m.content = d["content"].cast<std::string>();
                if (d.contains("tool_call_id"))
                    m.tool_call_id = d["tool_call_id"].cast<std::string>();
                if (d.contains("tool_name"))
                    m.tool_name = d["tool_name"].cast<std::string>();
                p.messages.push_back(std::move(m));
            }
            // Tool definitions — accept ChatTool objects or
            // {name, description, parameters} dicts.
            p.tools.reserve(py::len(tools));
            for (const auto& item : tools) {
                if (py::isinstance<neograph::ChatTool>(item)) {
                    p.tools.push_back(item.cast<neograph::ChatTool>());
                    continue;
                }
                auto d = item.cast<py::dict>();
                neograph::ChatTool t;
                if (d.contains("name"))
                    t.name = d["name"].cast<std::string>();
                if (d.contains("description"))
                    t.description = d["description"].cast<std::string>();
                if (d.contains("parameters"))
                    t.parameters = py_to_json(d["parameters"]);
                p.tools.push_back(std::move(t));
            }
            py::gil_scoped_release release;
            return self.complete(p);
        },
            py::arg("messages"),
            py::arg("model")       = "",
            py::arg("temperature") = 0.7,
            py::arg("max_tokens")  = -1,
            py::arg("tools")       = py::list{})
        .def("get_name", &neograph::Provider::get_name);

    // ── Tool base (opaque, no Python subclass yet) ───────────────────────
    py::class_<neograph::Tool, std::shared_ptr<neograph::Tool>>(m, "Tool",
        "Abstract callable tool. Python subclassing arrives in commit 2 "
        "via a pybind11 trampoline. For now, treat as opaque — only "
        "C++-side tools (e.g. neograph::mcp::MCPTool) can populate "
        "NodeContext.tools.");

#ifdef NEOGRAPH_PYBIND_HAS_LLM
    // ── OpenAIProvider ───────────────────────────────────────────────────
    py::class_<neograph::llm::OpenAIProvider, neograph::Provider,
               std::shared_ptr<neograph::llm::OpenAIProvider>>(m, "OpenAIProvider",
        "OpenAI-compatible HTTP provider. Works with OpenAI, Groq, "
        "Together, vLLM, Ollama — anything serving the /v1/chat/completions "
        "shape.")
        .def(py::init([](const std::string& api_key,
                         const std::string& base_url,
                         const std::string& default_model,
                         int timeout_seconds) {
            neograph::llm::OpenAIProvider::Config cfg;
            cfg.api_key = api_key;
            cfg.base_url = base_url;
            cfg.default_model = default_model;
            cfg.timeout_seconds = timeout_seconds;
            // create() returns unique_ptr; convert to shared_ptr so the
            // pybind11 holder type matches.
            return std::shared_ptr<neograph::llm::OpenAIProvider>(
                neograph::llm::OpenAIProvider::create(cfg).release());
        }),
            py::arg("api_key") = "",
            py::arg("base_url") = "https://api.openai.com",
            py::arg("default_model") = "gpt-4o-mini",
            py::arg("timeout_seconds") = 60);

    // ── SchemaProvider ───────────────────────────────────────────────────
    py::class_<neograph::llm::SchemaProvider, neograph::Provider,
               std::shared_ptr<neograph::llm::SchemaProvider>>(m, "SchemaProvider",
        "Schema-driven multi-vendor provider. Built-in schemas: "
        "\"openai\", \"openai-responses\", \"claude\", \"gemini\". "
        "Pass a file path to use a custom schema.")
        .def(py::init([](const std::string& schema_path,
                         const std::string& api_key,
                         const std::string& default_model,
                         int timeout_seconds,
                         const std::string& base_url_override,
                         bool use_websocket,
                         bool prefer_libcurl) {
            neograph::llm::SchemaProvider::Config cfg;
            cfg.schema_path = schema_path;
            cfg.api_key = api_key;
            cfg.default_model = default_model;
            cfg.timeout_seconds = timeout_seconds;
            cfg.base_url_override = base_url_override;
            cfg.use_websocket = use_websocket;
            cfg.prefer_libcurl = prefer_libcurl;
            return std::shared_ptr<neograph::llm::SchemaProvider>(
                neograph::llm::SchemaProvider::create(cfg).release());
        }),
            py::arg("schema_path"),
            py::arg("api_key") = "",
            py::arg("default_model") = "gpt-4o-mini",
            py::arg("timeout_seconds") = 60,
            py::arg("base_url_override") = "",
            py::arg("use_websocket") = false,
            py::arg("prefer_libcurl") = false);
#endif // NEOGRAPH_PYBIND_HAS_LLM
}

} // namespace neograph::pybind
