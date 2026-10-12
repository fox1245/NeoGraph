// MCP (Model Context Protocol) client, bound to Python — issue #95.
//
// The C++ client already existed, tested, handling the parts that are actually
// fiddly: fd hygiene across fork, SSE frame parsing, the protocol-version
// header, the 16 MB line cap. Python had none of it, and the reason was never a
// design decision — "Python users can just reach for fastmcp" was an assumption
// nobody revisited. So this is plumbing, not research.
//
// THE ONE THING THAT IS NOT PLUMBING
//
// MCPClient::get_tools() hands back C++ Tools (MCPTool, which is an AsyncTool
// and really does suspend). They must reach the engine AS C++ tools.
//
// The obvious route — drop them into NodeContext(tools=[...]) alongside Python
// tools — would send them through wrap_python_tools, which wraps every entry in
// a PyToolOwner. That compiles, runs, and passes a functional test. It also
// costs them their native asynchronous I/O. A PyToolOwner is only concurrent
// if it holds a Python ng.AsyncTool, and an MCPTool is not one. Preserving the
// C++ type means the host can deliberately opt a server-safe tool into
// re-entrant execution through ToolExecutionPolicyRegistry. The engine remains
// conservative by default: repeated calls to one tool name serialize until the
// host declares otherwise.

// wrap_python_tools retains the native C++ Tool inside a SharedToolRef in
// the engine's ToolSet, preserving its asynchronous dispatch and keeping
// the Python-owned shared_ptr alive through every run.

#include "json_bridge.h"

#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include <neograph/mcp/client.h>
#include <neograph/tool.h>

#include <pybind11/pybind11.h>
#include <pybind11/chrono.h>
#include <pybind11/stl.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace py = pybind11;

namespace neograph::pybind {

void init_mcp(py::module_& m) {
    auto mcp = m.def_submodule("mcp",
        "MCP (Model Context Protocol) client — connect to a remote tool server, "
        "discover its catalogue, and hand the tools straight to a graph node.");

    auto mcp_error = py::register_exception<neograph::mcp::MCPError>(mcp, "MCPError",
                                                                      PyExc_RuntimeError);
    auto transport_error = py::register_exception<neograph::mcp::MCPTransportError>(
        mcp, "MCPTransportError", PyExc_RuntimeError);
    // Registered after the plain translators, so pybind11 tries it first: the
    // Python exceptions keep their typed fields, as A2ARpcError and
    // HookBoundaryBlocked do. MCPError: the JSON-RPC error's `code`, `data`
    // and the `http_status` that carried it (0 when none). MCPTransportError:
    // `failure` ("connection", "timeout", "cancelled", "shutdown",
    // "http_status", "protocol") and `http_status`.
    py::register_exception_translator([](std::exception_ptr pointer) {
        if (!pointer) return;
        try {
            std::rethrow_exception(pointer);
        } catch (const neograph::mcp::MCPError& error) {
            try {
                py::object type = py::module_::import("neograph_engine._neograph")
                                      .attr("mcp").attr("MCPError");
                py::object instance = type(utf8_to_py(error.what()));
                instance.attr("code") = py::int_(error.code());
                instance.attr("data") = json_to_py(error.data());
                instance.attr("http_status") = py::int_(error.http_status());
                PyErr_SetObject(type.ptr(), instance.ptr());
            } catch (py::error_already_set& failure) {
                failure.restore();
            }
        } catch (const neograph::mcp::MCPTransportError& error) {
            try {
                py::object type = py::module_::import("neograph_engine._neograph")
                                      .attr("mcp").attr("MCPTransportError");
                py::object instance = type(utf8_to_py(error.what()));
                const char* failure = "protocol";
                switch (error.failure()) {
                case neograph::mcp::MCPFailure::connection: failure = "connection"; break;
                case neograph::mcp::MCPFailure::timeout: failure = "timeout"; break;
                case neograph::mcp::MCPFailure::cancelled: failure = "cancelled"; break;
                case neograph::mcp::MCPFailure::shutdown: failure = "shutdown"; break;
                case neograph::mcp::MCPFailure::http_status: failure = "http_status"; break;
                case neograph::mcp::MCPFailure::protocol: failure = "protocol"; break;
                }
                instance.attr("failure") = py::str(failure);
                instance.attr("http_status") = py::int_(error.http_status());
                PyErr_SetObject(type.ptr(), instance.ptr());
            } catch (py::error_already_set& failure) {
                failure.restore();
            }
        }
    });
    (void)mcp_error;
    (void)transport_error;

    py::class_<neograph::mcp::StdioClientConfig>(mcp, "StdioClientConfig",
        "Hardened stdio launch: an explicit argv, working directory and "
        "environment, bounded frames and stderr capture, startup and request "
        "deadlines, and process-tree ownership. Without replace_environment the "
        "server inherits this process's environment and `environment` entries "
        "add to or override it; with it, they are the whole environment.")
        .def(py::init<>())
        .def_readwrite("argv", &neograph::mcp::StdioClientConfig::argv)
        .def_property("cwd",
            [](const neograph::mcp::StdioClientConfig& self) { return self.cwd.string(); },
            [](neograph::mcp::StdioClientConfig& self, const std::string& value) {
                self.cwd = std::filesystem::path(value);
            })
        .def_readwrite("environment", &neograph::mcp::StdioClientConfig::environment)
        .def_readwrite("replace_environment",
                       &neograph::mcp::StdioClientConfig::replace_environment)
        .def_readwrite("max_frame_bytes", &neograph::mcp::StdioClientConfig::max_frame_bytes)
        .def_readwrite("max_stderr_bytes", &neograph::mcp::StdioClientConfig::max_stderr_bytes)
        .def_readwrite("startup_timeout", &neograph::mcp::StdioClientConfig::startup_timeout)
        .def_readwrite("request_timeout", &neograph::mcp::StdioClientConfig::request_timeout)
        .def_readwrite("cancel_grace", &neograph::mcp::StdioClientConfig::cancel_grace);

    py::class_<neograph::mcp::MCPClientConfig>(mcp, "MCPClientConfig")
        .def(py::init<>())
        .def_readwrite("request_timeout",
                       &neograph::mcp::MCPClientConfig::request_timeout)
        .def_readwrite("headers", &neograph::mcp::MCPClientConfig::headers,
            "Static HTTP headers as (name, value) pairs. Use this for bearer "
            "tokens that do not need per-request refresh.");

    py::class_<neograph::mcp::InitializeResult>(mcp, "InitializeResult")
        .def_readonly("protocol_version",
                      &neograph::mcp::InitializeResult::protocol_version)
        .def_readonly("instructions",
                      &neograph::mcp::InitializeResult::instructions)
        .def_property_readonly("capabilities", [](const neograph::mcp::InitializeResult& self) {
            return json_to_py(self.capabilities);
        })
        .def_property_readonly("server_info", [](const neograph::mcp::InitializeResult& self) {
            return json_to_py(self.server_info);
        })
        .def_property_readonly("raw", [](const neograph::mcp::InitializeResult& self) {
            return json_to_py(self.raw);
        });

    py::class_<neograph::mcp::ToolDefinition>(mcp, "ToolDefinition")
        .def_readonly("name", &neograph::mcp::ToolDefinition::name)
        .def_readonly("title", &neograph::mcp::ToolDefinition::title)
        .def_readonly("description", &neograph::mcp::ToolDefinition::description)
        .def_property_readonly("icons", [](const neograph::mcp::ToolDefinition& self) {
            return json_to_py(self.icons);
        })
        .def_property_readonly("input_schema", [](const neograph::mcp::ToolDefinition& self) {
            return json_to_py(self.input_schema);
        })
        .def_property_readonly("output_schema", [](const neograph::mcp::ToolDefinition& self) {
            return json_to_py(self.output_schema);
        })
        .def_property_readonly("annotations", [](const neograph::mcp::ToolDefinition& self) {
            return json_to_py(self.annotations);
        })
        .def_property_readonly("execution", [](const neograph::mcp::ToolDefinition& self) {
            return json_to_py(self.execution);
        })
        .def_property_readonly("meta", [](const neograph::mcp::ToolDefinition& self) {
            return json_to_py(self.meta);
        })
        .def_property_readonly("raw", [](const neograph::mcp::ToolDefinition& self) {
            return json_to_py(self.raw);
        });

    py::class_<neograph::mcp::ListToolsPage>(mcp, "ListToolsPage")
        .def_readonly("tools", &neograph::mcp::ListToolsPage::tools)
        .def_readonly("next_cursor", &neograph::mcp::ListToolsPage::next_cursor)
        .def_property_readonly("meta", [](const neograph::mcp::ListToolsPage& self) {
            return json_to_py(self.meta);
        })
        .def_property_readonly("raw", [](const neograph::mcp::ListToolsPage& self) {
            return json_to_py(self.raw);
        });

    py::class_<neograph::mcp::CallToolResult>(mcp, "CallToolResult")
        .def_readonly("is_error", &neograph::mcp::CallToolResult::is_error)
        .def_property_readonly("content", [](const neograph::mcp::CallToolResult& self) {
            return json_to_py(self.content);
        })
        .def_property_readonly("structured_content", [](const neograph::mcp::CallToolResult& self) {
            return json_to_py(self.structured_content);
        })
        .def_property_readonly("meta", [](const neograph::mcp::CallToolResult& self) {
            return json_to_py(self.meta);
        })
        .def_property_readonly("raw", [](const neograph::mcp::CallToolResult& self) {
            return json_to_py(self.raw);
        });

    // MCPTool is a neograph::Tool. Bind it with the C++ Tool as its base so it
    // satisfies isinstance checks and can go into NodeContext(tools=[...])
    // without being mistaken for a Python tool.
    py::class_<neograph::mcp::MCPTool, neograph::Tool,
               std::shared_ptr<neograph::mcp::MCPTool>>(mcp, "MCPTool",
        "A tool living on an MCP server. Produced by MCPClient.get_tools(); you "
        "do not construct these yourself.\n\n"
        "It is a C++ AsyncTool — it genuinely suspends. Graph dispatch keeps "
        "repeated calls to one tool name conservative by default; after the "
        "host marks a server-safe tool Reentrant through "
        "ToolExecutionPolicyRegistry, HTTP requests and stdio JSON-RPC IDs can "
        "overlap when the server supports concurrency.")
        .def("get_name", &neograph::mcp::MCPTool::get_name)
        .def("get_definition", &neograph::mcp::MCPTool::get_definition,
            "The ChatTool the model sees: name, description, parameter schema.")
        .def("get_mcp_definition",
             &neograph::mcp::MCPTool::get_mcp_definition,
             py::return_value_policy::reference_internal,
             "The complete MCP tool definition, including output schema and metadata.")
        .def("execute_result",
             [](neograph::mcp::MCPTool& self, py::object arguments) {
                 json args = arguments.is_none() ? json::object()
                                                 : py_to_json(arguments);
                 neograph::mcp::CallToolResult result;
                 {
                     py::gil_scoped_release release;
                     result = self.execute_result(args);
                 }
                 return result;
             },
             py::arg("arguments") = py::none(),
             "Execute directly and preserve structured, non-text, error, and metadata fields.");

    py::class_<neograph::mcp::MCPClient>(mcp, "MCPClient",
        "Client for an MCP server.\n\n"
        "Two transports:\n\n"
        "  MCPClient(\"http://localhost:8000/mcp\")    # HTTP: the endpoint URL, as given\n"
        "  MCPClient([\"python\", \"server.py\"])        # stdio, spawns a subprocess\n\n"
        "The subprocess is terminated when the last reference to the session is "
        "dropped — which is the client, or any tool it produced.")
        .def(py::init<const std::string&>(), py::arg("server_url"),
            "HTTP transport. `server_url` is the server's MCP endpoint URL, used as "
            "given (nothing is appended). Tool calls made through this server overlap.")
        .def(py::init<const std::string&, neograph::mcp::MCPClientConfig>(),
             py::arg("server_url"), py::arg("config"))
        .def(py::init<std::vector<std::string>>(), py::arg("argv"),
            "stdio transport: spawn a subprocess and speak JSON-RPC over its "
            "pipes. argv[0] is resolved via PATH.\n\n"
            "Requests are correlated by JSON-RPC id, so calls overlap when the "
            "subprocess handles requests concurrently.")
        .def(py::init<neograph::mcp::StdioClientConfig>(), py::arg("config"),
            "Hardened stdio transport (see StdioClientConfig).")
        .def("initialize",
            [](neograph::mcp::MCPClient& self, const std::string& client_name,
               std::optional<double> timeout,
               std::shared_ptr<neograph::graph::CancelToken> cancel_token) {
                py::gil_scoped_release release;
                if (!timeout && !cancel_token) return self.initialize(client_name);
                const auto deadline = timeout
                    ? std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(*timeout))
                    : std::chrono::steady_clock::time_point::max();
                return self.initialize(client_name, deadline, std::move(cancel_token));
            },
            py::arg("client_name") = "neograph", py::arg("timeout") = py::none(),
            py::arg("cancel_token") = py::none(),
            "Perform the MCP handshake once. Repeated calls are idempotent; "
            "protocol and transport failures raise. `timeout` (seconds) and "
            "`cancel_token` stop waiting for it (MCPTransportError); MCP never "
            "cancels initialize on the wire.")
        .def("shutdown", &neograph::mcp::MCPClient::shutdown,
            py::call_guard<py::gil_scoped_release>(),
            "Permanently close the client and every tool it produced: an HTTP "
            "session is ended with DELETE, a stdio server is shut down (stdin "
            "closed first).")
        .def("set_tool_list_changed_handler",
            [](neograph::mcp::MCPClient& self, py::object handler) {
                if (handler.is_none()) {
                    self.set_tool_list_changed_handler({});
                    return;
                }
                // The callable lives in C++ and may be released on any thread.
                auto owned = std::shared_ptr<py::object>(
                    new py::object(std::move(handler)), [](py::object* object) {
                        if (Py_IsInitialized()) {
                            py::gil_scoped_acquire gil;
                            delete object;
                        }
                    });
                self.set_tool_list_changed_handler([owned] {
                    if (!Py_IsInitialized()) return;
                    py::gil_scoped_acquire gil;
                    try {
                        (*owned)();
                    } catch (py::error_already_set& error) {
                        error.discard_as_unraisable("MCP tool list changed handler");
                    }
                });
            },
            py::arg("handler"),
            "Call `handler()` when the server sends notifications/tools/list_changed; "
            "read get_tools() again then. It runs on the client's transport thread: "
            "return promptly and do not call this client from it. None removes it.")
        .def("is_initialized", &neograph::mcp::MCPClient::is_initialized)
        .def("get_initialize_result",
             &neograph::mcp::MCPClient::get_initialize_result,
             "Return negotiated protocol, capabilities, server info, and instructions.")
        .def("list_tools", &neograph::mcp::MCPClient::list_tools,
             py::arg("cursor") = py::none(),
             py::call_guard<py::gil_scoped_release>(),
             "Fetch one tools/list page using an opaque cursor.")
        .def("get_tool_definitions",
             &neograph::mcp::MCPClient::get_tool_definitions,
             py::call_guard<py::gil_scoped_release>(),
             "Fetch every tools/list page and preserve complete MCP metadata.")
        .def("get_tools",
            [](neograph::mcp::MCPClient& self) {
                std::vector<std::unique_ptr<neograph::Tool>> tools;
                {
                    py::gil_scoped_release release;   // network / subprocess I/O
                    tools = self.get_tools();
                }
                // Hand ownership to Python as shared_ptrs. NodeContext keeps
                // them alive for the engine's lifetime via the keep_alive
                // chain, and compile() passes them through as C++ tools rather
                // than wrapping them (see the header comment).
                py::list out;
                for (auto& tool : tools) {
                    auto* mcp_tool =
                        dynamic_cast<neograph::mcp::MCPTool*>(tool.get());
                    if (!mcp_tool) continue;
                    tool.release();
                    out.append(py::cast(
                        std::shared_ptr<neograph::mcp::MCPTool>(mcp_tool)));
                }
                return out;
            },
            "Discover the server's tools. Pass the result straight to "
            "NodeContext(tools=[...]) — mixing them with your own Python tools "
            "is fine. Graph dispatch serializes one named tool by default; use "
            "ToolExecutionPolicyRegistry only when the server supports "
            "concurrent calls.")
        .def("call_tool",
            [](neograph::mcp::MCPClient& self, const std::string& name,
               py::object arguments) {
                json args = arguments.is_none() ? json::object()
                                                : py_to_json(arguments);
                json result;
                {
                    py::gil_scoped_release release;
                    result = self.call_tool(name, args);
                }
                return json_to_py(result);
            },
            py::arg("name"), py::arg("arguments") = py::none(),
            "Call a tool by name, outside any graph. Returns the server's raw "
            "response — the MCP content envelope, "
            "{\"content\": [{\"type\": \"text\", \"text\": ...}]}.")
        .def("call_tool_result",
            [](neograph::mcp::MCPClient& self, const std::string& name,
               py::object arguments, std::optional<double> timeout,
               std::shared_ptr<neograph::graph::CancelToken> cancel_token) {
                json args = arguments.is_none() ? json::object()
                                                : py_to_json(arguments);
                neograph::mcp::CallToolResult result;
                {
                    py::gil_scoped_release release;
                    if (!timeout && !cancel_token) {
                        result = self.call_tool_result(name, args);
                    } else {
                        const auto deadline = timeout
                            ? std::chrono::steady_clock::now() +
                                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double>(*timeout))
                            : std::chrono::steady_clock::time_point::max();
                        result = neograph::async::run_sync(self.call_tool_result_async(
                            name, args, deadline, std::move(cancel_token)));
                    }
                }
                return result;
            },
            py::arg("name"), py::arg("arguments") = py::none(),
            py::arg("timeout") = py::none(), py::arg("cancel_token") = py::none(),
            "Call a tool and preserve all MCP result fields. Tool-level "
            "isError remains data; JSON-RPC failures raise MCPError. `timeout` "
            "(seconds) and `cancel_token` bound the call: the client stops "
            "waiting, tells the server with notifications/cancelled and raises "
            "MCPTransportError with failure \"timeout\" or \"cancelled\".");
}

}  // namespace neograph::pybind
