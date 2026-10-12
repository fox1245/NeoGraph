"""45 - A2A client: call an agent hosted by the official SDK with NeoGraph's own client.

The server is example 27, an agent hosted by the official A2A SDK. This script starts it in a
thread of its own process on a free port, then talks to it with `ng.a2a.A2AClient`, the native
client of the wheel: it fetches the agent card, sends a message and reads the finished task, and
sends a second message in the same conversation while watching the stream. Nothing is installed
beyond the SDK, and no model is involved: the agent's graph just echoes.

    pip install "neograph-engine[a2a]"
    python 45_a2a_client.py

smoke-requires: a2a, uvicorn
smoke-expect: agent: NeoGraph A2A Agent
smoke-expect: dialect: V1_0
smoke-expect: first answer: NeoGraph received: hello (turn 1)
smoke-expect: streamed chunks: ['NeoGraph ', 'received: ', 'again', ' (turn 2)']
smoke-expect: second answer: NeoGraph received: again (turn 2)
"""

import importlib.util
import os
import socket
import threading
import time
from pathlib import Path

import neograph_engine as ng


def free_port():
    """A port the OS hands out; nothing else is listening on it for now."""
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def start_example_27(port):
    """Run example 27's agent on `port` in a thread. Returns (server, thread)."""
    import uvicorn

    # Example 27 reads its port from the environment when it is loaded (the agent card names it).
    os.environ["NEOGRAPH_A2A_PORT"] = str(port)
    path = Path(__file__).with_name("27_a2a_server.py")
    spec = importlib.util.spec_from_file_location("a2a_server_example", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    server = uvicorn.Server(uvicorn.Config(module.app, host="127.0.0.1", port=port, log_level="error"))
    thread = threading.Thread(target=server.run, daemon=True)
    thread.start()
    deadline = time.monotonic() + 20
    while not server.started:
        if not thread.is_alive() or time.monotonic() > deadline:
            raise RuntimeError("the A2A server did not start")
        time.sleep(0.05)
    return server, thread


def text_of(task):
    """The agent's answer: the text of the task's artifacts, in order."""
    return "".join(part.text for artifact in task.artifacts for part in artifact.parts)


def main():
    port = free_port()
    server, thread = start_example_27(port)
    try:
        client = ng.a2a.A2AClient(f"http://127.0.0.1:{port}")

        card = client.fetch_agent_card()
        print("agent:", card.name)

        task = client.send_message("hello")
        print("dialect:", client.wire_dialect().name)
        print("first answer:", text_of(task))
        print("state:", task.state.name)

        # A second message in the same conversation: the stream delivers the answer in chunks.
        chunks = []

        def on_event(event):
            if event.artifact_update is not None:
                chunks.append(event.artifact_update.artifact.parts[0].text)
            return True  # keep reading

        second = client.send_message_stream("again", on_event, context_id=task.context_id)
        print("streamed chunks:", chunks)
        print("second answer:", text_of(second))
    finally:
        server.should_exit = True
        thread.join(timeout=10)


if __name__ == "__main__":
    main()
