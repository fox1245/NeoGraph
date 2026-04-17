// NeoGraph Example 08: State Management (get_state / update_state / fork)
//
// Demonstrates state management features corresponding to LangGraph's Checkpointer API.
//
// Scenario:
//   1. Run graph → interrupt → inspect state (get_state)
//   2. Modify state (update_state) — edit messages and resume
//   3. Fork — fork from an existing checkpoint to a new thread
//   4. Time travel — re-run from a past checkpoint
//
// No API key required (uses Mock Provider)
//
// Usage: ./example_state_management

#include <neograph/neograph.h>
#include <neograph/graph/react_graph.h>

#include <iostream>
#include <iomanip>

// Mock Provider: responds differently based on message content
class StateMockProvider : public neograph::Provider {
public:
    neograph::ChatCompletion complete(const neograph::CompletionParams& params) override {
        neograph::ChatCompletion result;
        result.message.role = "assistant";

        // Check the last user message
        std::string last_user;
        for (auto it = params.messages.rbegin(); it != params.messages.rend(); ++it) {
            if (it->role == "user") { last_user = it->content; break; }
        }

        if (last_user.find("Seoul") != std::string::npos) {
            result.message.content = "Seoul is the capital of South Korea, with a population of about 9.5 million.";
        } else if (last_user.find("Busan") != std::string::npos) {
            result.message.content = "Busan is the second largest city in South Korea, famous for Haeundae Beach.";
        } else if (last_user.find("Tokyo") != std::string::npos) {
            result.message.content = "Tokyo is the capital of Japan and one of the largest metropolitan areas in the world.";
        } else {
            result.message.content = "Hello! Please ask me about a city.";
        }
        return result;
    }

    neograph::ChatCompletion complete_stream(
        const neograph::CompletionParams& p, const neograph::StreamCallback& cb) override {
        auto r = complete(p);
        if (cb && !r.message.content.empty()) cb(r.message.content);
        return r;
    }
    std::string get_name() const override { return "state_mock"; }
};

static void print_separator(const std::string& title) {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << " " << title << "\n";
    std::cout << std::string(60, '=') << "\n\n";
}

static void print_messages(const neograph::json& state) {
    if (!state.contains("channels") || !state["channels"].contains("messages")) return;
    auto msgs = state["channels"]["messages"]["value"];
    if (!msgs.is_array()) return;

    for (const auto& m : msgs) {
        std::string role = m.value("role", "?");
        std::string content = m.value("content", "");
        if (!content.empty()) {
            std::cout << "  [" << role << "] " << content << "\n";
        }
    }
}

int main() {
    auto provider = std::make_shared<StateMockProvider>();

    // 2-node graph: llm → reviewer (interrupt before reviewer)
    // The user reviews the LLM response, then the reviewer does final confirmation
    neograph::json definition = {
        {"name", "state_demo"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {
            {"llm",      {{"type", "llm_call"}}},
            {"reviewer", {{"type", "llm_call"}}}
        }},
        {"edges", neograph::json::array({
            {{"from", "__start__"}, {"to", "llm"}},
            {{"from", "llm"},      {"to", "reviewer"}},
            {{"from", "reviewer"}, {"to", "__end__"}}
        })},
        {"interrupt_before", neograph::json::array({"reviewer"})}
    };

    neograph::graph::NodeContext ctx;
    ctx.provider = provider;

    auto store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    auto engine = neograph::graph::GraphEngine::compile(definition, ctx, store);

    // ================================================================
    // 1. Execute → interrupt → get_state
    // ================================================================
    print_separator("1. Inspect state after execution (get_state)");

    neograph::graph::RunConfig config;
    config.thread_id = "thread-001";
    config.input = {{"messages", neograph::json::array({
        {{"role", "user"}, {"content", "Tell me about Seoul"}}
    })}};

    auto result = engine->run(config);
    std::cout << "interrupted: " << std::boolalpha << result.interrupted << "\n\n";

    // Inspect current state via get_state
    auto state = engine->get_state("thread-001");
    if (state) {
        std::cout << "Current conversation:\n";
        print_messages(*state);
    }

    // ================================================================
    // 2. update_state — modify messages and resume
    // ================================================================
    print_separator("2. Modify state and resume (update_state)");

    std::cout << "Original question: \"Tell me about Seoul\"\n";
    std::cout << "Modified to:       \"Tell me about Busan\" (question changed)\n\n";

    // Add a new user message (reducer=append, so it appends to existing)
    engine->update_state("thread-001", {
        {"messages", neograph::json::array({
            {{"role", "user"}, {"content", "Tell me about Busan"}}
        })}
    });

    // Verify modified state
    auto updated_state = engine->get_state("thread-001");
    if (updated_state) {
        std::cout << "Modified conversation:\n";
        print_messages(*updated_state);
    }

    // Resume execution
    std::cout << "\nResuming...\n";
    auto resumed = engine->resume("thread-001");
    std::cout << "Execution trace: ";
    for (const auto& n : resumed.execution_trace) std::cout << n << " → ";
    std::cout << "END\n\n";

    auto final_state = engine->get_state("thread-001");
    if (final_state) {
        std::cout << "Final conversation:\n";
        print_messages(*final_state);
    }

    // ================================================================
    // 3. fork — branch execution
    // ================================================================
    print_separator("3. Fork");

    // Fork from thread-001's current state to a new thread
    auto fork_cp_id = engine->fork("thread-001", "thread-001-tokyo");
    std::cout << "Fork complete: thread-001 -> thread-001-tokyo\n";
    std::cout << "New checkpoint ID: " << fork_cp_id << "\n\n";

    // Modify the forked thread's state
    engine->update_state("thread-001-tokyo", {
        {"messages", neograph::json::array({
            {{"role", "user"}, {"content", "Tell me about Tokyo"}}
        })}
    });

    // Execute on the forked thread
    auto forked_result = engine->resume("thread-001-tokyo");
    std::cout << "Forked thread execution trace: ";
    for (const auto& n : forked_result.execution_trace) std::cout << n << " → ";
    std::cout << "END\n\n";

    auto fork_state = engine->get_state("thread-001-tokyo");
    if (fork_state) {
        std::cout << "Forked thread conversation:\n";
        print_messages(*fork_state);
    }

    // ================================================================
    // 4. State history (time travel)
    // ================================================================
    print_separator("4. State History (Time Travel)");

    auto history = engine->get_state_history("thread-001");
    std::cout << "thread-001 checkpoint history (" << history.size() << " entries):\n\n";
    for (size_t i = 0; i < history.size(); ++i) {
        const auto& cp = history[i];
        std::cout << "  #" << (i + 1) << " [" << to_string(cp.interrupt_phase) << "]"
                  << " step=" << cp.step
                  << " node=" << cp.current_node
                  << " → ";
        for (size_t j = 0; j < cp.next_nodes.size(); ++j) {
            if (j) std::cout << ",";
            std::cout << cp.next_nodes[j];
        }
        std::cout << " id=" << cp.id.substr(0, 8) << "...\n";
    }

    auto fork_history = engine->get_state_history("thread-001-tokyo");
    std::cout << "\nthread-001-tokyo checkpoint history (" << fork_history.size() << " entries):\n\n";
    for (size_t i = 0; i < fork_history.size(); ++i) {
        const auto& cp = fork_history[i];
        std::string meta_info;
        if (cp.metadata.contains("forked_from")) {
            meta_info = " (forked from " +
                cp.metadata["forked_from"].value("thread_id", "?") + ")";
        }
        std::cout << "  #" << (i + 1) << " [" << to_string(cp.interrupt_phase) << "]"
                  << " step=" << cp.step
                  << " node=" << cp.current_node
                  << meta_info << "\n";
    }

    return 0;
}
