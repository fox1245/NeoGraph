// NeoGraph Example 35: a reverse-engineering agent over MCP
//
// GhidraMCP puts a Ghidra project behind MCP tools: list the functions, decompile one, rename
// it, comment it. This example starts that server as a child process over stdio, gives its tools
// to a ReAct graph, and lets the agent recover names for the functions of a stripped binary.
// The agent's job is described by kSystemPrompt in re_agent_common.h: list, skip the compiler
// scaffolding, decompile each user function, rename it, comment it, and finish with a JSON
// summary.
//
// Offline by default. examples/ghidra_stand_in.py is a standard-library stand-in for the bridge
// that keeps a three-function "binary" in memory, and a scripted model plays the analyst: it
// picks each name by reading the decompiled code it is shown. Afterwards the program lists its
// functions again, so the renames are visible in the server's state, not only in the answer.
//
// Usage:
//   ./example_re_agent --bridge examples/ghidra_stand_in.py       # offline
//   GHIDRA_MCP_BRIDGE=/path/to/bridge_mcp_ghidra.py OPENROUTER_API_KEY=... ./example_re_agent --live
//       (a real model and a real Ghidra project with the GhidraMCP backend; billed;
//        GHIDRA_MCP_PYTHON picks the interpreter, GHIDRA_SERVER_URL the backend)
//
// smoke-args: --bridge @SRC@/examples/ghidra_stand_in.py
// smoke-env: GHIDRA_MCP_PYTHON=@PYTHON@
// smoke-expect: "original":"FUN_00101189","renamed":"xor_decrypt"
// smoke-expect: "original":"FUN_001011d4","renamed":"compute_checksum"
// smoke-expect: "original":"FUN_00101230","renamed":"validate_arguments"
// smoke-expect: program now: xor_decrypt @ 00101189
// smoke-forbid: [neograph] lint

#include "re_agent_common.h"
#include "scripted_provider.h"

#include <neograph/neograph.h>
#include <neograph/graph/react_graph.h>

#include <deque>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>

namespace {

// The scripted analyst: a small state machine over the tool results it is shown.
struct Analyst {
    std::string last_tool;                                         // the call it made last turn
    std::string current;                                           // function being decompiled
    std::map<std::string, std::string> address;                    // name -> address, from list_methods
    std::deque<std::pair<std::string, neograph::json>> queue;      // calls decided, not yet made
    neograph::json recovered = neograph::json::array();

    examples::Reply next(const examples::ScriptedTurn& turn) {
        if (last_tool.empty()) return call("list_methods", {{"offset", 0}, {"limit", 100}});
        const std::string seen = turn.last_tool_result();
        if (last_tool == "list_methods") {
            std::istringstream lines(seen);
            for (std::string line; std::getline(lines, line);) {
                const auto at = line.find(" @ ");
                if (at == std::string::npos) continue;
                const auto name = line.substr(0, at);
                address[name] = line.substr(at + 3);
                // Skip compiler scaffolding, as the system prompt says; keep Ghidra's FUN_ names.
                if (name.rfind("FUN_", 0) == 0) queue.push_back({"decompile_function", {{"name", name}}});
            }
        } else if (last_tool == "decompile_function") {
            decide(seen);
        }
        if (queue.empty()) return examples::say(neograph::json{{"recovered", recovered}}.dump());
        auto [tool, arguments] = queue.front();
        queue.pop_front();
        if (tool == "decompile_function") current = arguments["name"].get<std::string>();
        return call(tool, arguments);
    }

private:
    examples::Reply call(const std::string& tool, neograph::json arguments) {
        last_tool = tool;
        return examples::call_tool(tool, std::move(arguments));
    }
    // "Read" the pseudo-C and choose a name; then rename the function and comment it.
    void decide(const std::string& code) {
        std::string name, summary;
        neograph::json tags;
        if (code.find("^ 0x5a") != std::string::npos) {
            name = "xor_decrypt"; summary = "Decrypts a buffer in place with a one-byte XOR key"; tags = {"crypto", "xor"};
        } else if (code.find("* 33") != std::string::npos) {
            name = "compute_checksum"; summary = "Hashes a buffer with a multiplicative checksum"; tags = {"hash"};
        } else if (code.find("strlen(argv[1])") != std::string::npos) {
            name = "validate_arguments"; summary = "Checks that exactly one 16-character argument was given"; tags = {"cli"};
        } else {
            return;  // no real logic: leave it alone
        }
        queue.push_front({"set_decompiler_comment", {{"address", address[current]}, {"comment", summary}}});
        queue.push_front({"rename_function", {{"old_name", current}, {"new_name", name}}});
        recovered.push_back({{"original", current}, {"renamed", name}, {"summary", summary}, {"tags", tags}});
    }
};

}  // namespace

int main(int argc, char** argv) {
    int max_steps = 80;
    std::string bridge_script;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--max-steps" && i + 1 < argc) max_steps = std::atoi(argv[++i]);
        else if (arg == "--bridge" && i + 1 < argc) bridge_script = argv[++i];
    }

    try {
        // 1. Start the bridge over stdio and discover its tools.
        auto bridge = neograph::re_agent::spawn_ghidra_bridge(bridge_script);
        if (bridge.tools.empty()) {
            std::cerr << "The GhidraMCP server listed no tools.\n";
            return 2;
        }
        // 2. A listed tool does not prove the backend is up: some releases list every tool while
        //    Ghidra is down. One cheap call must succeed before the model loop starts.
        if (const auto problem = neograph::re_agent::check_ghidra_backend(bridge); !problem.empty()) {
            std::cerr << "The GhidraMCP backend did not answer: " << problem << "\n";
            return 2;
        }

        // 3. The scripted analyst, or a real model with --live.
        auto analyst = std::make_shared<Analyst>();
        auto scripted = std::make_shared<examples::ScriptedProvider>(
            [analyst](const examples::ScriptedTurn& turn) { return analyst->next(turn); });
        auto provider = examples::choose_provider(argc, argv, scripted);

        // 4. ReAct graph with the GhidraMCP tools wired in.
        auto engine = neograph::graph::create_react_graph(
            provider, std::move(bridge.tools), neograph::re_agent::kSystemPrompt, examples::openrouter_model);
        neograph::graph::RunConfig run;
        run.provider_messages = std::vector<sp::Message>{examples::message(sp::Role::User,
            "Recover names and summaries for every user-defined function in the open program. "
            "Output the final JSON.")};
        run.max_steps = max_steps;
        const auto result = engine->run(run);

        std::cout << neograph::re_agent::extract_final_response(result) << "\n";

        // The renames really happened on the server: list its functions again.
        const auto listing = bridge.client->call_tool("list_methods", {{"offset", 0}, {"limit", 100}});
        std::istringstream lines(listing["content"][0]["text"].get<std::string>());
        for (std::string line; std::getline(lines, line);) std::cout << "program now: " << line << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
