/**
 * @file a2a/a2a_caller_node.h
 * @brief GraphNode that delegates work to a remote A2A agent.
 *
 * Mirrors the role MCPCallerNode plays for tool servers. On execute(),
 * the node:
 *   1. Reads `prompt` (or a configured key) from the GraphState.
 *   2. Calls `message/send` on the configured A2AClient.
 *   3. Writes the agent's response back into the configured output key.
 *
 * Send fan-out works transparently: many A2ACallerNode instances
 * sharing one A2AClient produce concurrent HTTP requests via the async
 * executor. Each call is independent — no shared state in the client.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/a2a/client.h>
#include <neograph/graph/node.h>

#include <functional>
#include <memory>
#include <string>

namespace neograph::a2a {

/**
 * @brief Node that forwards an input string to a remote A2A agent.
 *
 * Reads from `input_key` (default "prompt"), writes the agent's first
 * text response into `output_key` (default "response"). The context id is
 * written to "<output_key>_context_id" so the next call continues the
 * conversation; "<output_key>_task_id" holds the task id only while the
 * task waits for input (INPUT_REQUIRED / AUTH_REQUIRED), so the next call
 * resumes that task, and is empty once the task is terminal (A2A 3.4.3: a
 * terminal task accepts no further messages).
 */
class NEOGRAPH_API A2ACallerNode : public neograph::graph::GraphNode {
  public:
    /// Returns one message ID per outbound A2A call. Custom factories must be
    /// thread-safe when the node is used by concurrent graph branches.
    using MessageIdFactory = std::function<std::string()>;

    A2ACallerNode(std::string name,
                   std::shared_ptr<A2AClient> client,
                   std::string input_key  = "prompt",
                   std::string output_key = "response",
                   MessageIdFactory message_id_factory = {});

    /// v0.4 PR 9a: unified ``run`` — reads ``input_key`` from state,
    /// forwards to the remote A2A agent, writes the response (and
    /// task_id / context_id continuation handles) back. Cancel
    /// propagates through the underlying A2AClient transport.
    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override;

    std::string get_name() const override { return name_; }

  private:
    std::string                name_;
    std::shared_ptr<A2AClient> client_;
    std::string                input_key_;
    std::string                output_key_;
    MessageIdFactory           message_id_factory_;
};

} // namespace neograph::a2a
