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
        text = messages[-1].get("content", "") if messages else ""
        return [
            ng.ChannelWrite(
                "messages",
                [{"role": "assistant", "content": f"NeoGraph received: {text}"}],
            )
        ]


def make_adapter():
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
    engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
    return ng.ProtocolHostAdapter(engine)
