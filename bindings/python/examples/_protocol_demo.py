"""Shared offline graph for the A2A and ACP hosting examples."""

import neograph_engine as ng


class EchoNode(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        messages = input.state.get("messages") or []
        content = messages[-1].get("content", "") if messages else ""
        if isinstance(content, list):
            text = " ".join(
                block.get("text") or f"[{block.get('type', 'content')}]"
                for block in content
                if isinstance(block, dict)
            )
        else:
            text = str(content)
        turn = sum(message.get("role") == "user" for message in messages)
        answer = f"NeoGraph received: {text} (turn {turn})"
        if input.stream_cb:
            for token in ("NeoGraph ", "received: ", text, f" (turn {turn})"):
                event = ng.GraphEvent()
                event.type = ng.GraphEvent.Type.LLM_TOKEN
                event.node_name = self._name
                event.data = token
                input.stream_cb(event)
        return [
            ng.ChannelWrite(
                "messages",
                [{"role": "assistant", "content": answer}],
            )
        ]


def make_adapter(checkpoint_store=None, input_builder=None):
    type_name = "protocol_demo_echo"
    ng.NodeFactory.register_type(
        type_name, lambda name, config, ctx: EchoNode(name)
    )
    definition = {
        "name": "protocol_demo",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"echo": {"type": type_name}},
        "edges": [
            {"from": ng.START_NODE, "to": "echo"},
            {"from": "echo", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    engine.set_checkpoint_store(
        checkpoint_store or ng.InMemoryCheckpointStore()
    )
    return ng.ProtocolHostAdapter(
        engine, input_builder=input_builder, stream_node="echo"
    )
