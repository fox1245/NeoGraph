"""Consumer lifetime/error boundaries that previously risked process failure."""

import subprocess
import sys
import textwrap

import pytest
import neograph_engine as ng


def _run_isolated(source):
    result = subprocess.run(
        [sys.executable, "-c", textwrap.dedent(source)],
        capture_output=True, text=True, timeout=15,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_retained_parts_and_config_messages_survive_vector_replacement():
    _run_isolated("""
        import gc
        import neograph_engine as ng

        message = ng.ProviderMessage(ng.ProviderRole.User, [
            ng.Text("retained text"), ng.Thinking("retained thought", "local signature"),
        ])
        text, thought = message.parts
        message.parts = []
        del message
        gc.collect()
        assert text.value == "retained text"
        assert thought.text == "retained thought"
        assert thought.signature == "local signature"
        text.value = "independently edited text"
        thought.text = "independently edited thought"
        assert text.value == "independently edited text"
        assert thought.text == "independently edited thought"

        config = ng.RunConfig()
        config.provider_messages = [
            ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("retained config message")]),
        ]
        retained = config.provider_messages[0]
        config.provider_messages = [
            ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("replacement config message")]),
        ]
        gc.collect()
        assert retained.parts[0].value == "retained config message"
        retained.role = ng.ProviderRole.Assistant
        retained.parts = [ng.Text("edited retained value")]
        assert config.provider_messages[0].role == ng.ProviderRole.User
        assert config.provider_messages[0].parts[0].value == "replacement config message"
        del config
        gc.collect()
        assert retained.parts[0].value == "edited retained value"
    """)


def test_none_preparation_is_type_error_not_process_failure():
    _run_isolated("""
        import neograph_engine as ng

        class ForgotReturn(ng.Provider):
            def __init__(self):
                super().__init__("openai.chat")

            def get_name(self):
                return "forgot-prepare-return"

            def prepare(self, request):
                return None

        provider = ForgotReturn()
        request = ng.make_provider_request(provider, "local-model", [
            ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("must not dispatch")]),
        ])
        try:
            provider.invoke(request)
        except TypeError:
            pass
        else:
            raise AssertionError("None preparation was accepted")
    """)


def test_active_program_host_teardown_allows_python_worker_to_finish():
    _run_isolated("""
        import gc
        import threading
        import time
        import neograph_engine as ng

        entered = threading.Event()
        finished = threading.Event()

        class PendingNode(ng.GraphNode):
            def get_name(self):
                return "work"

            def run(self, input):
                entered.set()
                time.sleep(0.2)
                finished.set()
                return [ng.ChannelWrite("value", 42)]

        ng.NodeFactory.register_type("teardown_pending_node", lambda _n, _c, _ctx: PendingNode())
        builder = ng.ProgramRegistryBuilder()
        builder.add_registered_node("teardown_pending_node", "1.0.0", "sha256:" + "1" * 64)
        builder.add_registered_reducer("overwrite", "1.0.0", "sha256:" + "2" * 64)
        budget = ng.ProgramRunBudget()
        budget.wall_time_ms = 10000
        budget.model_tokens = 1000
        budget.monetary_microunits = 1000
        budget.max_concurrency = 2
        budget.max_program_operations = 32
        budget.max_core_steps = 20
        budget.max_dynamic_compiles = 1
        budget.max_child_depth = 1
        budget.max_total_children = 4
        source = ng.ProgramSource.from_javascript("host-teardown.js", '''
            export function define() {
              const graph = ng.graph("main");
              graph.channel("value", {reducer: "overwrite", initial: 0});
              graph.node("work", {type: "teardown_pending_node"});
              graph.entry("work");
              graph.exit("work");
              return graph;
            }
            export function* main(input) {
              return yield ng.callCore("main", input, "teardown:main");
            }
        ''')
        host = ng.LocalProgramHost(builder.build(), "teardown-owner", budget)
        version = host.compile_admit(source, budget)
        handle = host.start(version, {}, budget)
        assert entered.wait(5), "Python node never entered"
        del host
        gc.collect()
        assert finished.is_set(), "Python worker could not finish during host drain"
        result = handle.wait()
        assert result.run_id == handle.run_id
        assert result.status in {ng.ProgramTerminalStatus.Completed, ng.ProgramTerminalStatus.Cancelled}
    """)


@pytest.mark.parametrize("timeout_ms,error", [
    (-1, OverflowError), (2**63 - 1, ValueError), (2**64 - 1, ValueError),
])
def test_metadata_rejects_unrepresentable_constructor_deadline(timeout_ms, error):
    with pytest.raises(error):
        ng.RunMetadata(timeout_ms=timeout_ms)


@pytest.mark.parametrize("timeout_ms,error", [
    (-1, OverflowError), (2**63 - 1, ValueError), (2**64 - 1, ValueError),
])
@pytest.mark.parametrize("existing_timeout_ms", [0, 60000])
def test_failed_metadata_setter_preserves_existing_execution_deadline(
        provider_peer, timeout_ms, error, existing_timeout_ms):
    metadata = ng.RunMetadata(timeout_ms=existing_timeout_ms)
    with pytest.raises(error):
        metadata.set_timeout_ms(timeout_ms)
    engine = ng.GraphEngine.compile({
        "name": "metadata-provider-deadline", "schema_version": 1,
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"model": {"type": "llm_call"}},
        "edges": [{"from": ng.START_NODE, "to": "model"},
                  {"from": "model", "to": ng.END_NODE}],
    }, ng.NodeContext(provider=provider_peer.provider(), model="local-model"))
    config = ng.RunConfig(
        thread_id="metadata-provider-deadline",
        input={"messages": [{"role": "user", "content": "honour existing deadline"}]},
    )
    outcomes = ng.ProviderOutcomes()
    config.provider_outcomes = outcomes
    if existing_timeout_ms == 0:
        with pytest.raises(RuntimeError):
            engine.run(config, metadata)
        outcome, = outcomes.snapshot()
        assert outcome.completion is None
        assert outcome.failure.error.kind == ng.ProviderErrorKind.DeadlineExceeded
        assert provider_peer.requests == []
    else:
        result = engine.run(config, metadata)
        assert result.output["channels"]["messages"]["value"][-1]["content"] == "ok"
        outcome, = outcomes.snapshot()
        assert outcome.failure is None
        assert outcome.text == "ok"
        assert len(provider_peer.requests) == 1


def test_policy_identity_is_stable_binary_digest():
    builtin = ng.builtin_provider_policy()
    builtin_identity = builtin.identity
    assert isinstance(builtin_identity, bytes)
    assert len(builtin_identity) == 32
    assert builtin.identity == builtin_identity
    family_json = ng.provider_policy_json()
    resource_json = ng.provider_codec_defaults_json()
    custom = ng.load_provider_policy(family_json, resource_json)
    again = ng.load_provider_policy(family_json, resource_json)
    assert isinstance(custom.identity, bytes)
    assert len(custom.identity) == 32
    assert custom.identity == again.identity
