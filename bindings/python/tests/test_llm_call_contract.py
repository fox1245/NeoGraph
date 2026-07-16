"""The built-in llm_call takes its prompt from NodeContext (issue #116)."""

import ast
from pathlib import Path

import pytest

import neograph_engine as ng


COOKBOOK_GRAPHS = [
    "examples/cookbook/byo-openai/hybrid.py",
    "examples/cookbook/ollama-provider/via_openai_compat.py",
    "examples/cookbook/ollama-provider/via_native_api.py",
]


class _ResolveGraphConstants(ast.NodeTransformer):
    def visit_Attribute(self, node):
        if (isinstance(node.value, ast.Name) and node.value.id == "ng"
                and node.attr in {"START_NODE", "END_NODE"}):
            return ast.copy_location(ast.Constant(f"__{node.attr.lower()}__"), node)
        return self.generic_visit(node)


def _cookbook_graph(path):
    tree = ast.parse(path.read_text(), filename=str(path))
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign):
            continue
        if any(isinstance(target, ast.Name) and target.id == "graph_def"
               for target in node.targets):
            value = _ResolveGraphConstants().visit(node.value)
            return ast.literal_eval(value)
    raise AssertionError(f"graph_def not found in {path}")


class _CapturingProvider(ng.Provider):
    def __init__(self):
        super().__init__()
        self.calls = []

    def complete(self, params):
        self.calls.append(params)
        completion = ng.ChatCompletion()
        completion.message = ng.ChatMessage("assistant", "captured")
        return completion

    def get_name(self):
        return "capturing"


def _definition(node):
    return {
        "name": "llm-call-contract",
        "schema_version": 1,
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"answer": node},
        "edges": [
            {"from": ng.START_NODE, "to": "answer"},
            {"from": "answer", "to": ng.END_NODE},
        ],
    }


def test_llm_call_uses_node_context_instructions():
    provider = _CapturingProvider()
    context = ng.NodeContext(
        provider=provider,
        model="context-model",
        instructions="Context system prompt",
    )
    engine = ng.GraphEngine.compile(
        _definition({"type": "llm_call"}), context)

    engine.run(ng.RunConfig(
        thread_id="llm-call-contract",
        input={"messages": [{"role": "user", "content": "ping"}]},
    ))

    assert len(provider.calls) == 1
    params = provider.calls[0]
    assert params.model == "context-model"
    assert [(message.role, message.content) for message in params.messages] == [
        ("system", "Context system prompt"),
        ("user", "ping"),
    ]


def test_strict_llm_call_rejects_per_node_system_prompt():
    context = ng.NodeContext(provider=_CapturingProvider())

    with pytest.raises(RuntimeError, match="unknown or unconsumed key 'config'"):
        ng.GraphEngine.compile(_definition({
            "type": "llm_call",
            "config": {"system": "ignored"},
        }), context)


@pytest.mark.parametrize("relative_path", COOKBOOK_GRAPHS)
def test_provider_cookbook_uses_strict_context_config(relative_path):
    root = Path(__file__).resolve().parents[3]
    definition = _cookbook_graph(root / relative_path)

    assert definition["schema_version"] == 1
    assert definition["nodes"] == {"answer": {"type": "llm_call"}}
