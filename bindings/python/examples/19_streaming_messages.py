"""19 — `message_stream` adapter for LangChain-shape token chunks.

The engine emits `LLM_TOKEN` events whenever a streaming node forwards
a token through its callback. `message_stream` wraps a graph callback
so each LLM_TOKEN becomes a dict shaped like LangChain's
`stream_mode="messages"` output — handy when porting a frontend that
already speaks that shape.

This example uses a node that emits a fixed token sequence so it runs
fully offline. In a real graph you'd plug a streaming LLM provider
into a node that forwards tokens to `cb(LLM_TOKEN_event)`.

Run:
    pip install neograph-engine
    python 19_streaming_messages.py
"""

import neograph_engine as ng
from neograph_engine import message_stream


class TokenEmitter(ng.GraphNode):
    """Stand-in for a streaming LLM node — emits a canned sequence."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        for token in ["Octo", "puses ", "have ", "three ", "hearts."]:
            ev = ng.GraphEvent()
            ev.type = ng.GraphEvent.Type.LLM_TOKEN
            ev.node_name = self._name
            ev.data = token
            input.stream_cb(ev)
        return ng.NodeResult(writes=[
            ng.ChannelWrite("messages", [{
                "role": "assistant",
                "content": "Octopuses have three hearts.",
            }])
        ])


ng.NodeFactory.register_type(
    "token_emitter",
    lambda name, config, ctx: TokenEmitter(name))

definition = {
    "name": "streaming_demo",
    "channels": {"messages": {"reducer": "append"}},
    "nodes":    {"emit":     {"type": "token_emitter"}},
    "edges": [
        {"from": ng.START_NODE, "to": "emit"},
        {"from": "emit",        "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
engine.set_checkpoint_store(ng.InMemoryCheckpointStore())


# Print each chunk's delta token in-place — what a frontend handler
# would do to render the message live.
def on_message(msg):
    print(msg["content"], end="", flush=True)

cb = message_stream(on_message)

cfg = ng.RunConfig(
    thread_id="t1",
    input={},
    max_steps=5,
    stream_mode=ng.StreamMode.TOKENS,
)
print("Live stream:")
print("  ", end="")
engine.run_stream(cfg, cb)
print()  # newline after the streamed content


# Same callback, but capture each chunk into a list so we can show
# the full LangChain-shape dict.
print("\nCaptured chunks (full shape):")
chunks = []
engine.run_stream(
    ng.RunConfig(thread_id="t2", input={}, max_steps=5,
                 stream_mode=ng.StreamMode.TOKENS),
    message_stream(chunks.append))
for c in chunks:
    print(f"  {c}")
