#include <neograph/a2a/a2a_caller_node.h>

#include <neograph/graph/state.h>

#include <stdexcept>

namespace neograph::a2a {

using neograph::graph::ChannelWrite;
using neograph::graph::GraphState;

A2ACallerNode::A2ACallerNode(std::string name,
                             std::shared_ptr<A2AClient> client,
                             std::string input_key,
                             std::string output_key)
    : name_(std::move(name)),
      client_(std::move(client)),
      input_key_(std::move(input_key)),
      output_key_(std::move(output_key)) {
    if (!client_) {
        throw std::invalid_argument(
            "A2ACallerNode '" + name_ + "': client must not be null");
    }
}

asio::awaitable<std::vector<ChannelWrite>>
A2ACallerNode::execute_async(const GraphState& state) {
    auto raw = state.get(input_key_);
    std::string prompt;
    if (raw.is_string()) {
        prompt = raw.get<std::string>();
    } else if (!raw.is_null()) {
        prompt = raw.dump();
    }

    auto task_id_val    = state.get(output_key_ + "_task_id");
    auto context_id_val = state.get(output_key_ + "_context_id");

    MessageSendParams params;
    params.message.message_id = name_ + "-" + std::to_string(
        reinterpret_cast<std::uintptr_t>(this) & 0xFFFF);
    params.message.role = Role::User;
    params.message.parts.push_back(Part::text_part(std::move(prompt)));
    if (task_id_val.is_string())    params.message.task_id    = task_id_val.get<std::string>();
    if (context_id_val.is_string()) params.message.context_id = context_id_val.get<std::string>();

    auto task = co_await client_->send_message_async(params);

    std::string response_text;
    if (!task.history.empty()) {
        for (auto& part : task.history.back().parts) {
            if (part.kind == "text") {
                if (!response_text.empty()) response_text.push_back('\n');
                response_text.append(part.text);
            }
        }
    }
    if (response_text.empty() && !task.artifacts.empty()) {
        for (auto& part : task.artifacts.front().parts) {
            if (part.kind == "text") {
                if (!response_text.empty()) response_text.push_back('\n');
                response_text.append(part.text);
            }
        }
    }

    std::vector<ChannelWrite> writes;
    writes.push_back({output_key_, response_text});
    if (!task.id.empty())
        writes.push_back({output_key_ + "_task_id", task.id});
    if (!task.context_id.empty())
        writes.push_back({output_key_ + "_context_id", task.context_id});
    co_return writes;
}

}  // namespace neograph::a2a
