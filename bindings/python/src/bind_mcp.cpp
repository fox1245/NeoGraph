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
// costs them their concurrency: a PyToolOwner is only concurrent if it holds a
// Python ng.AsyncTool, and an MCPTool is not one. Every MCP call would then
// serialize, and MCP would have arrived in Python without the property that
// makes it worth having (MCP tools are network round-trips — see #96).
//
// So wrap_python_tools now recognises a bound C++ Tool and passes it through
// behind a SharedToolRef, which keeps the Python-owned shared_ptr alive while
// the engine holds a raw Tool*. test_http_tool_calls_overlap is what stops this
// regressing: three 0.4 s MCP calls in ~0.4 s, not 1.2 s.

#include "json_bridge.h"

#include <neograph/mcp/client.h>
#include <neograph/tool.h>

#include <pybind11/pybind11.h>
#include <pybind11/chrono.h>
#include <pybind11/stl.h>

#include <memory>
#include <string>
#include <vector>

namespace py = pybind11;

namespace neograph::pybind {

void init_mcp(py::module_& m) {
    auto mcp = m.def_submodule("mcp",
        "MCP (Model Context Protocol) client — connect to a remote tool server, "
        "discover its catalogue, and hand the tools straight to a graph node.");

    py::register_exception<neograph::mcp::MCPError>(mcp, "MCPError",
                                                     PyExc_RuntimeError);

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
        "It is a C++ AsyncTool — it genuinely suspends — so several MCP calls in "
        "one turn overlap rather than queueing. HTTP uses concurrent requests; "
        "stdio multiplexes request IDs over its single pipe when the server can "
        "process requests concurrently.")
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
        "  MCPClient(\"http://localhost:8000\")        # HTTP\n"
        "  MCPClient([\"python\", \"server.py\"])        # stdio, spawns a subprocess\n\n"
        "The subprocess is terminated when the last reference to the session is "
        "dropped — which is the client, or any tool it produced.")
        .def(py::init<const std::string&>(), py::arg("server_url"),
            "HTTP transport. Tool calls made through this server overlap.")
        .def(py::init<const std::string&, neograph::mcp::MCPClientConfig>(),
             py::arg("server_url"), py::arg("config"))
        .def(py::init<std::vector<std::string>>(), py::arg("argv"),
            "stdio transport: spawn a subprocess and speak JSON-RPC over its "
            "pipes. argv[0] is resolved via PATH.\n\n"
            "Requests are correlated by JSON-RPC id, so calls overlap when the "
            "subprocess handles requests concurrently.")
        .def("initialize", &neograph::mcp::MCPClient::initialize,
            py::arg("client_name") = "neograph",
            py::call_guard<py::gil_scoped_release>(),
            "Perform the MCP handshake once. Repeated calls are idempotent; "
            "protocol and transport failures raise.")
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
            "is fine.")
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
               py::object arguments) {
                json args = arguments.is_none() ? json::object()
                                                : py_to_json(arguments);
                neograph::mcp::CallToolResult result;
                {
                    py::gil_scoped_release release;
                    result = self.call_tool_result(name, args);
                }
                return result;
            },
            py::arg("name"), py::arg("arguments") = py::none(),
            "Call a tool and preserve all MCP result fields. Tool-level "
            "isError remains data; JSON-RPC failures raise MCPError.");
}

}  // namespace neograph::pybind
