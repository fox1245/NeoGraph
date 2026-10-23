"""Shared offline graph for the A2A and ACP hosting examples.

It echoes the last message; a message that starts with `approve:` pauses it for an
answer first, so the examples can show how a graph interrupt reaches each protocol.
"""

import neograph_engine as ng


APPROVE_PREFIX = "approve:"
YES = ("approve", "approved", "yes", "y", "ok")


def _text_of(content):
    if isinstance(content, list):
        return " ".join(
            block.get("text") or f"[{block.get('type', 'content')}]"
            for block in content
            if isinstance(block, dict)
        )
    return str(content)


class EchoNode(ng.GraphNode):
    """Echoes the last user message, and asks before running an `approve:` command.

    A message that starts with `approve:` pauses the graph with a NodeInterrupt
    (the command is the payload). The answer arrives as `input.ctx.resume_value`:
    the option id of an ACP permission request ("approve" or "reject") or the text
    of an A2A follow-up message.
    """

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        messages = list(input.state.get("messages") or [])
        answer = input.ctx.resume_value
        if answer is not None and messages:
            messages.pop()  # the engine appends the answer as the last user message
        content = messages[-1].get("content", "") if messages else ""
        text = _text_of(content)
        turn = sum(message.get("role") == "user" for message in messages)
        words = ("NeoGraph ", "received: ", text, f" (turn {turn})")
        if text.startswith(APPROVE_PREFIX):
            command = text[len(APPROVE_PREFIX):].strip()
            if answer is None:
                raise ng.NodeInterrupt(
                    {"command": command}, reason=f"Run `{command}`?"
                )
            verb = "ran" if str(answer).strip().lower() in YES else "skipped"
            words = ("NeoGraph ", f"{verb}: ", command, f" (turn {turn})")
        if input.stream_cb:
            for token in words:
                event = ng.GraphEvent()
                event.type = ng.GraphEvent.Type.LLM_TOKEN
                event.node_name = self._name
                event.data = token
                input.stream_cb(event)
        return [
            ng.ChannelWrite(
                "messages",
                [{"role": "assistant", "content": "".join(words)}],
            )
        ]


def make_adapter(checkpoint_store=None, input_builder=None):
    type_name = "protocol_demo_echo"
    ng.NodeFactory.register_type(
        type_name, lambda name, config, ctx: EchoNode(name)
    )
    definition = {
        "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
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
