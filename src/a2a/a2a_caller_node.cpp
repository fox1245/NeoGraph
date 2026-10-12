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
                              std::string output_key,
                              MessageIdFactory message_id_factory)
    : name_(std::move(name)),
      client_(std::move(client)),
      input_key_(std::move(input_key)),
      output_key_(std::move(output_key)),
      message_id_factory_(std::move(message_id_factory)) {
    if (!client_) {
        throw std::invalid_argument(
            "A2ACallerNode '" + name_ + "': client must not be null");
    }
    if (!message_id_factory_) {
        message_id_factory_ = fresh_caller_message_id;
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
    params.message.message_id = message_id_factory_();
    if (params.message.message_id.empty()) {
        throw std::runtime_error(
            "A2ACallerNode '" + name_ + "': message ID factory returned empty ID");
    }
    params.message.role = Role::User;
    params.message.parts.push_back(Part::text_part(std::move(prompt)));
    // A2A continues a conversation through its contextId; a taskId continues
    // only a task that waits for input (spec 3.4.3). A terminal task accepts
    // no further messages, so its id is never sent (see the writes below).
    if (task_id_val.is_string() && !task_id_val.get<std::string>().empty())
        params.message.task_id = task_id_val.get<std::string>();
    if (context_id_val.is_string() && !context_id_val.get<std::string>().empty())
        params.message.context_id = context_id_val.get<std::string>();

    auto task = co_await client_->send_message_async(params);

    // The agent's answer, in order of authority: the terminal status
    // message, the artifacts' text, then the last *agent* message in the
    // history (which may also hold the caller's own turn or an interim
    // "working" note, so it is only the last resort).
    auto text_of = [](const std::vector<Part>& parts) {
        std::string text;
        for (auto& part : parts) {
            if (part.kind == "text") {
                if (!text.empty()) text.push_back('\n');
                text.append(part.text);
            }
        }
        return text;
    };
    std::string response_text;
    const auto state = task.status.state;
    const bool final_status = state == TaskState::Completed || state == TaskState::Canceled
        || state == TaskState::Failed || state == TaskState::Rejected
        || state == TaskState::InputRequired || state == TaskState::AuthRequired;
    if (final_status && task.status.message && task.status.message->role == Role::Agent) {
        response_text = text_of(task.status.message->parts);
    }
    if (response_text.empty() && !task.artifacts.empty()) {
        response_text = text_of(task.artifacts.front().parts);
    }
    for (auto it = task.history.rbegin();
         response_text.empty() && it != task.history.rend(); ++it) {
        if (it->role == Role::Agent) response_text = text_of(it->parts);
    }

    NodeOutput out;
    out.writes.push_back({output_key_, response_text});
    // Keep the task id only while the task waits for input, so the next call
    // resumes it; once the task is terminal the next call starts a new task
    // in the same context.
    const bool awaiting_input =
        state == TaskState::InputRequired || state == TaskState::AuthRequired;
    if (!task.id.empty())
        out.writes.push_back({output_key_ + "_task_id", awaiting_input ? task.id : std::string()});
    if (!task.context_id.empty())
        out.writes.push_back({output_key_ + "_context_id", task.context_id});
    co_return out;
}

}  // namespace neograph::a2a
