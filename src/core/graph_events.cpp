#include <neograph/graph/types.h>

namespace neograph::graph {

TypedGraphEvent to_typed_event(const GraphEvent& event) {
    auto raw = [&event]() -> TypedGraphEvent { return RawGraphEvent{event}; };

    try {
        if (event.type == GraphEvent::Type::CHANNEL_WRITE && event.node_name == "__state__") {
            return StateSnapshotEvent{event.data};
        }

        if (event.type == GraphEvent::Type::NODE_START && event.node_name == "__routing__") {
            if (!event.data.is_object()) return raw();

            RoutingEvent typed;
            typed.data = event.data;
            if (event.data.contains("command_goto")) {
                const auto value = event.data["command_goto"];
                if (!value.is_string()) return raw();
                typed.command_goto = value.get<std::string>();
            }
            if (event.data.contains("next_nodes")) {
                const auto values = event.data["next_nodes"];
                if (!values.is_array()) return raw();
                for (const auto& value : values) {
                    if (!value.is_string()) return raw();
                    typed.next_nodes.push_back(value.get<std::string>());
                }
            }
            if (event.data.contains("step")) {
                const auto value = event.data["step"];
                if (!value.is_number_integer()) return raw();
                typed.step = value.get<int>();
            }
            return typed;
        }

        if (event.type == GraphEvent::Type::NODE_START && event.node_name == "__send__") {
            if (!event.data.is_object() || !event.data.contains("sends") ||
                !event.data["sends"].is_array()) {
                return raw();
            }

            SendDispatchEvent typed;
            for (const auto& item : event.data["sends"]) {
                if (!item.is_object() || !item.contains("target") || !item["target"].is_string()) {
                    return raw();
                }
                Send send;
                send.target_node = item["target"].get<std::string>();
                if (item.contains("input")) send.input = item["input"];
                typed.sends.push_back(std::move(send));
            }
            return typed;
        }

        switch (event.type) {
            case GraphEvent::Type::NODE_START: {
                NodeStartEvent typed{event.node_name, std::nullopt, event.data};
                if (event.data.is_object() && event.data.contains("retry_attempt")) {
                    const auto value = event.data["retry_attempt"];
                    if (!value.is_number_integer()) return raw();
                    typed.retry_attempt = value.get<int>();
                }
                return typed;
            }
            case GraphEvent::Type::NODE_END: {
                NodeEndEvent typed;
                typed.node_name = event.node_name;
                typed.data      = event.data;
                if (event.data.is_object() && event.data.contains("command_goto")) {
                    const auto value = event.data["command_goto"];
                    if (!value.is_string()) return raw();
                    typed.command_goto = value.get<std::string>();
                }
                if (event.data.is_object() && event.data.contains("sends")) {
                    const auto value = event.data["sends"];
                    if (!value.is_number_integer() || value.get<int>() < 0) return raw();
                    typed.send_count = static_cast<std::size_t>(value.get<int>());
                }
                return typed;
            }
            case GraphEvent::Type::LLM_TOKEN:
                if (!event.data.is_string()) return raw();
                return LlmTokenEvent{event.node_name, event.data.get<std::string>()};
            case GraphEvent::Type::CHANNEL_WRITE: {
                if (!event.data.is_object() || !event.data.contains("channel") ||
                    !event.data["channel"].is_string() || !event.data.contains("value")) {
                    return raw();
                }
                return ChannelWriteEvent{event.node_name, event.data["channel"].get<std::string>(),
                                         event.data["value"]};
            }
            case GraphEvent::Type::INTERRUPT: {
                InterruptEvent typed;
                typed.node_name = event.node_name;
                typed.data      = event.data;
                if (event.data.is_object() && event.data.contains("phase")) {
                    const auto value = event.data["phase"];
                    if (!value.is_string()) return raw();
                    typed.phase = value.get<std::string>();
                }
                if (event.data.is_object() && event.data.contains("checkpoint_id")) {
                    const auto value = event.data["checkpoint_id"];
                    if (!value.is_string()) return raw();
                    typed.checkpoint_id = value.get<std::string>();
                }
                return typed;
            }
            case GraphEvent::Type::ERROR:
                if (!event.data.is_object() || !event.data.contains("error") ||
                    !event.data["error"].is_string()) {
                    return raw();
                }
                return ErrorEvent{event.node_name, event.data["error"].get<std::string>(),
                                  event.data};
        }
    } catch (const json::exception&) {
        return raw();
    }
    return raw();
}

}  // namespace neograph::graph
