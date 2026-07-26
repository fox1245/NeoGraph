#include <neograph/a2a/a2a_caller_node.h>

#include <neograph/graph/state.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace neograph::a2a {

namespace {
std::string fresh_caller_message_id() {
    static std::atomic<std::uint64_t> counter{0};
    char buf[40];
    std::snprintf(buf, sizeof(buf), "ng-a2a-call-%016llx",
                  static_cast<unsigned long long>(
                      counter.fetch_add(1, std::memory_order_relaxed)));
    return buf;
}
}

using neograph::graph::ChannelWrite;
using neograph::graph::GraphState;
using neograph::graph::NodeInput;
using neograph::graph::NodeOutput;

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

asio::awaitable<NodeOutput> A2ACallerNode::run(NodeInput in) {
    auto raw = in.state.get(input_key_);
    std::string prompt;
    if (raw.is_string()) {
        prompt = raw.get<std::string>();
    } else if (!raw.is_null()) {
        prompt = raw.dump();
    }

    auto task_id_val    = in.state.get(output_key_ + "_task_id");
    auto context_id_val = in.state.get(output_key_ + "_context_id");

    MessageSendParams params;
    params.message.message_id = fresh_caller_message_id();
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

    NodeOutput out;
    out.writes.push_back({output_key_, response_text});
    if (!task.id.empty())
        out.writes.push_back({output_key_ + "_task_id", task.id});
    if (!task.context_id.empty())
        out.writes.push_back({output_key_ + "_context_id", task.context_id});
    co_return out;
}

}  // namespace neograph::a2a
